package shm

import (
	"runtime"
	"sync"
	"sync/atomic"
	"unsafe"
)

const (
	BlockMagicData  = 0xAB12CD34
	BlockMagicPad   = 0xAB12CD35
	BlockHeaderSize = 16
	QueueHeaderSize = 128
)

// Corresponds to the C++ layout in include/IPCUtils.h
// struct QueueHeader {
//     std::atomic<uint64_t> writePos;     // 8 bytes
//     std::atomic<uint64_t> readPos;      // 8 bytes
//     std::atomic<uint64_t> capacity;     // 8 bytes
//     std::atomic<uint32_t> consumerActive; // 4 bytes (0=Sleeping, 1=Active)
//     uint32_t _pad1;                     // 4 bytes
//     uint8_t _pad2[96];                  // Padding to 128 bytes
// };
type QueueHeader struct {
	WritePos        uint64   // 8 bytes
	_pad1           [56]byte // 56 bytes
	ReadPos         uint64   // 8 bytes
	Capacity        uint64   // 8 bytes
	ConsumerActive  uint32   // 4 bytes
	_pad2           [44]byte // 44 bytes
}

type SPSCQueue struct {
	Header *QueueHeader
	Buffer []byte
	Event  EventHandle
	Pool   *sync.Pool
}

func NewSPSCQueue(shmBase uintptr, capacity uint64, event EventHandle) *SPSCQueue {
	return &SPSCQueue{
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
func (q *SPSCQueue) Recycle(msgs []*[]byte) {
	for _, m := range msgs {
		if m != nil {
			*m = (*m)[:0] // Reset length
			q.Pool.Put(m)
		}
	}
}

// Enqueue blocks if full.
func (q *SPSCQueue) Enqueue(data []byte, msgId uint32) {
	// Align total size to 8 bytes
	alignedSize := (len(data) + 7) & ^7
	totalSize := uint64(alignedSize + BlockHeaderSize)
	cap := atomic.LoadUint64(&q.Header.Capacity)

	for {
		wPos := atomic.LoadUint64(&q.Header.WritePos)
		rPos := atomic.LoadUint64(&q.Header.ReadPos)

		used := wPos - rPos
		if used+totalSize > cap {
			// Full - Spin/Yield
			runtime.Gosched()
			continue
		}

		offset := wPos % cap
		spaceToEnd := cap - offset

		// Check wrapping
		if spaceToEnd < totalSize {
			if spaceToEnd < BlockHeaderSize {
				// Too small for header, skip
				atomic.StoreUint64(&q.Header.WritePos, wPos+spaceToEnd)
				continue
			} else {
				// Write Padding Header
				ptr := unsafe.Pointer(&q.Buffer[offset])
				*(*uint32)(ptr) = uint32(spaceToEnd) - BlockHeaderSize
				// msgId=0, padding
				*(*uint32)(unsafe.Pointer(uintptr(ptr) + 4)) = 0

				magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 8))
				atomic.StoreUint32(magicPtr, BlockMagicPad)

				atomic.StoreUint64(&q.Header.WritePos, wPos+spaceToEnd)
				continue
			}
		}

		// Fits
		ptr := unsafe.Pointer(&q.Buffer[offset])
		// size
		*(*uint32)(ptr) = uint32(len(data))
		// msgId
		*(*uint32)(unsafe.Pointer(uintptr(ptr) + 4)) = msgId

		if len(data) > 0 {
			copy(q.Buffer[offset+BlockHeaderSize:], data)
		}

		magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 8))
		atomic.StoreUint32(magicPtr, BlockMagicData)

		atomic.StoreUint64(&q.Header.WritePos, wPos+totalSize)

		// Barrier: StoreLoad
		if atomic.AddUint32(&q.Header.ConsumerActive, 0) == 0 {
			SignalEvent(q.Event)
		}
		return
	}
}

