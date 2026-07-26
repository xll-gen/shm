package shm

import (
	"bytes"
	"runtime"
	"sync"
	"sync/atomic"
	"testing"
	"time"
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

// The tests below pin the 2026-07-25 per-stream locking split: the reassembler
// map mutex no longer covers the chunk payload copy or the StreamStart
// allocation, so each stream carries its own mutex plus done/dead markers and
// the map entry is retired with a compare-and-delete. They are written to fail
// under `-race` if that discipline regresses, and to fail deterministically if
// the SPEC §3.3.4 exactly-once/completion invariants do.

// TestStreamReassembly_ConcurrentSameStreamChunks drives the chunks of ONE
// stream from several goroutines at once — the shape a multi-slot host worker
// pool produces for a pipelined sender — with each worker owning an interleaved
// subset of the chunk indices, so every worker is ahead of the in-order cursor
// part of the time. Every stream must still deliver exactly once with its bytes
// in index order.
func TestStreamReassembly_ConcurrentSameStreamChunks(t *testing.T) {
	const numStreams = 8
	const chunksPerStream = 64
	const workersPerStream = 4
	const chunkLen = 512

	payload := func(s, c int) []byte {
		return bytes.Repeat([]byte{byte(s), byte(c)}, chunkLen/2)
	}

	var mu sync.Mutex
	delivered := make(map[uint64]int)
	assembled := make(map[uint64][]byte)
	handler := NewStreamReassembler(func(streamID uint64, data []byte) {
		mu.Lock()
		delivered[streamID]++
		assembled[streamID] = data
		mu.Unlock()
	}, nil)

	var wg sync.WaitGroup
	for s := 0; s < numStreams; s++ {
		id := uint64(5000 + s)
		if _, mt := handler(streamStartReq(id, chunksPerStream*chunkLen, chunksPerStream), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
			t.Fatalf("stream %d: start rejected: %v", id, mt)
		}
		for w := 0; w < workersPerStream; w++ {
			wg.Add(1)
			go func(s, w int) {
				defer wg.Done()
				for c := w; c < chunksPerStream; c += workersPerStream {
					if _, mt := handler(streamChunkReq(uint64(5000+s), uint32(c), payload(s, c)), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
						t.Errorf("stream %d chunk %d rejected: %v", 5000+s, c, mt)
						return
					}
				}
			}(s, w)
		}
	}
	wg.Wait()

	for s := 0; s < numStreams; s++ {
		id := uint64(5000 + s)
		if delivered[id] != 1 {
			t.Errorf("stream %d delivered %d times, want exactly 1", id, delivered[id])
			continue
		}
		want := make([]byte, 0, chunksPerStream*chunkLen)
		for c := 0; c < chunksPerStream; c++ {
			want = append(want, payload(s, c)...)
		}
		if !bytes.Equal(assembled[id], want) {
			t.Errorf("stream %d: assembled bytes wrong (chunk landed at the wrong offset?)", id)
		}
	}
}

// TestStreamReassembly_ConcurrentRestartKeepsFreshContext pins the
// compare-and-delete on retirement. A StreamStart that reuses a live stream ID
// installs a fresh context; a chunk that was already in flight against the
// replaced context may finish afterwards, and its retirement must NOT remove
// the fresh entry. Without the identity check the fresh context vanishes and
// the next chunk for that ID is answered SystemError.
func TestStreamReassembly_ConcurrentRestartKeepsFreshContext(t *testing.T) {
	const id = 4242
	const iters = 400
	// The chunks are deliberately large. The completing chunk holds the finishing
	// context's lock for the length of a 1 MiB copy, which is what lets the
	// concurrent restart install its replacement *before* the finishing chunk
	// reaches retirement — precisely the interleaving the identity check exists
	// for. With 4-byte chunks the window is too short to hit.
	const chunkLen = 1 << 20
	const totalSize = 2 * chunkLen

	payload := bytes.Repeat([]byte{0xA5}, chunkLen)

	var deliveries int32
	handler := NewStreamReassembler(func(streamID uint64, data []byte) {
		if streamID != id {
			t.Errorf("delivered unexpected streamID %d", streamID)
		}
		if len(data) != totalSize {
			t.Errorf("delivered %d bytes, want %d (partial buffer handed out?)", len(data), totalSize)
		}
		atomic.AddInt32(&deliveries, 1)
	}, nil)

	for i := 0; i < iters; i++ {
		if _, mt := handler(streamStartReq(id, totalSize, 2), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
			t.Fatalf("iter %d: start rejected: %v", i, mt)
		}
		if _, mt := handler(streamChunkReq(id, 0, payload), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("iter %d: chunk0 rejected: %v", i, mt)
		}

		// Race the completing chunk of the current context against a restart of
		// the same ID. Both requests are built up front so the goroutines race on
		// the handler, not on request marshalling.
		final := streamChunkReq(id, 1, payload)
		restart := streamStartReq(id, totalSize, 2)
		var wg sync.WaitGroup
		wg.Add(2)
		go func() {
			defer wg.Done()
			handler(final, nil, MsgTypeStreamChunk)
		}()
		go func() {
			defer wg.Done()
			runtime.Gosched()
			handler(restart, nil, MsgTypeStreamStart)
		}()
		wg.Wait()

		// Whichever way the race resolved, the restart's context is the live map
		// entry, so a chunk for it must be accepted. (The restart is the last
		// StreamStart for this ID; only a stale retirement could have removed it.)
		if _, mt := handler(streamChunkReq(id, 0, payload), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("iter %d: chunk for the restarted stream got %v, want Normal — a stale completion deleted the fresh context", i, mt)
		}
		// Leftover state is irrelevant: the next iteration's StreamStart replaces it.
	}

	if n := atomic.LoadInt32(&deliveries); n == 0 {
		t.Error("nothing ever completed — the test never reached the retirement path")
	}
}

// TestStreamReassembly_PruneRacesChunkConsumption runs the age-based prune (and
// the memory it frees) concurrently with chunk consumption on the streams it is
// reclaiming. A reclaimed context must stop absorbing bytes and must never
// deliver, so every stream ID delivers at most once and only ever with its full
// advertised size — a context that kept consuming after being pruned would show
// up as a second delivery or a short buffer.
func TestStreamReassembly_PruneRacesChunkConsumption(t *testing.T) {
	const numWorkers = 4
	const streamsPerWorker = 200
	const timeout = time.Millisecond

	var nanos int64
	clock := func() time.Time { return time.Unix(0, atomic.LoadInt64(&nanos)) }

	var mu sync.Mutex
	delivered := make(map[uint64]int)
	handler := newStreamReassembler(func(streamID uint64, data []byte) {
		if len(data) != 4 {
			t.Errorf("stream %d delivered %d bytes, want 4", streamID, len(data))
		}
		if !bytes.Equal(data, []byte("aabb")) {
			t.Errorf("stream %d delivered %q, want \"aabb\"", streamID, data)
		}
		mu.Lock()
		delivered[streamID]++
		mu.Unlock()
	}, nil, timeout, clock)

	var wg sync.WaitGroup
	for w := 0; w < numWorkers; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			for i := 0; i < streamsPerWorker; i++ {
				id := uint64(w)<<32 | uint64(i)
				if _, mt := handler(streamStartReq(id, 4, 2), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
					t.Errorf("stream %d: start rejected: %v", id, mt)
					return
				}
				// Push the virtual clock forward so other workers' StreamStarts
				// prune streams (possibly this one) mid-flight.
				atomic.AddInt64(&nanos, int64(timeout))
				// A prune may have reclaimed this stream between the two chunks;
				// SystemError is then the correct answer, not a failure.
				handler(streamChunkReq(id, 0, []byte("aa")), nil, MsgTypeStreamChunk)
				handler(streamChunkReq(id, 1, []byte("bb")), nil, MsgTypeStreamChunk)
			}
		}(w)
	}
	wg.Wait()

	mu.Lock()
	defer mu.Unlock()
	for id, n := range delivered {
		if n != 1 {
			t.Errorf("stream %d delivered %d times, want at most 1 (onStream must fire exactly once)", id, n)
		}
	}
	if len(delivered) == 0 {
		t.Fatal("no stream completed at all — the prune starved every stream, test is not exercising the race")
	}
}
