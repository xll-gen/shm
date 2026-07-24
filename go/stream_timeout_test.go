package shm

import (
	"testing"
	"time"
)

// TestStreamReassembler_PrunesExpiredStreams pins the C++-parity age-based
// timeout prune (go/stream.go, mirroring StreamReassembler::PruneInternal): a
// partially-reassembled stream that stalls past the timeout is dropped — and
// the ooo bytes it parked are released — without waiting for the count-LRU
// bound (which only fires once MaxConcurrentStreams distinct streams pile up).
// The prune runs at the next StreamStart. Clock is injected so the timeout is
// driven deterministically with no real sleeps.
func TestStreamReassembler_PrunesExpiredStreams(t *testing.T) {
	current := time.Unix(1000, 0)
	clock := func() time.Time { return current }

	var completed []uint64
	onStream := func(id uint64, data []byte) { completed = append(completed, id) }

	const timeout = 10 * time.Second
	h := newStreamReassembler(onStream, nil, timeout, clock)

	// Victim: a 3-chunk stream that receives only an out-of-order chunk, so it
	// stays incomplete AND holds parked bytes in ctx.ooo (the memory item 2 is
	// about). chunkIndex 2 arrives before the cursor at 0, so it is parked.
	const victimID = 1
	if _, mt := h(streamStartReq(victimID, 6, 3), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("victim start: got %v want Normal ACK", mt)
	}
	if _, mt := h(streamChunkReq(victimID, 2, []byte{0x05, 0x06}), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
		t.Fatalf("victim ooo chunk: got %v want Normal ACK", mt)
	}

	// Advance the clock past the timeout, then start a new stream. That
	// StreamStart must trigger the age-based prune and evict the victim.
	current = current.Add(timeout + time.Second)
	const freshID = 2
	if _, mt := h(streamStartReq(freshID, 4, 2), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("fresh start: got %v want Normal ACK", mt)
	}

	// The victim's context is gone: a late chunk for it is now rejected
	// (same observable the count-LRU eviction test uses). Before the timeout
	// port the victim would still be resident and this would ACK Normal.
	if _, mt := h(streamChunkReq(victimID, 0, []byte{0x01, 0x02}), nil, MsgTypeStreamChunk); mt != MsgTypeSystemError {
		t.Fatalf("victim chunk after timeout prune: got %v want SystemError (context should be pruned)", mt)
	}

	// The freshly-started stream is unaffected and completes normally.
	if _, mt := h(streamChunkReq(freshID, 0, []byte{0xAA, 0xBB}), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
		t.Fatalf("fresh chunk0: got %v want Normal ACK", mt)
	}
	if _, mt := h(streamChunkReq(freshID, 1, []byte{0xCC, 0xDD}), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
		t.Fatalf("fresh chunk1: got %v want Normal ACK", mt)
	}
	if len(completed) != 1 || completed[0] != freshID {
		t.Fatalf("completed=%v, want [%d] (only the fresh stream should finish)", completed, freshID)
	}
}

// TestStreamReassembler_YoungStreamSurvivesTimeoutPrune verifies the prune is
// age-gated, not indiscriminate: a stream younger than the timeout stays
// resident across a prune triggered by another StreamStart and still completes.
func TestStreamReassembler_YoungStreamSurvivesTimeoutPrune(t *testing.T) {
	current := time.Unix(2000, 0)
	clock := func() time.Time { return current }

	var completed []uint64
	onStream := func(id uint64, data []byte) { completed = append(completed, id) }

	const timeout = 10 * time.Second
	h := newStreamReassembler(onStream, nil, timeout, clock)

	const aID = 10
	h(streamStartReq(aID, 4, 2), nil, MsgTypeStreamStart)
	h(streamChunkReq(aID, 0, []byte{0x01, 0x02}), nil, MsgTypeStreamChunk)

	// Advance the clock by LESS than the timeout, then start B (triggers the
	// prune). A's age is below the timeout, so it must survive.
	current = current.Add(timeout - time.Second)
	const bID = 11
	h(streamStartReq(bID, 4, 2), nil, MsgTypeStreamStart)

	if _, mt := h(streamChunkReq(aID, 1, []byte{0x03, 0x04}), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
		t.Fatalf("A chunk1: got %v want Normal ACK (A wrongly pruned?)", mt)
	}
	found := false
	for _, id := range completed {
		if id == aID {
			found = true
		}
	}
	if !found {
		t.Fatalf("young stream %d did not complete — it was wrongly pruned", aID)
	}
}

// TestStreamReassembler_TimeoutDisabled confirms timeout <= 0 turns the
// age-based prune off entirely, leaving only the count-LRU bound.
func TestStreamReassembler_TimeoutDisabled(t *testing.T) {
	current := time.Unix(3000, 0)
	clock := func() time.Time { return current }

	h := newStreamReassembler(func(uint64, []byte) {}, nil, 0, clock)

	const id = 20
	h(streamStartReq(id, 4, 2), nil, MsgTypeStreamStart)
	h(streamChunkReq(id, 0, []byte{0x01, 0x02}), nil, MsgTypeStreamChunk)

	// Jump far past any timeout, then start another stream. With the timeout
	// disabled, no age-based prune occurs and the first stream survives.
	current = current.Add(1000 * time.Hour)
	h(streamStartReq(id+1, 4, 2), nil, MsgTypeStreamStart)

	if _, mt := h(streamChunkReq(id, 1, []byte{0x03, 0x04}), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
		t.Fatalf("stream chunk with timeout disabled: got %v want Normal ACK (should not be pruned)", mt)
	}
}
