package shm

import (
	"bytes"
	"math/rand"
	"runtime"
	"strconv"
	"testing"
)

// These tests pin the SPEC §3.3.4 per-chunk running-length guard on the
// OUT-OF-ORDER path. The guard used to live only on the in-order branch of
// streamContext.consume, so a peer could advertise a tiny totalSize and then
// park unbounded bytes under ahead-of-cursor chunk indices: the byte-sum was
// only checked once index 0 arrived, i.e. after every parked copy was already
// resident. The rejection must happen at arrival time, and — the part that is
// easy to break while fixing it — a well-formed out-of-order stream whose sum
// equals totalSize exactly must still complete.

// chunkStep is one STREAM_CHUNK delivery and its expected reply.
type chunkStep struct {
	idx  uint32
	size int
	want MsgType
}

// TestStreamReassembly_OutOfOrderRunningLengthOverflow rejects an
// ahead-of-cursor chunk at park time as soon as the running received length
// (assembled bytes + parked bytes) would exceed the advertised totalSize.
func TestStreamReassembly_OutOfOrderRunningLengthOverflow(t *testing.T) {
	cases := []struct {
		name        string
		totalSize   uint64
		totalChunks uint32
		steps       []chunkStep
	}{
		{
			// The reported scenario in miniature: totalSize is 1 byte but the
			// header advertises 1000 chunks, and every chunk arrives ahead of
			// the cursor so the in-order guard never runs. The very first
			// chunk must be refused; the stream is then gone, so the next one
			// is refused too.
			name:        "first_ooo_chunk_overflows",
			totalSize:   1,
			totalChunks: 1000,
			steps: []chunkStep{
				{idx: 1, size: 4096, want: MsgTypeSystemError},
				{idx: 2, size: 4096, want: MsgTypeSystemError},
			},
		},
		{
			// No single parked chunk overflows; their sum does. This is the
			// case a per-chunk-only (non-accumulating) guard would miss.
			name:        "accumulated_ooo_chunks_overflow",
			totalSize:   8,
			totalChunks: 4,
			steps: []chunkStep{
				{idx: 3, size: 4, want: MsgTypeNormal},
				{idx: 2, size: 4, want: MsgTypeNormal},
				{idx: 1, size: 1, want: MsgTypeSystemError},
			},
		},
		{
			// Parked bytes must also constrain the in-order branch: chunk 0
			// fits on its own, but not once the already-parked tail is counted.
			name:        "in_order_chunk_overflows_against_parked_bytes",
			totalSize:   8,
			totalChunks: 3,
			steps: []chunkStep{
				{idx: 2, size: 6, want: MsgTypeNormal},
				{idx: 0, size: 6, want: MsgTypeSystemError},
			},
		},
		{
			// A zero-length parked chunk contributes nothing, so the guard must
			// not fire on it; the following oversized one must.
			name:        "zero_length_park_then_overflow",
			totalSize:   4,
			totalChunks: 3,
			steps: []chunkStep{
				{idx: 2, size: 0, want: MsgTypeNormal},
				{idx: 1, size: 5, want: MsgTypeSystemError},
			},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			var delivered int
			handler := NewStreamReassembler(func(uint64, []byte) { delivered++ }, nil)
			if _, mt := handler(streamStartReq(42, tc.totalSize, tc.totalChunks), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
				t.Fatalf("StreamStart rejected: %v", mt)
			}
			for i, st := range tc.steps {
				payload := make([]byte, st.size)
				if _, mt := handler(streamChunkReq(42, st.idx, payload), nil, MsgTypeStreamChunk); mt != st.want {
					t.Fatalf("step %d (chunk %d, %d bytes): got %v, want %v", i, st.idx, st.size, mt, st.want)
				}
			}
			if delivered != 0 {
				t.Errorf("corrupt stream delivered %d times, want 0", delivered)
			}
		})
	}
}

// TestStreamReassembly_OutOfOrderOverflowBoundsMemory is the residency half of
// the guard. A peer advertises totalSize=1024 with totalChunks=64 and then
// streams 512 KiB chunks under ahead-of-cursor indices only. Before the fix
// every one of them was copied into the side map (~32 MiB for this scaled-down
// case) and the stream was rejected only when index 0 finally arrived.
// TotalAlloc is cumulative and GC-independent, so it observes those copies even
// after they become garbage.
func TestStreamReassembly_OutOfOrderOverflowBoundsMemory(t *testing.T) {
	const chunkLen = 512 << 10
	const totalChunks = 64
	const advertised = 1024

	payload := make([]byte, chunkLen)
	reqs := make([][]byte, 0, totalChunks-1)
	for i := 1; i < totalChunks; i++ {
		reqs = append(reqs, streamChunkReq(7, uint32(i), payload))
	}

	handler := NewStreamReassembler(func(uint64, []byte) {
		t.Error("over-advertised stream must never be delivered")
	}, nil)
	if _, mt := handler(streamStartReq(7, advertised, totalChunks), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("StreamStart rejected: %v", mt)
	}

	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	for i, req := range reqs {
		if _, mt := handler(req, nil, MsgTypeStreamChunk); mt != MsgTypeSystemError {
			t.Fatalf("ooo chunk %d: got %v, want SystemError", i+1, mt)
		}
	}
	runtime.ReadMemStats(&after)

	if grew := after.TotalAlloc - before.TotalAlloc; grew > 1<<20 {
		t.Errorf("reassembler allocated %d bytes rejecting a stream that advertised %d; want < 1 MiB — ahead-of-cursor chunks are being parked before the totalSize guard runs",
			grew, advertised)
	}
}

