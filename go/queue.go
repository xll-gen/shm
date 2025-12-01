package shm

import (
	"runtime"
	"sync/atomic"
	"unsafe"
)

const (
	BlockMagicData  = 0xDA7A0001
	BlockMagicPad   = 0xDA7A0002
	BlockHeaderSize = 8
	QueueHeaderSize = 128
)

// Corresponds to the C++ layout
type QueueHeader struct {
	WritePos uint64   // 8 bytes
	_pad1    [56]byte // 56 bytes
	ReadPos  uint64   // 8 bytes
	Capacity uint64   // 8 bytes
	_pad2    [48]byte // 48 bytes
}

type SPSCQueue struct {
	Header *QueueHeader
	Buffer []byte
	Event  EventHandle
}

func NewSPSCQueue(shmBase uintptr, capacity uint64, event EventHandle) *SPSCQueue {
	return &SPSCQueue{
		Header: (*QueueHeader)(unsafe.Pointer(shmBase)),
		Buffer: unsafe.Slice((*byte)(unsafe.Pointer(shmBase + QueueHeaderSize)), capacity),
		Event:  event,
	}
}

// Enqueue blocks if full
func (q *SPSCQueue) Enqueue(data []byte) {
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
				magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 4))
				atomic.StoreUint32(magicPtr, BlockMagicPad) // Release

				atomic.StoreUint64(&q.Header.WritePos, wPos+spaceToEnd)
				continue
			}
		}

		// Fits
		ptr := unsafe.Pointer(&q.Buffer[offset])
		*(*uint32)(ptr) = uint32(len(data))
		copy(q.Buffer[offset+BlockHeaderSize:], data)

		magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 4))
		atomic.StoreUint32(magicPtr, BlockMagicData) // Release

		atomic.StoreUint64(&q.Header.WritePos, wPos+totalSize) // Release

		SignalEvent(q.Event)
		return
	}
}

// Dequeue blocks if empty
func (q *SPSCQueue) Dequeue() []byte {
	spinCount := 0
	for {
		rPos := atomic.LoadUint64(&q.Header.ReadPos)
		wPos := atomic.LoadUint64(&q.Header.WritePos)

		if rPos == wPos {
			if spinCount < 4000 {
				spinCount++
				continue
			}
			WaitForEvent(q.Event, 100)
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
		magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 4))

		// Wait for magic
		spin := 0
		for {
			magic := atomic.LoadUint32(magicPtr)
			if magic != 0 {
				break
			}
			spin++
			if spin > 1000 {
				runtime.Gosched()
			}
		}

		magic := atomic.LoadUint32(magicPtr)
		size := *(*uint32)(ptr)

		if magic == BlockMagicPad {
			// Clear magic
			atomic.StoreUint32(magicPtr, 0)
			// Advance ReadPos
			atomic.StoreUint64(&q.Header.ReadPos, rPos+uint64(size+BlockHeaderSize))
			continue
		}

		// Data
		alignedSize := (size + 7) & ^uint32(7)
		nextRPosDiff := uint64(alignedSize + BlockHeaderSize)

		data := make([]byte, size)
		copy(data, q.Buffer[offset+BlockHeaderSize:offset+BlockHeaderSize+uint64(size)])

		// Clear magic
		atomic.StoreUint32(magicPtr, 0)
		// Advance ReadPos
		atomic.StoreUint64(&q.Header.ReadPos, rPos+nextRPosDiff)

		return data
	}
}
