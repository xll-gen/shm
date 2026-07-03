package shm

import (
	"bytes"
	"sync"
	"testing"
)

// These tests pin the 2026-07-03 direct-into-destination reassembler: chunks
// are consumed straight into a preallocated buffer at an in-order cursor,
// with ahead-of-cursor chunks parked in a side buffer. Delivery order,
// dedup, and the SPEC §3.3.4 guards must all behave identically to the old
// per-chunk-allocation layout.

func reassembleWith(t *testing.T, totalSize uint64, totalChunks uint32, deliver func(handler func(req []byte, resp []byte, msgType MsgType) (int32, MsgType))) (delivered [][]byte) {
	t.Helper()
	handler := NewStreamReassembler(func(streamID uint64, data []byte) {
		delivered = append(delivered, data)
	}, nil)
	if _, mt := handler(streamStartReq(42, totalSize, totalChunks), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("StreamStart rejected: %v", mt)
	}
	deliver(handler)
	return delivered
}

// TestStreamReassembly_OutOfOrder delivers chunks ahead of the in-order
// cursor (as a pipelined sender can) and verifies the assembled bytes land at
// the correct offsets.
func TestStreamReassembly_OutOfOrder(t *testing.T) {
	c0, c1, c2 := []byte("aaaa"), []byte("bbbb"), []byte("cc")
	want := []byte("aaaabbbbcc")

	for name, order := range map[string][]struct {
		idx     uint32
		payload []byte
	}{
		"1-0-2": {{1, c1}, {0, c0}, {2, c2}},
		"2-1-0": {{2, c2}, {1, c1}, {0, c0}},
	} {
		delivered := reassembleWith(t, uint64(len(want)), 3, func(handler func([]byte, []byte, MsgType) (int32, MsgType)) {
			for _, ch := range order {
				if _, mt := handler(streamChunkReq(42, ch.idx, ch.payload), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
					t.Fatalf("%s: chunk %d rejected: %v", name, ch.idx, mt)
				}
			}
		})
		if len(delivered) != 1 || !bytes.Equal(delivered[0], want) {
			t.Errorf("%s: delivered %q, want exactly one %q", name, delivered, want)
		}
	}
}

// TestStreamReassembly_DuplicateChunks re-delivers both an already-consumed
// chunk and an already-parked out-of-order chunk. Duplicates must be ACKed
// without consuming (each index filled exactly once) and the stream must
// still assemble correctly.
func TestStreamReassembly_DuplicateChunks(t *testing.T) {
	c0, c1, c2 := []byte("xx"), []byte("yy"), []byte("zz")
	delivered := reassembleWith(t, 6, 3, func(handler func([]byte, []byte, MsgType) (int32, MsgType)) {
		steps := []struct {
			idx     uint32
			payload []byte
		}{
			{0, c0},
			{0, c0}, // duplicate of a consumed chunk
			{2, c2}, // parked ahead of cursor
			{2, c2}, // duplicate of a parked chunk
			{1, c1}, // fills the gap, drains chunk 2
		}
		for i, ch := range steps {
			if _, mt := handler(streamChunkReq(42, ch.idx, ch.payload), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
				t.Fatalf("step %d (chunk %d): rejected: %v", i, ch.idx, mt)
			}
		}
	})
	if len(delivered) != 1 || !bytes.Equal(delivered[0], []byte("xxyyzz")) {
		t.Errorf("delivered %q, want exactly one %q", delivered, "xxyyzz")
	}
}

// TestStreamReassembly_ZeroLengthChunk verifies a zero-length chunk is
// deduplicated by map presence (not slice nil-ness) and counted exactly once.
func TestStreamReassembly_ZeroLengthChunk(t *testing.T) {
	delivered := reassembleWith(t, 2, 2, func(handler func([]byte, []byte, MsgType) (int32, MsgType)) {
		if _, mt := handler(streamChunkReq(42, 1, nil), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("zero-length parked chunk rejected: %v", mt)
		}
		if _, mt := handler(streamChunkReq(42, 1, nil), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("duplicate zero-length chunk rejected: %v", mt)
		}
		if _, mt := handler(streamChunkReq(42, 0, []byte("ok")), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("final chunk rejected: %v", mt)
		}
	})
	if len(delivered) != 1 || !bytes.Equal(delivered[0], []byte("ok")) {
		t.Errorf("delivered %q, want exactly one %q", delivered, "ok")
	}
}

