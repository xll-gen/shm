package shm

import (
	"runtime"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"
)

// MPSCQueue matches the C++ MPSCQueue layout.
// It uses CAS for Enqueue and checks Magic for Dequeue.
type MPSCQueue struct {
	Header *QueueHeader
	Buffer []byte
	Event  EventHandle
	Pool   *sync.Pool
}

func NewMPSCQueue(shmBase uintptr, capacity uint64, event EventHandle) *MPSCQueue {
	return &MPSCQueue{
		Header: (*QueueHeader)(unsafe.Pointer(shmBase)),
		Buffer: unsafe.Slice((*byte)(unsafe.Pointer(shmBase + QueueHeaderSize)), capacity),
		Event:  event,
		Pool: &sync.Pool{
			New: func() any {
				b := make([]byte, 0, 1024)
				return &b
			},
		},
	}
}

// Recycle returns a slice of byte pointers to the pool.
func (q *MPSCQueue) Recycle(msgs []*[]byte) {
	for _, m := range msgs {
		if m != nil {
			*m = (*m)[:0] // Reset length
			q.Pool.Put(m)
		}
	}
}

// Enqueue blocks if full (CAS loop).
func (q *MPSCQueue) Enqueue(data []byte, msgId uint32) {
	alignedSize := (len(data) + 7) & ^7
	totalSize := uint64(alignedSize + BlockHeaderSize)
	cap := atomic.LoadUint64(&q.Header.Capacity)

	for {
		wPos := atomic.LoadUint64(&q.Header.WritePos)
		rPos := atomic.LoadUint64(&q.Header.ReadPos)

		used := wPos - rPos
		if used+totalSize > cap {
			// Full - Yield/Sleep
			runtime.Gosched()
			continue
		}

		offset := wPos % cap
		spaceToEnd := cap - offset

		// Check wrapping
		if spaceToEnd < totalSize {
			if spaceToEnd < BlockHeaderSize {
				// Too small, skip
				if atomic.CompareAndSwapUint64(&q.Header.WritePos, wPos, wPos+spaceToEnd) {
					continue
				}
			} else {
				// Try to reserve PAD
				if atomic.CompareAndSwapUint64(&q.Header.WritePos, wPos, wPos+spaceToEnd) {
					// Write Padding
					ptr := unsafe.Pointer(&q.Buffer[offset])
					*(*uint32)(ptr) = uint32(spaceToEnd) - BlockHeaderSize
					*(*uint32)(unsafe.Pointer(uintptr(ptr) + 4)) = 0

					magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 8))
					atomic.StoreUint32(magicPtr, BlockMagicPad)

					continue
				}
			}
		} else {
			// Fits
			if atomic.CompareAndSwapUint64(&q.Header.WritePos, wPos, wPos+totalSize) {
				// Write Data
				ptr := unsafe.Pointer(&q.Buffer[offset])
				*(*uint32)(ptr) = uint32(len(data))
				*(*uint32)(unsafe.Pointer(uintptr(ptr) + 4)) = msgId

				if len(data) > 0 {
					copy(q.Buffer[offset+BlockHeaderSize:], data)
				}

				magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 8))
				atomic.StoreUint32(magicPtr, BlockMagicData)

				// Barrier: StoreLoad
				if atomic.AddUint32(&q.Header.ConsumerActive, 0) == 0 {
					SignalEvent(q.Event)
				}
				return
			}
		}
		// CAS failed
		runtime.Gosched()
	}
}

// EnqueueBatch writes multiple messages.
// MPSC implementation must do one by one or complex reservation.
// We do one by one for correctness and simplicity in MPSC.
func (q *MPSCQueue) EnqueueBatch(msgs [][]byte) {
	for _, data := range msgs {
		q.Enqueue(data, 0)
	}
}

