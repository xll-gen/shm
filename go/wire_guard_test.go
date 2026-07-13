package shm

import (
	"bytes"
	"encoding/binary"
	"testing"
)

// TestStreamChunk_HostilePayloadSizeRejected pins the unsigned-arithmetic
// bounds check on the chunk path: a PayloadSize >= 2^31 with no matching
// payload bytes must be rejected as SystemError. On 32-bit builds a signed
// int conversion of such a value goes negative and would slip past a
// `len(req) < headerLen+int(payloadSize)` comparison, panicking at the
// payload slice expression.
func TestStreamChunk_HostilePayloadSizeRejected(t *testing.T) {
	handler := NewStreamReassembler(func(uint64, []byte) {}, nil)

	handler(streamStartReq(42, 4, 2), nil, MsgTypeStreamStart)

	buf := new(bytes.Buffer)
	binary.Write(buf, binary.LittleEndian, ChunkHeader{
		StreamID:    42,
		ChunkIndex:  0,
		PayloadSize: 1<<31 + 16, // hostile: header claims 2GiB+, no payload follows
	})
	if _, mt := handler(buf.Bytes(), nil, MsgTypeStreamChunk); mt != MsgTypeSystemError {
		t.Fatalf("hostile PayloadSize: got %v, want SystemError", mt)
	}
}

// TestRingBufferRead_CorruptWriteOffsetRejected pins the wire-boundary clamp
// in RingBufferReceiver.Read: WriteOffset is peer-provided, and a corrupt
// value implying more unread bytes than the ring's capacity must yield 0
// instead of driving the wrap copy past len(data).
func TestRingBufferRead_CorruptWriteOffsetRejected(t *testing.T) {
	const capacity = 64
	req := make([]byte, 256+capacity)
	rb, ok := BootstrapRingBuffer(req)
	if !ok {
		t.Skip("test buffer not 8-byte aligned")
	}

	rb.header.WriteOffset = 1000 // corrupt: far beyond capacity
	rb.header.ReadOffset = 0

	out := make([]byte, 200)
	if n := rb.Read(out); n != 0 {
		t.Fatalf("corrupt WriteOffset: Read returned %d, want 0", n)
	}
}
