package shm

import (
	"sync/atomic"
	"unsafe"
)

// RingBufferHeader matches the C++ struct layout in
// include/shm/RingBufferUtils.h (256 bytes: two cache-line-padded offsets).
type RingBufferHeader struct {
	WriteOffset uint64 // Offset 0
	_pad1       [120]byte
	ReadOffset  uint64 // Offset 128
	_pad2       [120]byte
}

// Compile-time size assertion against the C++ static_assert (== 256).
var (
	_ [256 - unsafe.Sizeof(RingBufferHeader{})]byte
	_ [unsafe.Sizeof(RingBufferHeader{}) - 256]byte
)

// RingBufferReceiver reads a single-producer/single-consumer byte ring laid
// out as [RingBufferHeader (256B)][data]. It is bootstrapped over a slot
// request buffer by BootstrapRingBuffer.
type RingBufferReceiver struct {
	header   *RingBufferHeader
	data     []byte
	capacity uint32
}

// BootstrapRingBuffer overlays a RingBufferHeader + data region onto req.
// Returns (nil, false) when req is unusable:
//   - shorter than the 256-byte header plus at least one data byte
//     (a zero-capacity ring would divide by zero in Read), or
//   - not 8-byte aligned (WriteOffset/ReadOffset are accessed with 64-bit
//     atomics, which require natural alignment).
func BootstrapRingBuffer(req []byte) (*RingBufferReceiver, bool) {
	const headerSize = 256
	if len(req) <= headerSize {
		return nil, false
	}
	if uintptr(unsafe.Pointer(&req[0]))%8 != 0 {
		return nil, false
	}

	header := (*RingBufferHeader)(unsafe.Pointer(&req[0]))
	dataBuf := req[headerSize:]

	return &RingBufferReceiver{
		header:   header,
		data:     dataBuf,
		capacity: uint32(len(dataBuf)),
	}, true
}

// Read copies up to len(out) available bytes out of the ring and advances
// ReadOffset. Returns the number of bytes read (0 when the ring is empty).
func (rb *RingBufferReceiver) Read(out []byte) int {
	w := atomic.LoadUint64(&rb.header.WriteOffset)
	r := atomic.LoadUint64(&rb.header.ReadOffset)

	available := w - r
	if available == 0 {
		return 0
	}

	toRead := uint64(len(out))
	if available < toRead {
		toRead = available
	}

	readIdx := uint32(r % uint64(rb.capacity))
	contiguous := uint32(rb.capacity) - readIdx
	chunk := uint32(toRead)
	if chunk > contiguous {
		chunk = contiguous
	}

	copy(out[:chunk], rb.data[readIdx:readIdx+chunk])
	if chunk < uint32(toRead) {
		copy(out[chunk:], rb.data[0:uint32(toRead)-chunk])
	}

	atomic.StoreUint64(&rb.header.ReadOffset, r+toRead)
	return int(toRead)
}