// Dequeue blocks if empty.
// Returns (data, msgId)
func (q *MPSCQueue) Dequeue() ([]byte, uint32) {
	atomic.StoreUint32(&q.Header.ConsumerActive, 1)
	spinCount := 0

	for {
		rPos := atomic.LoadUint64(&q.Header.ReadPos)
		wPos := atomic.LoadUint64(&q.Header.WritePos)

		if rPos == wPos {
			if spinCount < 4000 {
				spinCount++
				continue
			}
			atomic.StoreUint32(&q.Header.ConsumerActive, 0)
			atomic.AddUint32(&q.Header.ConsumerActive, 0) // Barrier

			if atomic.LoadUint64(&q.Header.ReadPos) != atomic.LoadUint64(&q.Header.WritePos) {
				atomic.StoreUint32(&q.Header.ConsumerActive, 1)
				continue
			}

			WaitForEvent(q.Event, 100)
			atomic.StoreUint32(&q.Header.ConsumerActive, 1)
			spinCount = 0
			continue
		}

		cap := atomic.LoadUint64(&q.Header.Capacity)
		offset := rPos % cap
		spaceToEnd := cap - offset

		if spaceToEnd < BlockHeaderSize {
			atomic.StoreUint64(&q.Header.ReadPos, rPos+spaceToEnd)
			continue
		}

		ptr := unsafe.Pointer(&q.Buffer[offset])
		magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 8))

		// Spin on Magic
		magicSpin := 0
		var magic uint32
		for {
			magic = atomic.LoadUint32(magicPtr)
			if magic != 0 {
				break
			}
			if magicSpin > 10000 {
				time.Sleep(time.Microsecond)
			}
			magicSpin++
		}

		if magic == BlockMagicPad {
			size := *(*uint32)(ptr)
			// Clear Magic
			atomic.StoreUint32(magicPtr, 0)
			atomic.StoreUint64(&q.Header.ReadPos, rPos+uint64(size+BlockHeaderSize))
			continue
		}

		// Data
		size := *(*uint32)(ptr)
		msgId := *(*uint32)(unsafe.Pointer(uintptr(ptr) + 4))

		alignedSize := (size + 7) & ^uint32(7)
		nextRPosDiff := uint64(alignedSize + BlockHeaderSize)

		data := make([]byte, size)
		if size > 0 {
			copy(data, q.Buffer[offset+BlockHeaderSize:offset+BlockHeaderSize+uint64(size)])
		}

		// Clear Magic
		atomic.StoreUint32(magicPtr, 0)

		atomic.StoreUint64(&q.Header.ReadPos, rPos+nextRPosDiff)
		return data, msgId
	}
}

// DequeueBatch reads multiple messages using pooled buffers.
func (q *MPSCQueue) DequeueBatch(maxCount int) []*[]byte {
	if maxCount == 0 {
		return nil
	}

	atomic.StoreUint32(&q.Header.ConsumerActive, 1)
	var outMsgs []*[]byte
	spinCount := 0

	for {
		rPos := atomic.LoadUint64(&q.Header.ReadPos)
		wPos := atomic.LoadUint64(&q.Header.WritePos)

		if rPos == wPos {
			if len(outMsgs) > 0 {
				return outMsgs
			}
			if spinCount < 4000 {
				spinCount++
				continue
			}

			atomic.StoreUint32(&q.Header.ConsumerActive, 0)
			atomic.AddUint32(&q.Header.ConsumerActive, 0) // Barrier

			if atomic.LoadUint64(&q.Header.ReadPos) != atomic.LoadUint64(&q.Header.WritePos) {
				atomic.StoreUint32(&q.Header.ConsumerActive, 1)
				continue
			}

			WaitForEvent(q.Event, 100)
			atomic.StoreUint32(&q.Header.ConsumerActive, 1)
			spinCount = 0
			continue
		}

		capacity := atomic.LoadUint64(&q.Header.Capacity)

		for len(outMsgs) < maxCount && rPos != wPos {
			offset := rPos % capacity
			spaceToEnd := capacity - offset

			if spaceToEnd < BlockHeaderSize {
				rPos += spaceToEnd
				continue
			}

			ptr := unsafe.Pointer(&q.Buffer[offset])
			magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 8))

			// Spin on Magic
			magicSpin := 0
			var magic uint32
			for {
				magic = atomic.LoadUint32(magicPtr)
				if magic != 0 {
					break
				}
				if magicSpin > 10000 {
					time.Sleep(time.Microsecond)
				}
				magicSpin++
			}

			if magic == BlockMagicPad {
				size := *(*uint32)(ptr)
				atomic.StoreUint32(magicPtr, 0)
				rPos += uint64(size) + BlockHeaderSize
				continue
			}

			size := *(*uint32)(ptr)
			alignedSize := (size + 7) & ^uint32(7)
			nextRPosDiff := uint64(alignedSize + BlockHeaderSize)

			bufPtr := q.Pool.Get().(*[]byte)
			if c := cap(*bufPtr); uint32(c) < size {
				*bufPtr = make([]byte, size)
			} else {
				*bufPtr = (*bufPtr)[:size]
			}

			copy(*bufPtr, q.Buffer[offset+BlockHeaderSize:offset+BlockHeaderSize+uint64(size)])
			outMsgs = append(outMsgs, bufPtr)

			atomic.StoreUint32(magicPtr, 0)

			rPos += nextRPosDiff
		}

		atomic.StoreUint64(&q.Header.ReadPos, rPos)
		return outMsgs
	}
}
