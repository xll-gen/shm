package shm

import (
	"bytes"
	"encoding/binary"
	"testing"
)

func streamStartReq(streamID, totalSize uint64, totalChunks uint32) []byte {
	buf := new(bytes.Buffer)
	binary.Write(buf, binary.LittleEndian, StreamHeader{
		StreamID:    streamID,
		TotalSize:   totalSize,
		TotalChunks: totalChunks,
	})
	return buf.Bytes()
}

func streamChunkReq(streamID uint64, chunkIndex uint32, payload []byte) []byte {
	buf := new(bytes.Buffer)
	binary.Write(buf, binary.LittleEndian, ChunkHeader{
		StreamID:    streamID,
		ChunkIndex:  chunkIndex,
		PayloadSize: uint32(len(payload)),
	})
	buf.Write(payload)
	return buf.Bytes()
}

// TestStreamReassembler_EvictsIncompleteStreamsUnderPressure verifies that a
// stream which never receives all of its chunks cannot pin memory forever.
// Once MaxConcurrentStreams distinct streams are in flight, the
// least-recently-active incomplete stream is evicted. A final chunk that
// arrives for an evicted stream is rejected (SystemError) rather than
// silently completing.
func TestStreamReassembler_EvictsIncompleteStreamsUnderPressure(t *testing.T) {
	var completed []uint64
	onStream := func(streamID uint64, data []byte) {
		completed = append(completed, streamID)
	}
	handler := NewStreamReassembler(onStream, nil)

	const victimID = 1

	// Start the victim (2 chunks) and deliver only chunk 0 — incomplete.
	if _, mt := handler(streamStartReq(victimID, 4, 2), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("victim start: got %v, want Normal ACK", mt)
	}
	if _, mt := handler(streamChunkReq(victimID, 0, []byte{0xAA, 0xBB}), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
		t.Fatalf("victim chunk0: got %v, want Normal ACK", mt)
	}

	// Flood with MaxConcurrentStreams more distinct incomplete streams. This
	// pushes the in-flight count past the bound, forcing eviction of the LRU
	// entry — the victim, whose last activity predates the flood.
	for i := 0; i < MaxConcurrentStreams; i++ {
		id := uint64(1000 + i)
		handler(streamStartReq(id, 4, 2), nil, MsgTypeStreamStart)
	}

	// The victim's final chunk must now be rejected: its context was evicted.
	if _, mt := handler(streamChunkReq(victimID, 1, []byte{0xCC, 0xDD}), nil, MsgTypeStreamChunk); mt != MsgTypeSystemError {
		t.Fatalf("victim chunk1 after eviction: got %v, want SystemError", mt)
	}
	for _, id := range completed {
		if id == victimID {
			t.Fatalf("victim stream %d completed despite eviction", victimID)
		}
	}
}

// TestStreamReassembler_ActiveStreamSurvivesPressure verifies eviction is
// least-recently-active, not arbitrary: a stream that keeps receiving chunks
// stays resident even while many other streams start and go stale, and still
// completes correctly.
func TestStreamReassembler_ActiveStreamSurvivesPressure(t *testing.T) {
	var completed []uint64
	onStream := func(streamID uint64, data []byte) {
		completed = append(completed, streamID)
	}
	handler := NewStreamReassembler(onStream, nil)

	const keepID = 7

	// keepID needs 3 chunks of 2 bytes (TotalSize 6).
	handler(streamStartReq(keepID, 6, 3), nil, MsgTypeStreamStart)
	handler(streamChunkReq(keepID, 0, []byte{0x01, 0x02}), nil, MsgTypeStreamChunk)

	// Interleave keepID activity with a flood so it stays the most-recently
	// active each round and is never the eviction target.
	for i := 0; i < MaxConcurrentStreams; i++ {
		handler(streamStartReq(uint64(2000+i), 4, 2), nil, MsgTypeStreamStart)
		if i == MaxConcurrentStreams/2 {
			handler(streamChunkReq(keepID, 1, []byte{0x03, 0x04}), nil, MsgTypeStreamChunk)
		}
	}

	// Final chunk completes keepID.
	if _, mt := handler(streamChunkReq(keepID, 2, []byte{0x05, 0x06}), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
		t.Fatalf("keep chunk2: got %v, want Normal ACK", mt)
	}
	found := false
	for _, id := range completed {
		if id == keepID {
			found = true
		}
	}
	if !found {
		t.Fatalf("active stream %d was evicted under pressure (not completed)", keepID)
	}
}
