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

type MPSCQueue struct {
	Header *QueueHeader
	Buffer []byte
	Event  EventHandle
}

func NewMPSCQueue(shmBase uintptr, capacity uint64, event EventHandle) *MPSCQueue {
	return &MPSCQueue{
		Header: (*QueueHeader)(unsafe.Pointer(shmBase)),
		Buffer: unsafe.Slice((*byte)(unsafe.Pointer(shmBase + QueueHeaderSize)), capacity),
		Event:  event,
	}
}

func (q *MPSCQueue) Enqueue(data []byte) bool {
	// Align total size to 8 bytes
	alignedSize := (len(data) + 7) & ^7
	totalSize := uint64(alignedSize + BlockHeaderSize)
	cap := atomic.LoadUint64(&q.Header.Capacity)

	for {
		wPos := atomic.LoadUint64(&q.Header.WritePos)
		rPos := atomic.LoadUint64(&q.Header.ReadPos)

		used := wPos - rPos
		if used+totalSize > cap {
			return false // Full
		}

		offset := wPos % cap
		spaceToEnd := cap - offset

		// Check wrapping
		if spaceToEnd < totalSize {
			if spaceToEnd < BlockHeaderSize {
				// Too small for header, skip
				if atomic.CompareAndSwapUint64(&q.Header.WritePos, wPos, wPos+spaceToEnd) {
					continue
				}
			} else {
				// Wrap needed. Reserve padding.
				if atomic.CompareAndSwapUint64(&q.Header.WritePos, wPos, wPos+spaceToEnd) {
					// Write Padding Header
					ptr := unsafe.Pointer(&q.Buffer[offset])
					// Size
					*(*uint32)(ptr) = uint32(spaceToEnd) - BlockHeaderSize
					// Magic (Release)
					magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 4))
					atomic.StoreUint32(magicPtr, BlockMagicPad)

					continue // Retry for data
				}
			}
		} else {
			// Fits
			if atomic.CompareAndSwapUint64(&q.Header.WritePos, wPos, wPos+totalSize) {
				ptr := unsafe.Pointer(&q.Buffer[offset])
				// Size (actual size)
				*(*uint32)(ptr) = uint32(len(data))

				// Copy Data
				copy(q.Buffer[offset+BlockHeaderSize:], data)

				// Magic (Release)
				magicPtr := (*uint32)(unsafe.Pointer(uintptr(ptr) + 4))
				atomic.StoreUint32(magicPtr, BlockMagicData)

				// Signal
				SignalEvent(q.Event)
				return true
			}
		}
		runtime.Gosched()
	}
}

func (q *MPSCQueue) Dequeue() ([]byte, bool) {
	rPos := atomic.LoadUint64(&q.Header.ReadPos)
	wPos := atomic.LoadUint64(&q.Header.WritePos)

	if rPos == wPos {
		return nil, false
	}

	cap := atomic.LoadUint64(&q.Header.Capacity)
	offset := rPos % cap
	spaceToEnd := cap - offset

	if spaceToEnd < BlockHeaderSize {
		// Small tail skip
		atomic.StoreUint64(&q.Header.ReadPos, rPos+spaceToEnd)
		return q.Dequeue()
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

	var nextRPosDiff uint64

	if magic == BlockMagicPad {
		// PAD
		nextRPosDiff = uint64(size + BlockHeaderSize)

		// Clear magic
		atomic.StoreUint32(magicPtr, 0)
		// Advance ReadPos
		atomic.StoreUint64(&q.Header.ReadPos, rPos+nextRPosDiff)
		// Recurse
		return q.Dequeue()
	}

	// Data
	// Align
	alignedSize := (size + 7) & ^uint32(7)
	nextRPosDiff = uint64(alignedSize + BlockHeaderSize)

	data := make([]byte, size)
	copy(data, q.Buffer[offset+BlockHeaderSize : offset+BlockHeaderSize+uint64(size)])

	// Clear magic
	atomic.StoreUint32(magicPtr, 0)
	// Advance ReadPos
	atomic.StoreUint64(&q.Header.ReadPos, rPos+nextRPosDiff)

	return data, true
}
