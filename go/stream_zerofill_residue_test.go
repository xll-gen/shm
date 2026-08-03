package shm

import (
	"bytes"
	"testing"
	"time"
)

// The 2026-08-03 zero-fill investigation rests on one claim: a delivered stream
// has had EVERY byte of its destination overwritten, so the zeroing done by
// make([]byte, totalSize) contributes nothing observable and could in principle
// be skipped (by recycling a buffer instead of allocating one). The claim is a
// property of the delivery conditions, not of the zero-fill:
//
//   - a chunk is only written after fits() and inBounds(), so nothing lands
//     outside [0, totalSize);
//   - an in-order chunk must carry dstOffset == the running cursor, so the
//     written region is contiguous from 0;
//   - delivery requires BOTH next == totalChunks AND offset == totalSize, so
//     that contiguous region is the whole buffer.
//
// Drop the second half of that conjunction and a stream that advertises more
// bytes than it sends is delivered with whatever the tail happened to hold.
// Today that is zeros; the moment a buffer is recycled it is the previous
// stream's payload. These tests pin the property with a POISONED destination —
// the only way to observe it, because the production allocator hides the bug
// behind its own zeroing. newBuf (see newStreamReassemblerTypedPeek) is the
// benchmark seam that supplies it.
const residuePoison = 0xAA

// newPoisonedReassembler builds a reassembler whose destination buffers arrive
// pre-filled with residuePoison, standing in for a recycled buffer.
func newPoisonedReassembler(t *testing.T, onStream StreamHandler) func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
	t.Helper()
	handler, _ := newStreamReassemblerTypedPeek(
		func(streamID uint64, _ uint32, data []byte) { onStream(streamID, data) },
		nil, DefaultStreamTimeout, time.Now,
		func(n int) []byte {
			buf := make([]byte, n)
			for i := range buf {
				buf[i] = residuePoison
			}
			return buf
		},
	)
	return handler
}

// TestStreamDeliveredBytesCarryNoResidue drives a complete stream into a
// poisoned destination and requires the delivered bytes to be exactly the
// payload — no poison byte survives anywhere, including the short final chunk.
func TestStreamDeliveredBytesCarryNoResidue(t *testing.T) {
	const totalSize = 10_000
	const maxPayload = 1024
	totalChunks := (totalSize + maxPayload - 1) / maxPayload

	want := make([]byte, totalSize)
	for i := range want {
		want[i] = byte(i%251) + 1 // never 0, never the poison
	}

	var got []byte
	var calls int
	handler := newPoisonedReassembler(t, func(_ uint64, data []byte) {
		calls++
		got = append([]byte(nil), data...)
	})

	start := make([]byte, streamHeaderSize)
	putStreamHeader(start, 7, totalSize, uint32(totalChunks), 0)
	if _, mt := handler(start, nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("StreamStart rejected: %v", mt)
	}
	req := make([]byte, chunkHeaderSize+maxPayload)
	for c, off := 0, 0; off < totalSize; c++ {
		n := maxPayload
		if off+n > totalSize {
			n = totalSize - off
		}
		putChunkHeader(req, 7, uint32(c), uint32(n), uint32(off))
		copy(req[chunkHeaderSize:], want[off:off+n])
		if _, mt := handler(req[:chunkHeaderSize+n], nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("chunk %d rejected: %v", c, mt)
		}
		off += n
	}

	if calls != 1 {
		t.Fatalf("onStream called %d times, want 1", calls)
	}
	if !bytes.Equal(got, want) {
		t.Fatalf("delivered bytes differ from payload (residue leak): first mismatch at %d", firstDiff(got, want))
	}
}

// TestStreamShortStreamIsNeverDelivered covers the conjunction itself:
// totalChunks chunks arrive but sum to less than the advertised totalSize, so
// next == totalChunks holds and offset == totalSize does not, and the stream
// must be dropped.
//
// That the stream is dropped is ALREADY covered (stream_parity_table_test.go
// "undersize_completion_drops", TestStreamReassembly_SizeMismatchUndersized) —
// both fail if the second conjunct is deleted. What only this test can show is
// WHAT would be delivered: those tests run on production-allocated buffers, so
// the leaked tail is zeros and reads like harmless truncation. On a recycled
// buffer it is the previous stream's payload, and that is the difference
// between "truncated" and "cross-stream data disclosure".
func TestStreamShortStreamIsNeverDelivered(t *testing.T) {
	const totalSize = 4096
	const sent = 1000 // one chunk, short of totalSize

	var delivered [][]byte
	handler := newPoisonedReassembler(t, func(_ uint64, data []byte) {
		delivered = append(delivered, append([]byte(nil), data...))
	})

	start := make([]byte, streamHeaderSize)
	putStreamHeader(start, 9, totalSize, 1, 0)
	if _, mt := handler(start, nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("StreamStart rejected: %v", mt)
	}

	req := make([]byte, chunkHeaderSize+sent)
	putChunkHeader(req, 9, 0, sent, 0)
	for i := chunkHeaderSize; i < len(req); i++ {
		req[i] = 1
	}
	_, mt := handler(req, nil, MsgTypeStreamChunk)

	// The delivery check comes first: it is the one that names the actual
	// hazard when this regresses.
	for _, d := range delivered {
		if i := bytes.IndexByte(d, residuePoison); i >= 0 {
			t.Fatalf("short stream was delivered carrying %d bytes of residual destination content (first at offset %d) — on a recycled buffer that is the previous stream's payload", len(d)-i, i)
		}
		t.Fatalf("short stream was delivered (%d bytes) though its chunks sum to %d of %d", len(d), sent, totalSize)
	}
	if mt != MsgTypeSystemError {
		t.Fatalf("short stream answered %v, want MsgTypeSystemError (it must be dropped, not delivered)", mt)
	}
}

func firstDiff(a, b []byte) int {
	for i := range a {
		if i >= len(b) || a[i] != b[i] {
			return i
		}
	}
	return len(a)
}