// EnqueueBatch writes multiple messages.
// Assumes MsgId = 0 (Normal).
func (q *SPSCQueue) EnqueueBatch(msgs [][]byte) {
	if len(msgs) == 0 {
		return
	}

	cap := atomic.LoadUint64(&q.Header.Capacity)
	currentIdx := 0

	for currentIdx < len(msgs) {
		wPos := atomic.LoadUint64(&q.Header.WritePos)
		rPos := atomic.LoadUint64(&q.Header.ReadPos)

		tempWPos := wPos
		writtenAny := false

		for ; currentIdx < len(msgs); currentIdx++ {
			data := msgs[currentIdx]
			alignedSize := (len(data) + 7) & ^7
			totalSize := uint64(alignedSize + BlockHeaderSize)

			// Check capacity
			if (tempWPos-rPos)+totalSize > cap {
				if !writtenAny {
					// Full
					runtime.Gosched()
					// Refresh rPos
					rPos = atomic.LoadUint64(&q.Header.ReadPos)
					currentIdx-- // Retry this message
					break
				} else {
					break
				}
			}

			offset := tempWPos % cap
			spaceToEnd := cap - offset

			// Check wrapping
			if spaceToEnd < totalSize {
				if spaceToEnd < BlockHeaderSize {
					tempWPos += spaceToEnd
					currentIdx--
					continue
				} else {
					// Padding
					ptr := unsafe.Pointer(&q.Buffer[offset])
					*(*uint32)(ptr) = uint32(spaceToEnd) - BlockHeaderSize
					*(*uint32)(unsafe.Pointer(uintptr(ptr) + 4)) = 0 // msgId=0

					magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 8))
					atomic.StoreUint32(magicPtr, BlockMagicPad)

					tempWPos += spaceToEnd
					currentIdx--
					continue
				}
			}

			// Write Data
			ptr := unsafe.Pointer(&q.Buffer[offset])
			*(*uint32)(ptr) = uint32(len(data))
			*(*uint32)(unsafe.Pointer(uintptr(ptr) + 4)) = 0 // msgId=0

			magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 8))
			atomic.StoreUint32(magicPtr, BlockMagicData)

			copy(q.Buffer[offset+BlockHeaderSize:], data)

			tempWPos += totalSize
			writtenAny = true
		}

		if tempWPos != wPos {
			atomic.StoreUint64(&q.Header.WritePos, tempWPos)
			// Barrier: StoreLoad
			if atomic.AddUint32(&q.Header.ConsumerActive, 0) == 0 {
				SignalEvent(q.Event)
			}
		}
	}
}

// Dequeue blocks if empty
// Returns (data, msgId)
func (q *SPSCQueue) Dequeue() ([]byte, uint32) {
	// Active = 1
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
			// Waiting = 0
			atomic.StoreUint32(&q.Header.ConsumerActive, 0)

			// Barrier
			atomic.AddUint32(&q.Header.ConsumerActive, 0)

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
		magic := atomic.LoadUint32(magicPtr)

		if magic == BlockMagicPad {
			size := *(*uint32)(ptr)
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

		atomic.StoreUint64(&q.Header.ReadPos, rPos+nextRPosDiff)
		return data, msgId
	}
}

// DequeueBatch reads multiple messages using pooled buffers.
// Only returns data. Ignores msgId (assumes normal data).
func (q *SPSCQueue) DequeueBatch(maxCount int) []*[]byte {
	if maxCount == 0 {
		return nil
	}

	// Active = 1
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

			// Waiting = 0
			atomic.StoreUint32(&q.Header.ConsumerActive, 0)

			// Barrier
			atomic.AddUint32(&q.Header.ConsumerActive, 0)

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
			magic := atomic.LoadUint32(magicPtr)

			if magic == BlockMagicPad {
				size := *(*uint32)(ptr)
				rPos += uint64(size) + BlockHeaderSize
				continue
			}

			// Data
			size := *(*uint32)(ptr)
			// msgId is ignored for batch
			alignedSize := (size + 7) & ^uint32(7)
			nextRPosDiff := uint64(alignedSize + BlockHeaderSize)

			// Get buffer from pool
			bufPtr := q.Pool.Get().(*[]byte)
			// Ensure capacity
			if c := cap(*bufPtr); uint32(c) < size {
				*bufPtr = make([]byte, size)
			} else {
				*bufPtr = (*bufPtr)[:size]
			}

			copy(*bufPtr, q.Buffer[offset+BlockHeaderSize:offset+BlockHeaderSize+uint64(size)])
			outMsgs = append(outMsgs, bufPtr)

			rPos += nextRPosDiff
		}

		atomic.StoreUint64(&q.Header.ReadPos, rPos)
		return outMsgs
	}
}
