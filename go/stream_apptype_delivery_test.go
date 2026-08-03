package shm

import (
	"bytes"
	"testing"
)

// Receive-side delivery of StreamHeader.appMsgType@20 (SPEC §3.3.1 "Receive-side
// delivery"). The C++ twin is tests/test_reassembler_appmsgtype_delivery.cpp and
// the two are ONE artifact: same four obligations, case for case.
//
// WHY THIS FILE EXISTS SEPARATELY FROM THE OTHER appMsgType TESTS. Until this
// clause the field was WRITE-ONLY. Both senders wrote it and both reassemblers
// parsed past it, so the two guards that existed could not see the gap:
//
//   - stream_sender_appmsgtype_test.go proves the byte reaches WIRE OFFSET 20.
//     It reads the bytes back off the segment from the host side; it says
//     nothing about anything downstream of the wire.
//   - TestStreamReassembly_AppMsgTypeIsInertToReassembly proves the byte changes
//     NOTHING. It is a negative assertion by construction — it would stay green
//     against a reassembler that never touched offset 20 at all, which is
//     precisely the state it was written to describe.
//
// So neither could fail for "the tag never reaches the handler", and the tests
// below are the only thing that can.
//
// NON-VACUITY, and the trap this repo already documented for dstOffset (a parity
// table mistaken for a placement guard, because two different implementations
// produce byte-identical output): ask whether each assertion below could hold if
// the field were never read. It could not.
//
//   - the delivered tag is asserted EQUAL to a value no other header field
//     carries, so "deliver a constant" and "deliver 0" both fail;
//   - one case asserts a ZERO tag on a header whose every other field is
//     nonzero, so "read totalChunks@16 / totalSize@8 / streamId@0 instead" fails;
//   - one case interleaves two streams with different tags and completes them in
//     the reverse of their start order, so "remember the last tag the reassembler
//     saw" fails.
//
// Confirmed by mutation: delivering a hard-coded constant instead of
// ctx.appMsgType flips these red while the whole rest of the Go suite stays
// green.

const (
	// Chosen so it collides with no other field of any header these tests build.
	deliveryTag = uint32(0x1234ABCD)
	// A second, different tag for the interleaving case.
	deliveryTagB = uint32(0x0BADF00D)
)

// typedCapture records what the typed completion callback was handed.
type typedCapture struct {
	streamID   uint64
	appMsgType uint32
	data       []byte
}

func newTypedCollector() (handler func(req []byte, resp []byte, msgType MsgType) (int32, MsgType), got *[]typedCapture) {
	var seen []typedCapture
	h := NewStreamReassemblerTyped(func(streamID uint64, appMsgType uint32, data []byte) {
		cp := make([]byte, len(data))
		copy(cp, data)
		seen = append(seen, typedCapture{streamID: streamID, appMsgType: appMsgType, data: cp})
	}, nil)
	return h, &seen
}

// TestStreamReassemblyTyped_DeliversAppMsgTypeFromStreamStart is the core
// end-to-end assertion the sender-side test could never make: SetAppMsgType's
// value, carried on STREAM_START, reaches the completion HANDLER after a
// multi-chunk reassembly — i.e. it survives being stashed in the stream context
// across every chunk.
func TestStreamReassemblyTyped_DeliversAppMsgTypeFromStreamStart(t *testing.T) {
	const streamID = uint64(0xA1)
	payloads := [][]byte{[]byte("ABC"), []byte("DEF"), []byte("GHI")}

	handler, got := newTypedCollector()

	if _, mt := handler(parityStartReqApp(streamID, 9, 3, deliveryTag), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("STREAM_START reply = %v, want NORMAL", mt)
	}
	for i, p := range payloads {
		if _, mt := handler(streamChunkReq(streamID, uint32(i), p), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("chunk %d reply = %v, want NORMAL", i, mt)
		}
	}

	if len(*got) != 1 {
		t.Fatalf("deliveries = %d, want 1", len(*got))
	}
	d := (*got)[0]
	if d.appMsgType != deliveryTag {
		t.Errorf("handler received appMsgType = 0x%08X, want 0x%08X\n"+
			"StreamHeader.appMsgType@20 did not reach the stream handler. The wire-offset test "+
			"(stream_sender_appmsgtype_test.go) and the inertness tripwire both stay GREEN in that "+
			"state -- this test is the only thing that can see it.", d.appMsgType, deliveryTag)
	}
	if d.streamID != streamID {
		t.Errorf("handler received streamID = %#x, want %#x", d.streamID, streamID)
	}
	if !bytes.Equal(d.data, []byte("ABCDEFGHI")) {
		t.Errorf("handler received data = %q, want %q -- surfacing the tag must not perturb the bytes",
			d.data, "ABCDEFGHI")
	}
}

