package shm

import (
	"sync/atomic"
	"unsafe"
)

// RingBufferHeader matches the C++ struct layout.
// alignas(128) implies 128-byte alignment for fields.
type RingBufferHeader struct {
	WriteOffset uint64 // Offset 0
	_pad1       [120]byte
	ReadOffset  uint64 // Offset 128
	_pad2       [120]byte
}

type RingBufferReceiver struct {
	slotIdx     int
	header      *RingBufferHeader
	data        []byte
	capacity    uint32
	guest       *DirectGuest
}

func NewRingBufferReceiver(g *DirectGuest, slotIdx int) (*RingBufferReceiver, error) {
	// Access the slot memory via Guest API (assuming we can access it)
    // The guest API doesn't expose raw memory directly easily.
    // We might need a helper or unsafe access.

    // For this specific feature, we'll assume we can get the buffer.
    // In `DirectGuest`, we have `slots []Slot`.
    // But `Slot` struct is internal in `direct.go`? No, it's not exported.

    // We need to extend `DirectGuest` or use `unsafe` to access memory.
    // Let's assume we add a method `GetSlotBuffer(idx)` to `DirectGuest`.
    // Or we use the `req` buffer passed to the handler?
    // Handler is callback-based.

    // If we want a Pull-based RingBuffer, we need a different API.
    // BUT the user asked for "Streaming".
    // Let's implement this as a helper that takes a buffer pointer.

    return nil, nil // Placeholder
}

// Since I cannot easily modify internal state of DirectGuest from here without changing core files,
// I will add a helper in `go/direct.go` or similar to expose Raw Slot Memory if needed.
// OR, I can use the standard Handler mechanism to "bootstrap" the RingBuffer.
// i.e. Host sends a "SWITCH_TO_RING" message. Guest Handler receives pointer, upgrades to Ring Mode.

func BootstrapRingBuffer(req []byte) (*RingBufferReceiver, bool) {
    if len(req) < 256 {
        return nil, false
    }

    header := (*RingBufferHeader)(unsafe.Pointer(&req[0]))
    dataBuf := req[256:]

    return &RingBufferReceiver{
        header: header,
        data: dataBuf,
        capacity: uint32(len(dataBuf)),
    }, true
}

func (rb *RingBufferReceiver) Read(out []byte) int {
    w := atomic.LoadUint64(&rb.header.WriteOffset) // LoadAcquire equivalent? Go memory model is strong.
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

    atomic.StoreUint64(&rb.header.ReadOffset, r + toRead)
    return int(toRead)
}
