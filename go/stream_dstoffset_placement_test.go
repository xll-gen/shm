package shm

import (
	"bytes"
	"testing"
	"time"
)

// This file carries the ONLY test that can distinguish a dstOffset-honouring
// receiver from a cursor-appending one by observing reassembly state.
//
// WHY NOTHING ELSE CAN. The receiver requires dstOffset to equal the running
// in-order cursor at the moment an index becomes in-order, and drops the stream
// otherwise. So for every stream that is NOT dropped, "write at dstOffset" and
// "append at the cursor" put every byte in the same place, and therefore produce
// byte-identical delivered output, identical accept/reject replies, and identical
// parity digests. That is a theorem about the invariant, not a property of any
// particular arrival order: reversing arrival order does not help, because the old
// parking receiver drained in INDEX order too. (It is also why all 20 legacy parity
// digests survived the 0x00080000 rework unchanged — no luck was involved.)
//
// The one observable that does differ is the destination buffer's content BEFORE
// the cursor reaches it: a direct-placement receiver already has an
// ahead-of-cursor chunk's bytes at buf[dstOffset] while the stream is still
// incomplete; a parking receiver has zeros there and only fills them at drain.
// buf is handed to the stream handler only at completion, so the observation has
// to be taken mid-stream through newStreamReassemblerPeek.
//
// Companion guard: go/stream_dstoffset_residency_test.go measures the *cost* of
// the same change (a parked record holds no payload, and parked residency does
// not grow with chunk size). This file pins the *semantics*.

// mustPeek returns the live context for streamID or fails the test.
func mustPeek(t *testing.T, peek func(uint64) *streamContext, streamID uint64) *streamContext {
	t.Helper()
	ctx := peek(streamID)
	if ctx == nil {
		t.Fatalf("stream %d is not in flight — the observation window is gone, so this test would assert nothing", streamID)
	}
	return ctx
}

// TestStreamReassembly_AheadOfCursorChunkIsResidentAtDstOffset delivers a chunk
// that is AHEAD of the in-order cursor and then, while the stream is still
// incomplete, inspects the destination buffer.
//
// Post-0x00080000 the payload must ALREADY be at buf[dstOffset] and the region
// the cursor has not reached must still be untouched. A parking receiver fails
// the first assertion (zeros at dstOffset); a receiver that wrote at its own
// cursor instead of at dstOffset fails the second (payload at buf[0]).
func TestStreamReassembly_AheadOfCursorChunkIsResidentAtDstOffset(t *testing.T) {
	const streamID = 0x0801
	// Deliberately NON-UNIFORM sizes so dstOffset is not idx*size for any idx:
	// chunk 0 is 1 byte at 0, chunk 1 is 2 bytes at 1, chunk 2 is 3 bytes at 3.
	// A receiver that reconstructed the offset from the index instead of reading
	// the field cannot land chunk 2 at 3.
	const totalSize = 6
	chunk2 := []byte("CCC")

	var delivered [][]byte
	handler, peek := newStreamReassemblerPeek(func(_ uint64, data []byte) {
		cp := make([]byte, len(data))
		copy(cp, data)
		delivered = append(delivered, cp)
	}, nil, DefaultStreamTimeout, time.Now)

	if _, mt := handler(streamStartReq(streamID, totalSize, 3), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("StreamStart rejected: %v", mt)
	}

	// Chunk 2 only. The cursor is still at index 0 / offset 0, so this chunk is
	// ahead of the cursor and the stream cannot complete.
	if _, mt := handler(streamChunkReqAt(streamID, 2, 3, chunk2), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
		t.Fatalf("ahead-of-cursor chunk 2 rejected: %v", mt)
	}
	if len(delivered) != 0 {
		t.Fatalf("stream completed after one of three chunks (%d deliveries) — there is no mid-stream window to observe", len(delivered))
	}

	ctx := mustPeek(t, peek, streamID)
	ctx.mu.Lock()
	// Snapshot everything under the context lock; assert after unlocking so a
	// t.Fatalf cannot leave the mutex held.
	bufCopy := make([]byte, len(ctx.buf))
	copy(bufCopy, ctx.buf)
	next, offset, oooBytes := ctx.next, ctx.offset, ctx.oooBytes
	placed, parked := ctx.ooo[2]
	ctx.mu.Unlock()

	if next != 0 || offset != 0 {
		t.Fatalf("cursor moved to index %d / offset %d on an ahead-of-cursor chunk; the chunk was not actually ahead of the cursor and the test proves nothing", next, offset)
	}

	// THE DISCRIMINATOR. dstOffset = 3, payload "CCC".
	if got := bufCopy[3:6]; !bytes.Equal(got, chunk2) {
		t.Errorf("destination buffer holds %q at dstOffset 3 while the stream is incomplete, want %q\n"+
			"an ahead-of-cursor chunk is NOT being written to buf[dstOffset] on arrival — it is being PARKED as a payload copy, "+
			"which is exactly the pre-SHM_VERSION-0x00080000 behavior the bump exists to replace. "+
			"No output-byte or parity-digest test can catch this (see this file's header), so this assertion is the only guard.",
			got, chunk2)
	}
	// And it went to the ABSOLUTE offset, not to the cursor.
	if !bytes.Equal(bufCopy[0:3], []byte{0, 0, 0}) {
		t.Errorf("destination buffer holds %q at offsets 0..2, want zeros\n"+
			"the ahead-of-cursor chunk was appended at the in-order cursor instead of placed at its declared dstOffset",
			bufCopy[0:3])
	}

	// The record left behind is bookkeeping only: it names the destination and the
	// size, and its bytes are already counted as ahead-of-cursor residency.
	if !parked {
		t.Fatalf("chunk 2 is not recorded in ooo — nothing tracks the ahead-of-cursor index")
	}
	if placed.dstOffset != 3 || placed.size != 3 {
		t.Errorf("parked record = {dstOffset:%d size:%d}, want {3 3} — the recorded destination is what the drain compares against the cursor", placed.dstOffset, placed.size)
	}
	if oooBytes != 3 {
		t.Errorf("oooBytes = %d, want 3 (the §3.3.4 running-length bound must count bytes placed ahead of the cursor)", oooBytes)
	}

	// Finish the stream so the test also proves the mid-stream state it observed
	// is a state a NORMAL stream passes through, not a stuck one.
	if _, mt := handler(streamChunkReqAt(streamID, 0, 0, []byte("A")), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
		t.Fatalf("chunk 0 rejected: %v", mt)
	}
	if _, mt := handler(streamChunkReqAt(streamID, 1, 1, []byte("BB")), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
		t.Fatalf("chunk 1 rejected: %v", mt)
	}
	if len(delivered) != 1 || !bytes.Equal(delivered[0], []byte("ABBCCC")) {
		t.Fatalf("delivered %q, want exactly one %q", delivered, "ABBCCC")
	}
}

