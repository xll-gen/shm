package shm

import (
	"fmt"
	"sync/atomic"
	"testing"
)

// Micro-benchmarks for the streaming path. They exist to A/B the 2026-07-25
// changes and are cheap enough to keep as regression guards:
//
//   - BenchmarkStreamReassembler_StreamStart — allocation count of the
//     StreamStart parse (manual little-endian decode vs. reflective binary.Read
//     over a bytes.Reader).
//   - BenchmarkStreamReassembler_ConcurrentChunks — multi-stream concurrent
//     chunk ingestion, the case the reassembler-wide mutex used to serialize.
//   - BenchmarkStreamSender_Send — end-to-end guest-side send against a
//     spinning mock host, covering the maxInFlight==1 inline path and the
//     maxInFlight>1 pipelined path.
//
// The C++<->Go stream profile in benchmarks/harness.ps1 (run_stream.sh cells)
// remains the authoritative A/B for wall-clock stream throughput; these only
// isolate the Go-side costs.

// BenchmarkStreamReassembler_StreamStart measures the StreamStart handler in
// isolation. Run with -benchmem: the header decode itself must allocate nothing,
// so the reported allocs are exactly the streamContext plus its destination
// buffer (2/op).
func BenchmarkStreamReassembler_StreamStart(b *testing.B) {
	handler := NewStreamReassembler(func(uint64, []byte) {}, nil)
	req := streamStartReq(1, 64, 1)
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		// A distinct ID per iteration would grow the map to the LRU bound and
		// benchmark eviction instead; reusing one ID measures the parse plus the
		// context install (the same-ID replace path).
		if _, mt := handler(req, nil, MsgTypeStreamStart); mt != MsgTypeNormal {
			b.Fatalf("StreamStart rejected: %v", mt)
		}
	}
}

// BenchmarkStreamReassembler_StreamStartEmpty isolates the header decode with no
// allocation of its own: totalChunks == 0 completes at StreamStart, so 0 allocs/op
// means the decode is allocation-free (the previous binary.Read path allocated a
// bytes.Reader here).
func BenchmarkStreamReassembler_StreamStartEmpty(b *testing.B) {
	handler := NewStreamReassembler(func(uint64, []byte) {}, nil)
	req := streamStartReq(1, 0, 0)
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		if _, mt := handler(req, nil, MsgTypeStreamStart); mt != MsgTypeNormal {
			b.Fatalf("StreamStart rejected: %v", mt)
		}
	}
}

// BenchmarkStreamReassembler_ConcurrentChunks ingests whole streams from every
// parallel worker at once, each worker on its own stream IDs — the multi-slot
// host-worker shape. Before per-stream locking, both the StreamStart allocation
// and every chunk's payload copy ran under one reassembler-wide mutex, so added
// workers could not add throughput. Reported as bytes/sec (b.SetBytes).
//
// Use -cpu=1,2,4,8 to see the scaling.
func BenchmarkStreamReassembler_ConcurrentChunks(b *testing.B) {
	const chunkLen = 64 << 10
	const chunksPerStream = 16
	const streamBytes = chunkLen * chunksPerStream

	handler := NewStreamReassembler(func(uint64, []byte) {}, nil)

	var idGen uint64
	b.SetBytes(streamBytes)
	b.ResetTimer()
	b.RunParallel(func(pb *testing.PB) {
		base := atomic.AddUint64(&idGen, 1) << 32
		payload := make([]byte, chunkLen)
		reqs := make([][]byte, chunksPerStream)
		var id uint64
		for pb.Next() {
			id++
			streamID := base | id
			if _, mt := handler(streamStartReq(streamID, streamBytes, chunksPerStream), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
				b.Errorf("StreamStart rejected: %v", mt)
				return
			}
			for c := 0; c < chunksPerStream; c++ {
				// Rebuilt per stream because the header carries the stream ID;
				// the slice itself is reused to keep the sender-side cost out of
				// the measurement.
				reqs[c] = streamChunkReqInto(reqs[c], streamID, uint32(c), payload)
				if _, mt := handler(reqs[c], nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
					b.Errorf("chunk %d rejected: %v", c, mt)
					return
				}
			}
		}
	})
}

// streamChunkReqInto builds a StreamChunk request into buf (grown as needed),
// avoiding a fresh allocation per benchmark iteration.
func streamChunkReqInto(buf []byte, streamID uint64, chunkIndex uint32, payload []byte) []byte {
	need := chunkHeaderSize + len(payload)
	if cap(buf) < need {
		buf = make([]byte, need)
	}
	buf = buf[:need]
	putChunkHeader(buf, streamID, chunkIndex, uint32(len(payload)))
	copy(buf[chunkHeaderSize:], payload)
	return buf
}

// benchStreamHost answers every guest-slot request with a Normal ACK, spinning
// instead of sleeping so the mock host's own latency does not dominate the
// sender measurement.
func benchStreamHost(env *zombieStealEnv, stop <-chan struct{}, done chan<- struct{}) {
	defer close(done)
	for {
		select {
		case <-stop:
			return
		default:
		}
		for k := 1; k <= env.numGuest; k++ {
			hdr := env.slotHeader(k)
			if atomic.LoadUint32(&hdr.State) != SlotReqReady {
				continue
			}
			if !atomic.CompareAndSwapUint32(&hdr.State, SlotReqReady, SlotBusy) {
				continue
			}
			hdr.RespSize = 0
			hdr.MsgType = MsgTypeNormal
			atomic.StoreUint32(&hdr.State, SlotRespReady)
			SignalEvent(env.respEvents[k])
		}
	}
}

// BenchmarkStreamSender_Send sends a multi-chunk payload against the spinning
// mock host. inFlight=1 exercises the inline chunk path (no per-chunk goroutine
// or semaphore handoff); inFlight=2 exercises the pipelined path unchanged.
func BenchmarkStreamSender_Send(b *testing.B) {
	for _, inFlight := range []int{1, 2} {
		b.Run(fmt.Sprintf("inFlight=%d", inFlight), func(b *testing.B) {
			env := newZombieStealEnv(b, fmt.Sprintf("StreamSendBench%d", inFlight), 2)
			defer env.Close()

			client, err := ConnectDefault(env.name)
			if err != nil {
				b.Fatalf("ConnectDefault failed: %v", err)
			}
			defer client.Close()

			stop := make(chan struct{})
			done := make(chan struct{})
			go benchStreamHost(env, stop, done)
			defer func() { close(stop); <-done }()

			sender := NewStreamSender(client, inFlight)
			data := make([]byte, 64<<10) // ~135 chunks at a 488-byte payload cap

			b.ReportAllocs()
			b.SetBytes(int64(len(data)))
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				if err := sender.Send(data, uint64(i)); err != nil {
					b.Fatalf("Send failed: %v", err)
				}
			}
		})
	}
}