// TestStreamReassemblyTyped_DeliversZeroTagVerbatim is the half a "does the tag
// arrive" test cannot cover. Every other field of this header is nonzero, so a
// receiver that read totalChunks@16 (3), totalSize@8 (9) or streamId@0 (0xB2)
// instead of appMsgType@20 delivers a nonzero value and fails here, while
// passing the case above only if it happened to read the right offset.
//
// It also pins SPEC §3.3.1 obligation 4: 0 is a legal value, not "unset". No
// substitution, no normalisation.
func TestStreamReassemblyTyped_DeliversZeroTagVerbatim(t *testing.T) {
	const streamID = uint64(0xB2)
	handler, got := newTypedCollector()

	if _, mt := handler(parityStartReqApp(streamID, 9, 3, 0), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("STREAM_START reply = %v, want NORMAL", mt)
	}
	for i, p := range [][]byte{[]byte("ABC"), []byte("DEF"), []byte("GHI")} {
		if _, mt := handler(streamChunkReq(streamID, uint32(i), p), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("chunk %d reply = %v, want NORMAL", i, mt)
		}
	}

	if len(*got) != 1 {
		t.Fatalf("deliveries = %d, want 1", len(*got))
	}
	if tag := (*got)[0].appMsgType; tag != 0 {
		t.Errorf("handler received appMsgType = 0x%08X for a header whose appMsgType@20 is 0, want 0.\n"+
			"totalChunks@16 = 3, totalSize@8 = 9, streamId@0 = 0x%X -- a nonzero value here means the "+
			"receiver read the WRONG FIELD, not that it read nothing.", tag, streamID)
	}
}

// TestStreamReassemblyTyped_TagIsPerStreamNotLastSeen kills the cheapest wrong
// implementation: remember the most recent appMsgType in the reassembler and
// hand that to every completion. Two streams are opened with different tags, and
// the one started FIRST completes LAST, so a last-seen-wins receiver reports
// deliveryTagB for both.
//
// SPEC §3.3.1 obligation 2: the tag delivered is the one carried by the
// STREAM_START that created THAT stream's context.
func TestStreamReassemblyTyped_TagIsPerStreamNotLastSeen(t *testing.T) {
	const streamA = uint64(0xC3)
	const streamB = uint64(0xD4)

	handler, got := newTypedCollector()

	if _, mt := handler(parityStartReqApp(streamA, 6, 2, deliveryTag), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("STREAM_START A reply = %v, want NORMAL", mt)
	}
	if _, mt := handler(parityStartReqApp(streamB, 6, 2, deliveryTagB), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("STREAM_START B reply = %v, want NORMAL", mt)
	}

	// Interleave, and finish B before A.
	handler(streamChunkReq(streamA, 0, []byte("aaa")), nil, MsgTypeStreamChunk)
	handler(streamChunkReq(streamB, 0, []byte("bbb")), nil, MsgTypeStreamChunk)
	handler(streamChunkReq(streamB, 1, []byte("BBB")), nil, MsgTypeStreamChunk)
	handler(streamChunkReq(streamA, 1, []byte("AAA")), nil, MsgTypeStreamChunk)

	if len(*got) != 2 {
		t.Fatalf("deliveries = %d, want 2", len(*got))
	}
	want := map[uint64]uint32{streamA: deliveryTag, streamB: deliveryTagB}
	for _, d := range *got {
		w, ok := want[d.streamID]
		if !ok {
			t.Fatalf("unexpected delivery for streamID %#x", d.streamID)
		}
		if d.appMsgType != w {
			t.Errorf("stream %#x delivered appMsgType = 0x%08X, want 0x%08X -- the tag is being taken "+
				"from the most recent STREAM_START rather than from this stream's own context",
				d.streamID, d.appMsgType, w)
		}
		delete(want, d.streamID)
	}
	if len(want) != 0 {
		t.Errorf("streams never delivered: %v", want)
	}
}

// TestStreamReassemblyTyped_DeliversAppMsgTypeOnEmptyStream covers the second
// completion path (SPEC §3.3.1 obligation 3). An empty stream never reaches the
// chunk handler at all: it completes inside STREAM_START, so a fix applied only
// to the chunk-completion path leaves this one delivering 0.
func TestStreamReassemblyTyped_DeliversAppMsgTypeOnEmptyStream(t *testing.T) {
	const streamID = uint64(0xE5)
	handler, got := newTypedCollector()

	if _, mt := handler(parityStartReqApp(streamID, 0, 0, deliveryTag), nil, MsgTypeStreamStart); mt != MsgTypeNormal {
		t.Fatalf("STREAM_START reply = %v, want NORMAL", mt)
	}

	if len(*got) != 1 {
		t.Fatalf("deliveries = %d, want 1 (an empty stream completes inside STREAM_START)", len(*got))
	}
	if tag := (*got)[0].appMsgType; tag != deliveryTag {
		t.Errorf("empty-stream delivery carried appMsgType = 0x%08X, want 0x%08X -- the "+
			"totalChunks==0 completion path is a SECOND delivery site and must carry the tag too",
			tag, deliveryTag)
	}
	if len((*got)[0].data) != 0 {
		t.Errorf("empty-stream delivery carried %d data bytes, want 0", len((*got)[0].data))
	}
}