// TestStreamReassembly_OutOfOrderPatternsComplete is the no-regression half:
// well-formed streams whose byte-sum equals totalSize exactly must still
// complete no matter what order the chunks arrive in. The chunk sizes are
// deliberately uneven (and include a zero-length one) so the running offset
// never lines up with index*fixedSize — an ooo byte counter that is not
// decremented when a parked chunk is drained then shows up here as a spurious
// SystemError instead of as merely misplaced bytes.
func TestStreamReassembly_OutOfOrderPatternsComplete(t *testing.T) {
	sizes := []int{7, 1, 64, 0, 13, 255, 2, 100}
	n := uint32(len(sizes))

	var total uint64
	want := make([]byte, 0, 512)
	payloads := make([][]byte, n)
	for i, sz := range sizes {
		p := bytes.Repeat([]byte{byte('A' + i)}, sz)
		payloads[i] = p
		want = append(want, p...)
		total += uint64(sz)
	}

	reverse := make([]uint32, 0, n)
	for i := int(n) - 1; i >= 0; i-- {
		reverse = append(reverse, uint32(i))
	}
	oddEven := make([]uint32, 0, n)
	for i := uint32(1); i < n; i += 2 {
		oddEven = append(oddEven, i)
	}
	for i := uint32(0); i < n; i += 2 {
		oddEven = append(oddEven, i)
	}
	// "drain then park again": one gap is filled (draining the parked tail)
	// before further ahead-of-cursor chunks are parked. If the drain forgets to
	// release the parked byte count, the second park double-counts and the
	// stream is wrongly rejected.
	drainThenPark := []uint32{1, 0, 3, 2, 5, 4, 7, 6}

	orders := map[string][]uint32{
		"reverse":         reverse,
		"odd_then_even":   oddEven,
		"drain_then_park": drainThenPark,
	}
	for _, seed := range []int64{1, 7, 20260729} {
		perm := rand.New(rand.NewSource(seed)).Perm(int(n))
		order := make([]uint32, 0, n)
		for _, i := range perm {
			order = append(order, uint32(i))
		}
		orders["shuffled_seed_"+strconv.FormatInt(seed, 10)] = order
	}

	for name, order := range orders {
		t.Run(name, func(t *testing.T) {
			var delivered [][]byte
			handler := NewStreamReassembler(func(_ uint64, data []byte) {
				delivered = append(delivered, data)
			}, nil)
			if _, mt := handler(streamStartReq(99, total, n), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
				t.Fatalf("StreamStart rejected: %v", mt)
			}
			for step, idx := range order {
				if _, mt := handler(streamChunkReq(99, idx, payloads[idx]), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
					t.Fatalf("step %d (chunk %d of order %v): got %v, want Normal — a valid out-of-order stream was rejected", step, idx, order, mt)
				}
			}
			if len(delivered) != 1 {
				t.Fatalf("delivered %d times, want exactly 1 (order %v)", len(delivered), order)
			}
			if !bytes.Equal(delivered[0], want) {
				t.Errorf("assembled bytes wrong for order %v", order)
			}
		})
	}
}

// TestStreamReassembly_OutOfOrderExactFillAccepted covers the > vs >= boundary:
// a parked chunk that consumes the whole remaining advertised space must be
// accepted, both when it is parked first and when it lands last.
func TestStreamReassembly_OutOfOrderExactFillAccepted(t *testing.T) {
	cases := []struct {
		name        string
		totalSize   uint64
		totalChunks uint32
		sizes       []int
		order       []uint32
	}{
		{"parked_chunk_fills_everything", 4, 2, []int{0, 4}, []uint32{1, 0}},
		{"in_order_chunk_fills_everything", 4, 2, []int{4, 0}, []uint32{1, 0}},
		{"reverse_exact_fill", 6, 3, []int{2, 2, 2}, []uint32{2, 1, 0}},
		{"single_chunk_exact_fill", 3, 1, []int{3}, []uint32{0}},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			payloads := make([][]byte, len(tc.sizes))
			want := make([]byte, 0, tc.totalSize)
			for i, sz := range tc.sizes {
				payloads[i] = bytes.Repeat([]byte{byte('a' + i)}, sz)
				want = append(want, payloads[i]...)
			}
			var delivered [][]byte
			handler := NewStreamReassembler(func(_ uint64, data []byte) {
				delivered = append(delivered, data)
			}, nil)
			if _, mt := handler(streamStartReq(5, tc.totalSize, tc.totalChunks), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
				t.Fatalf("StreamStart rejected: %v", mt)
			}
			for _, idx := range tc.order {
				if _, mt := handler(streamChunkReq(5, idx, payloads[idx]), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
					t.Fatalf("chunk %d rejected: %v — exact fill must not trip the overflow guard", idx, mt)
				}
			}
			if len(delivered) != 1 || !bytes.Equal(delivered[0], want) {
				t.Errorf("delivered %q, want exactly one %q", delivered, want)
			}
		})
	}
}
