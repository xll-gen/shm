package shm

import (
	"runtime"
	"testing"
)

// These tests pin the SPEC §3.3.4 parked-chunk bound — the *count* dimension of
// "per-stream reassembly residency is O(totalSize)".
//
// The v0.8.17 running-byte guard (streamContext.fits / oooBytes) bounds the
// payload a stream can make resident by its advertised totalSize. It cannot
// bound bookkeeping overhead, for two independent reasons:
//
//  1. Overhead exists at zero payload. A parked chunk costs a map entry
//     (key + slice header + control/tombstone overhead) regardless of length.
//  2. A zero-length chunk passes the byte guard by construction:
//     offset+oooBytes+0 <= totalSize is true whenever the stream is still
//     alive, so the guard can never fire on one.
//
// Together those let a peer advertise totalSize = 1 with totalChunks = 2^20 and
// then park 2^20-1 zero-length chunks — tens of MiB of map entries per stream,
// times MaxConcurrentStreams. MaxParkedChunks closes that dimension, and the
// C++ reassembler carries the same bound with the same default so a stream
// accepted by one peer is accepted by the other
// (include/shm/StreamReassembler.h: StreamReassemblerConfig::maxParkedChunks,
// tests/test_reassembler_park_cap.cpp).

// TestStreamReassembly_ParkedChunkCountBounded checks the boundary exactly:
// MaxParkedChunks ahead-of-cursor chunks are accepted, and the one after that
// is rejected with SystemError and drops the stream.
func TestStreamReassembly_ParkedChunkCountBounded(t *testing.T) {
	const streamID = 4242
	// totalChunks must exceed MaxParkedChunks for the bound to be reachable;
	// index 0 is never sent so the cursor never advances.
	totalChunks := uint32(MaxParkedChunks + 8)

	var delivered int
	handler := NewStreamReassembler(func(uint64, []byte) { delivered++ }, nil)
	if _, mt := handler(streamStartReq(streamID, 1, totalChunks), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("StreamStart rejected: %v", mt)
	}

	// Zero-length chunks: they pass the byte guard by construction, so this
	// isolates the count bound from the byte bound.
	empty := []byte{}
	for i := 1; i <= MaxParkedChunks; i++ {
		if _, mt := handler(streamChunkReq(streamID, uint32(i), empty), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("parked chunk %d/%d rejected (%v); the bound must admit exactly MaxParkedChunks entries", i, MaxParkedChunks, mt)
		}
	}
	if _, mt := handler(streamChunkReq(streamID, uint32(MaxParkedChunks+1), empty), nil, MsgTypeStreamChunk); mt != MsgTypeSystemError {
		t.Fatalf("chunk %d past the parked bound: got %v, want SystemError", MaxParkedChunks+1, mt)
	}
	// Dropped, so any further chunk is answered as an unknown stream.
	if _, mt := handler(streamChunkReq(streamID, 1, empty), nil, MsgTypeStreamChunk); mt != MsgTypeSystemError {
		t.Fatalf("chunk after the bound tripped: got %v, want SystemError (stream must be dropped)", mt)
	}
	if delivered != 0 {
		t.Errorf("stream delivered %d times, want 0", delivered)
	}
}

// TestStreamReassembly_ParkedChunkCountBoundedResidency is the residency half.
// A peer advertises one byte with the maximum chunk count and floods
// zero-length ahead-of-cursor chunks. Every one of them slips through the byte
// guard, so before MaxParkedChunks existed the side map grew to ~2^20 entries
// (measured ~57 MiB live for this scenario) for a stream that advertised a
// single byte. The live heap after the flood must stay small.
func TestStreamReassembly_ParkedChunkCountBoundedResidency(t *testing.T) {
	const streamID = 777
	totalChunks := uint32(MaxStreamChunks)

	handler := NewStreamReassembler(func(uint64, []byte) {
		t.Error("over-advertised stream must never be delivered")
	}, nil)
	if _, mt := handler(streamStartReq(streamID, 1, totalChunks), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("StreamStart rejected: %v", mt)
	}

	req := streamChunkReq(streamID, 0, []byte{})

	var before, after runtime.MemStats
	runtime.GC()
	runtime.ReadMemStats(&before)

	// Indices 1..totalChunks-1: never index 0, so the cursor never advances and
	// nothing is ever drained.
	for i := uint32(1); i < totalChunks; i++ {
		// Every chunk is zero-length, so a conforming sender puts dstOffset 0 on
		// all of them -- the offsets are the running sum of prior sizes, and that
		// sum is 0 throughout. This test is about the COUNT bound, not placement.
		putChunkHeader(req, streamID, i, 0, 0)
		handler(req, nil, MsgTypeStreamChunk)
	}

	runtime.GC()
	runtime.ReadMemStats(&after)
	runtime.KeepAlive(handler)

	grew := int64(after.HeapAlloc) - int64(before.HeapAlloc)
	if grew < 0 {
		grew = 0
	}
	t.Logf("live heap after flooding %d zero-length ooo chunks: %d bytes", totalChunks-1, grew)

	// Budget: 1 MiB. With the bound in place the map holds at most
	// MaxParkedChunks (1024) empty entries, tens of KiB.
	const budget = 1 << 20
	if grew > budget {
		t.Errorf("reassembler retained %d bytes of parked-chunk bookkeeping for a stream advertising 1 byte; want < %d — the parked-entry count is unbounded",
			grew, budget)
	}
}
