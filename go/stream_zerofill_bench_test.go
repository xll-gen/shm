package shm

import (
	"testing"
	"time"
)

// Profiling benchmarks for the backlog item "StreamStart zero-fill avoidance
// (opt-in buffer supplier)". They exist to answer ONE question before any
// optimization is written: what share of the stream RECEIVE path is the
// runtime's zeroing of the destination buffer allocated in StreamStart?
//
// Shape fidelity to the real path (benchmarks/harness.ps1 -Profile stream):
//
//   - chunk payload = 4096 - chunkHeaderSize, matching the harness's default
//     -c 4096 slot size;
//   - the chunk request buffer is REUSED across every chunk, because on the wire
//     `req` points at the peer's fixed slot request buffer — the copy source is
//     the same hot 4 KiB region for every chunk of every stream, not a cold
//     stream-sized source;
//   - a fresh stream ID per iteration, and each stream is driven to completion,
//     so the map is empty between iterations (completed streams retire).
//
// ANSWER (2026-08-03, Ryzen 9 3900X, go1.26.3, -cpu=1). CPU profile of
// BenchmarkStreamReceive, runtime.memclrNoHeapPointers flat share, 100 % of it
// reached via mallocgcLarge — i.e. the StreamStart make and nothing else:
//
//	64 KiB  23.5 %   1 MiB  33.2 %   16 MiB  46.4 %
//
// So the item's premise holds. What a buffer supplier could actually buy is
// BenchmarkStreamReceive vs BenchmarkStreamReceivePooledCold (medians of 5,
// same session): 8228 → 5457 ns (1.51x), 135.4 → 79.7 µs (1.70x), 2.204 →
// 1.286 ms (1.71x). The opt-in was still DECLINED — see EXPERIMENTS.md
// "2026-08-03 stream destination zero-fill" for the cost side.
//
// Sizes are the 64 KiB / 1 MiB / 16 MiB cells of the harness stream profile.
var zeroFillBenchSizes = []struct {
	name string
	size int
}{
	{"64KiB", 64 << 10},
	{"1MiB", 1 << 20},
	{"16MiB", 16 << 20},
}

const zeroFillBenchSlot = 4096

// BenchmarkStreamReceive drives whole streams through the reassembler, which is
// the population the memclr share must be measured against.
func BenchmarkStreamReceive(b *testing.B) {
	for _, tc := range zeroFillBenchSizes {
		b.Run(tc.name, func(b *testing.B) {
			maxPayload := zeroFillBenchSlot - chunkHeaderSize
			totalChunks := (tc.size + maxPayload - 1) / maxPayload

			var delivered int64
			handler := NewStreamReassembler(func(_ uint64, data []byte) {
				delivered += int64(len(data))
			}, nil)

			// Both request buffers are built with the production encoders and
			// reused, so the helper's reflective binary.Write does not land in
			// the profile as receive-path cost.
			req := make([]byte, zeroFillBenchSlot)
			start := make([]byte, streamHeaderSize)
			b.SetBytes(int64(tc.size))
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				streamID := uint64(i) + 1
				putStreamHeader(start, streamID, uint64(tc.size), uint32(totalChunks), 0)
				if _, mt := handler(start, nil, MsgTypeStreamStart); mt != MsgTypeNormal {
					b.Fatalf("StreamStart rejected: %v", mt)
				}
				off := 0
				for c := 0; c < totalChunks; c++ {
					n := maxPayload
					if off+n > tc.size {
						n = tc.size - off
					}
					putChunkHeader(req, streamID, uint32(c), uint32(n), uint32(off))
					if _, mt := handler(req[:chunkHeaderSize+n], nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
						b.Fatalf("chunk %d rejected: %v", c, mt)
					}
					off += n
				}
			}
			b.StopTimer()
			if delivered != int64(b.N)*int64(tc.size) {
				b.Fatalf("delivered %d bytes, want %d", delivered, int64(b.N)*int64(tc.size))
			}
		})
	}
}