// TestStreamReassemblyTyped_RestartReplacesTag pins the last clause of
// obligation 2: a STREAM_START that replaces a live context for the same
// streamId replaces its tag along with the rest of its state. A receiver that
// captured the tag once and refused to overwrite it reports deliveryTag here.
func TestStreamReassemblyTyped_RestartReplacesTag(t *testing.T) {
	const streamID = uint64(0xF6)
	handler, got := newTypedCollector()

	handler(parityStartReqApp(streamID, 6, 2, deliveryTag), nil, MsgTypeStreamStart)
	handler(streamChunkReq(streamID, 0, []byte("xxx")), nil, MsgTypeStreamChunk)

	// Replace the live context, then run the stream to completion.
	handler(parityStartReqApp(streamID, 6, 2, deliveryTagB), nil, MsgTypeStreamStart)
	handler(streamChunkReq(streamID, 0, []byte("yyy")), nil, MsgTypeStreamChunk)
	handler(streamChunkReq(streamID, 1, []byte("zzz")), nil, MsgTypeStreamChunk)

	if len(*got) != 1 {
		t.Fatalf("deliveries = %d, want 1", len(*got))
	}
	if tag := (*got)[0].appMsgType; tag != deliveryTagB {
		t.Errorf("delivered appMsgType = 0x%08X, want 0x%08X -- a STREAM_START that replaces a live "+
			"context must replace its tag too", tag, deliveryTagB)
	}
}

// TestStreamReassemblyTyped_DroppedStreamDeliversNothing: obligation 3's second
// half. Chunk 1 is misplaced (dstOffset 99 on a 6-byte stream), so the stream is
// dropped -- and a dropped stream fires no completion at all, tag included.
func TestStreamReassemblyTyped_DroppedStreamDeliversNothing(t *testing.T) {
	const streamID = uint64(0x17)
	handler, got := newTypedCollector()

	handler(parityStartReqApp(streamID, 6, 2, deliveryTag), nil, MsgTypeStreamStart)
	handler(streamChunkReq(streamID, 0, []byte("xxx")), nil, MsgTypeStreamChunk)
	if _, mt := handler(streamChunkReqAt(streamID, 1, 99, []byte("yyy")), nil, MsgTypeStreamChunk); mt != MsgTypeSystemError {
		t.Fatalf("out-of-bounds chunk reply = %v, want SYSTEM_ERROR", mt)
	}

	if len(*got) != 0 {
		t.Fatalf("deliveries = %d, want 0 -- a dropped stream must not complete", len(*got))
	}
}

// TestStreamReassemblerUntyped_IsTheTypedFormWithTheTagDropped is SPEC §3.3.1
// obligation 1, the additive one: the pre-existing (streamID, data) entry point
// keeps its signature and its behavior with a tag set on the wire. If this file
// had changed StreamHandler instead of adding a parallel type, this would not
// compile -- which is the point.
func TestStreamReassemblerUntyped_IsTheTypedFormWithTheTagDropped(t *testing.T) {
	const streamID = uint64(0x28)
	var delivered [][]byte
	handler := NewStreamReassembler(func(id uint64, data []byte) {
		if id != streamID {
			t.Errorf("untyped handler streamID = %#x, want %#x", id, streamID)
		}
		cp := make([]byte, len(data))
		copy(cp, data)
		delivered = append(delivered, cp)
	}, nil)

	handler(parityStartReqApp(streamID, 9, 3, deliveryTag), nil, MsgTypeStreamStart)
	for i, p := range [][]byte{[]byte("ABC"), []byte("DEF"), []byte("GHI")} {
		if _, mt := handler(streamChunkReq(streamID, uint32(i), p), nil, MsgTypeStreamChunk); mt != MsgTypeNormal {
			t.Fatalf("chunk %d reply = %v, want NORMAL", i, mt)
		}
	}

	if len(delivered) != 1 || !bytes.Equal(delivered[0], []byte("ABCDEFGHI")) {
		t.Fatalf("untyped deliveries = %q, want one delivery of %q", delivered, "ABCDEFGHI")
	}
}