// TestStreamReassembly_ParkedChunksAreAllResidentBeforeTheGapFills is the
// many-chunk form of the same observation, and it is the one that would catch a
// receiver that placed *some* ahead-of-cursor chunks directly and parked others
// (e.g. a size threshold, or placement only for the first parked index).
//
// Every chunk except index 0 is delivered, in reverse order, so N-1 chunks are
// ahead of the cursor at once. The whole destination except chunk 0's slot must
// already be filled while the stream is still incomplete.
func TestStreamReassembly_ParkedChunksAreAllResidentBeforeTheGapFills(t *testing.T) {
	const streamID = 0x0802
	const totalChunks = 16
	const chunkLen = 8
	const totalSize = totalChunks * chunkLen

	payload := func(i int) []byte { return bytes.Repeat([]byte{byte('a' + i)}, chunkLen) }

	var deliveries int
	handler, peek := newStreamReassemblerPeek(func(_ uint64, _ []byte) { deliveries++ }, nil, DefaultStreamTimeout, time.Now)

	if _, mt := handler(streamStartReq(streamID, totalSize, totalChunks), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("StreamStart rejected: %v", mt)
	}
	for i := totalChunks - 1; i >= 1; i-- {
		if _, mt := handler(streamChunkReqAt(streamID, uint32(i), uint64(i)*chunkLen, payload(i)), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("chunk %d rejected: %v", i, mt)
		}
	}
	if deliveries != 0 {
		t.Fatalf("stream completed with chunk 0 missing (%d deliveries)", deliveries)
	}

	ctx := mustPeek(t, peek, streamID)
	ctx.mu.Lock()
	bufCopy := make([]byte, len(ctx.buf))
	copy(bufCopy, ctx.buf)
	parkedCount := len(ctx.ooo)
	ctx.mu.Unlock()

	if parkedCount != totalChunks-1 {
		t.Fatalf("%d indices recorded ahead of the cursor, want %d", parkedCount, totalChunks-1)
	}
	for i := 1; i < totalChunks; i++ {
		got := bufCopy[i*chunkLen : (i+1)*chunkLen]
		if !bytes.Equal(got, payload(i)) {
			t.Fatalf("chunk %d is not resident at its dstOffset %d while the stream is incomplete: buf holds %q, want %q\n"+
				"this ahead-of-cursor chunk was parked as a payload copy instead of written to buf[dstOffset] on arrival",
				i, i*chunkLen, got, payload(i))
		}
	}
	// Chunk 0's slot is the only untouched region.
	if !bytes.Equal(bufCopy[0:chunkLen], make([]byte, chunkLen)) {
		t.Errorf("buf[0:%d] = %q, want zeros — the missing chunk's region must be untouched", chunkLen, bufCopy[0:chunkLen])
	}

	if _, mt := handler(streamChunkReqAt(streamID, 0, 0, payload(0)), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
		t.Fatalf("gap-filling chunk 0 rejected: %v", mt)
	}
	if deliveries != 1 {
		t.Fatalf("deliveries = %d, want 1", deliveries)
	}
}