// BenchmarkStreamReceivePooled is BenchmarkStreamReceive with the destination
// buffer supplied from a (degenerate, single-entry) pool instead of allocated
// per stream — the A/B twin that puts an upper bound on what an opt-in buffer
// supplier could buy. Everything else is byte-identical to the benchmark above.
//
// The single reused buffer is legitimate here only because each stream is
// driven to completion and delivered before the next StreamStart: at no point
// are two live streams sharing it.
func BenchmarkStreamReceivePooled(b *testing.B) {
	for _, tc := range zeroFillBenchSizes {
		b.Run(tc.name, func(b *testing.B) {
			maxPayload := zeroFillBenchSlot - chunkHeaderSize
			totalChunks := (tc.size + maxPayload - 1) / maxPayload

			var delivered int64
			pool := make([]byte, tc.size)
			handler, _ := newStreamReassemblerTypedPeek(
				func(_ uint64, _ uint32, data []byte) { delivered += int64(len(data)) },
				nil, DefaultStreamTimeout, time.Now,
				func(n int) []byte { return pool[:n] },
			)

			req := make([]byte, zeroFillBenchSlot)
			start := make([]byte, streamHeaderSize)
			b.SetBytes(int64(tc.size))
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				streamID := uint64(i) + 1
				putStreamHeader(start, streamID, uint64(tc.size), uint32(totalChunks), 0)
				if _, mt := handler(start, nil, MsgTypeStreamStart); mt != MsgTypeNormal {
					b.Fatalf("StreamStart rejected: %v", mt)
				}
				off := 0
				for c := 0; c < totalChunks; c++ {
					n := maxPayload
					if off+n > tc.size {
						n = tc.size - off
					}
					putChunkHeader(req, streamID, uint32(c), uint32(n), uint32(off))
					if _, mt := handler(req[:chunkHeaderSize+n], nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
						b.Fatalf("chunk %d rejected: %v", c, mt)
					}
					off += n
				}
			}
			b.StopTimer()
			if delivered != int64(b.N)*int64(tc.size) {
				b.Fatalf("delivered %d bytes, want %d", delivered, int64(b.N)*int64(tc.size))
			}
		})
	}
}

// BenchmarkStreamReceivePooledCold is the honest version of the pooled A/B: the
// supplier rotates over a ring of buffers whose total footprint (~128 MiB)
// exceeds this host's L3, so the destination is as cold as a freshly allocated
// one and the only thing removed is the zero-fill (plus the GC work the
// allocation caused). BenchmarkStreamReceivePooled's single hot buffer flatters
// the result by turning every payload copy into a cache hit.
func BenchmarkStreamReceivePooledCold(b *testing.B) {
	const ringBytes = 128 << 20
	for _, tc := range zeroFillBenchSizes {
		b.Run(tc.name, func(b *testing.B) {
			maxPayload := zeroFillBenchSlot - chunkHeaderSize
			totalChunks := (tc.size + maxPayload - 1) / maxPayload

			ring := make([][]byte, ringBytes/tc.size)
			for i := range ring {
				ring[i] = make([]byte, tc.size)
			}
			var ringIdx int

			var delivered int64
			handler, _ := newStreamReassemblerTypedPeek(
				func(_ uint64, _ uint32, data []byte) { delivered += int64(len(data)) },
				nil, DefaultStreamTimeout, time.Now,
				func(n int) []byte {
					buf := ring[ringIdx]
					ringIdx = (ringIdx + 1) % len(ring)
					return buf[:n]
				},
			)

			req := make([]byte, zeroFillBenchSlot)
			start := make([]byte, streamHeaderSize)
			b.SetBytes(int64(tc.size))
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				streamID := uint64(i) + 1
				putStreamHeader(start, streamID, uint64(tc.size), uint32(totalChunks), 0)
				if _, mt := handler(start, nil, MsgTypeStreamStart); mt != MsgTypeNormal {
					b.Fatalf("StreamStart rejected: %v", mt)
				}
				off := 0
				for c := 0; c < totalChunks; c++ {
					n := maxPayload
					if off+n > tc.size {
						n = tc.size - off
					}
					putChunkHeader(req, streamID, uint32(c), uint32(n), uint32(off))
					if _, mt := handler(req[:chunkHeaderSize+n], nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
						b.Fatalf("chunk %d rejected: %v", c, mt)
					}
					off += n
				}
			}
			b.StopTimer()
			if delivered != int64(b.N)*int64(tc.size) {
				b.Fatalf("delivered %d bytes, want %d", delivered, int64(b.N)*int64(tc.size))
			}
		})
	}
}

// BenchmarkStreamDestAlloc isolates the StreamStart allocation alone — the
// make([]byte, totalSize) an opt-in buffer supplier would replace — under the
// same allocate-and-drop GC pressure the receive benchmark creates. It is the
// UPPER BOUND on what the supplier can save: everything else in the receive
// path (header decode, per-chunk map/bookkeeping, payload copy) stays.
func BenchmarkStreamDestAlloc(b *testing.B) {
	for _, tc := range zeroFillBenchSizes {
		b.Run(tc.name, func(b *testing.B) {
			var sink []byte
			b.SetBytes(int64(tc.size))
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				sink = make([]byte, tc.size)
				sink[0] = byte(i)
			}
			_ = sink
		})
	}
}