// TestStreamReassembly_RunningLengthOverflow trips the SPEC §3.3.4 per-chunk
// running-length guard on the in-order path: the stream is dropped with
// SystemError and later chunks for it are rejected.
func TestStreamReassembly_RunningLengthOverflow(t *testing.T) {
	delivered := reassembleWith(t, 4, 2, func(handler func([]byte, []byte, MsgType) (int32, MsgType)) {
		if _, mt := handler(streamChunkReq(42, 0, []byte("aaa")), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("chunk 0 rejected: %v", mt)
		}
		if _, mt := handler(streamChunkReq(42, 1, []byte("bbb")), nil, MsgTypeStreamChunk); mt != MsgTypeSystemError {
			t.Fatalf("overflowing chunk 1: got %v, want SystemError", mt)
		}
		// Stream must be gone now.
		if _, mt := handler(streamChunkReq(42, 1, []byte("b")), nil, MsgTypeStreamChunk); mt != MsgTypeSystemError {
			t.Fatalf("chunk for dropped stream: got %v, want SystemError", mt)
		}
	})
	if len(delivered) != 0 {
		t.Errorf("corrupt stream delivered: %q", delivered)
	}
}

// TestStreamReassembly_DrainOverflow trips the running-length guard on the
// out-of-order drain path (the parked chunk, not the directly-delivered one,
// overflows).
func TestStreamReassembly_DrainOverflow(t *testing.T) {
	delivered := reassembleWith(t, 4, 2, func(handler func([]byte, []byte, MsgType) (int32, MsgType)) {
		if _, mt := handler(streamChunkReq(42, 1, []byte("bbb")), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("parked chunk 1 rejected: %v", mt)
		}
		if _, mt := handler(streamChunkReq(42, 0, []byte("aaa")), nil, MsgTypeStreamChunk); mt != MsgTypeSystemError {
			t.Fatalf("gap fill draining overflowing chunk: got %v, want SystemError", mt)
		}
	})
	if len(delivered) != 0 {
		t.Errorf("corrupt stream delivered: %q", delivered)
	}
}

// TestStreamReassembly_SizeMismatchUndersized rejects a stream whose chunk
// byte-sum falls short of the advertised TotalSize (Σ payloadSize !=
// totalSize at completion).
func TestStreamReassembly_SizeMismatchUndersized(t *testing.T) {
	delivered := reassembleWith(t, 10, 2, func(handler func([]byte, []byte, MsgType) (int32, MsgType)) {
		if _, mt := handler(streamChunkReq(42, 0, []byte("aaa")), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("chunk 0 rejected: %v", mt)
		}
		if _, mt := handler(streamChunkReq(42, 1, []byte("bbb")), nil, MsgTypeStreamChunk); mt != MsgTypeSystemError {
			t.Fatalf("undersized completion: got %v, want SystemError", mt)
		}
	})
	if len(delivered) != 0 {
		t.Errorf("undersized stream delivered: %q", delivered)
	}
}

// TestStreamReassembly_ConcurrentStreams drives one reassembler handler from
// multiple goroutines with interleaved stream IDs (the shape a multi-slot
// worker pool produces) and verifies per-stream buf/cursor/ooo isolation:
// every stream must deliver exactly once with its own bytes. Run under -race
// this also pins the mutex discipline of the direct-into-destination path.
func TestStreamReassembly_ConcurrentStreams(t *testing.T) {
	const numStreams = 16
	const chunksPerStream = 8
	const chunkLen = 32

	var mu sync.Mutex
	delivered := make(map[uint64][]byte)
	handler := NewStreamReassembler(func(streamID uint64, data []byte) {
		mu.Lock()
		delivered[streamID] = data
		mu.Unlock()
	}, nil)

	var wg sync.WaitGroup
	for s := 0; s < numStreams; s++ {
		wg.Add(1)
		go func(s int) {
			defer wg.Done()
			id := uint64(1000 + s)
			total := uint64(chunksPerStream * chunkLen)
			if _, mt := handler(streamStartReq(id, total, chunksPerStream), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
				t.Errorf("stream %d: start rejected: %v", id, mt)
				return
			}
			for c := 0; c < chunksPerStream; c++ {
				payload := bytes.Repeat([]byte{byte(s), byte(c)}, chunkLen/2)
				if _, mt := handler(streamChunkReq(id, uint32(c), payload), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
					t.Errorf("stream %d chunk %d rejected: %v", id, c, mt)
					return
				}
			}
		}(s)
	}
	wg.Wait()

	if len(delivered) != numStreams {
		t.Fatalf("delivered %d streams, want %d", len(delivered), numStreams)
	}
	for s := 0; s < numStreams; s++ {
		id := uint64(1000 + s)
		want := make([]byte, 0, chunksPerStream*chunkLen)
		for c := 0; c < chunksPerStream; c++ {
			want = append(want, bytes.Repeat([]byte{byte(s), byte(c)}, chunkLen/2)...)
		}
		if !bytes.Equal(delivered[id], want) {
			t.Errorf("stream %d: delivered bytes corrupted (cross-stream bleed?)", id)
		}
	}
}
