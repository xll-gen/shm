package shm

import (
	"encoding/binary"
	"encoding/hex"
	"fmt"
	"strings"
	"testing"
)

// This file is one half of the SPEC §3.3.4 accept/reject parity table. The
// other half is tests/test_reassembler_parity_table.cpp, which drives the
// identical case list through the C++ StreamReassembler and asserts the
// identical `want` digests. The digests are literal strings duplicated verbatim
// in both files on purpose: a divergence in either reassembler shows up as a
// mismatch against a constant, not against the other implementation's current
// behavior (which could drift in lockstep).
//
// The table exists to freeze the *observable* contract across an internal
// storage rework. It deliberately pins behavior the SPEC prose does not spell
// out chunk-by-chunk:
//
//   - which rejections drop the stream (running-length overflow, completion
//     mismatch) and which leave it able to complete (framing-truncated chunk,
//     out-of-range chunkIndex, chunk for an unknown stream id),
//   - first-arrival-wins duplicate handling, including a duplicate that
//     declares a *different* payloadSize — the dedup gate runs before the
//     overflow guard on both sides, so such a duplicate is ACK-ignored rather
//     than rejected,
//   - a repeated STREAM_START for the same id replaces the context.
//
// Do not "fix" a digest to make a change pass. A digest change is a wire
// behavior change and must be reflected in SPECIFICATION.md §3.3.4 and in the
// C++ half in the same commit.

// parityStep is one STREAM_CHUNK delivery (optionally preceded by a repeated
// STREAM_START for the same stream).
type parityStep struct {
	restart  bool   // re-send the case's STREAM_START before this chunk
	idx      uint32 // ChunkHeader.chunkIndex
	size     int    // payload bytes actually appended after the header
	declared int    // ChunkHeader.payloadSize; -1 means "same as size"
	otherID  bool   // address the chunk to a stream id that was never started
	marker   byte   // payload fill byte; 0 means 'A'+idx
}

type parityCase struct {
	name        string
	totalSize   uint64
	totalChunks uint32
	steps       []parityStep
	want        string
}

// parityChunkReq builds a STREAM_CHUNK request whose declared payloadSize may
// deliberately disagree with the number of bytes supplied (declared > size
// exercises the framing guard).
func parityChunkReq(streamID uint64, idx uint32, declared uint32, payload []byte) []byte {
	return parityChunkReqReserved(streamID, idx, declared, payload, 0)
}

// parityChunkReqReserved additionally writes ChunkHeader.reserved@16, so the
// inert-field tripwire in TestStreamReassembly_ReservedFieldsAreInert can set it
// to garbage.
func parityChunkReqReserved(streamID uint64, idx uint32, declared uint32, payload []byte, reserved uint32) []byte {
	req := make([]byte, chunkHeaderSize+len(payload))
	binary.LittleEndian.PutUint64(req[0:8], streamID)
	binary.LittleEndian.PutUint32(req[8:12], idx)
	binary.LittleEndian.PutUint32(req[12:16], declared)
	binary.LittleEndian.PutUint32(req[16:20], reserved)
	copy(req[chunkHeaderSize:], payload)
	return req
}

// parityStartReqReserved is streamStartReq with StreamHeader.reserved@20 set.
func parityStartReqReserved(streamID, totalSize uint64, totalChunks uint32, reserved uint32) []byte {
	req := streamStartReq(streamID, totalSize, totalChunks)
	binary.LittleEndian.PutUint32(req[20:24], reserved)
	return req
}

func replyChar(mt MsgType) string {
	switch mt {
	case MsgTypeNormal:
		return "N"
	case MsgTypeSystemError:
		return "E"
	default:
		return "?"
	}
}

// runParityCase drives one case and returns its digest.
func runParityCase(tc parityCase, streamID uint64) string {
	return runParityCaseReserved(tc, streamID, 0, 0)
}

func runParityCaseReserved(tc parityCase, streamID uint64, chunkReserved, startReserved uint32) string {
	var delivered [][]byte
	handler := NewStreamReassembler(func(_ uint64, data []byte) {
		cp := make([]byte, len(data))
		copy(cp, data)
		delivered = append(delivered, cp)
	}, nil)

	start := func() string {
		_, mt := handler(parityStartReqReserved(streamID, tc.totalSize, tc.totalChunks, startReserved), nil, MsgTypeStreamStart)
		return replyChar(mt)
	}

	startReply := start()

	var steps strings.Builder
	for _, st := range tc.steps {
		if st.restart {
			steps.WriteString("s" + start())
		}
		marker := st.marker
		if marker == 0 {
			marker = byte('A' + st.idx)
		}
		payload := make([]byte, st.size)
		for i := range payload {
			payload[i] = marker
		}
		declared := st.declared
		if declared < 0 {
			declared = st.size
		}
		id := streamID
		if st.otherID {
			id = streamID ^ 0xDEAD
		}
		_, mt := handler(parityChunkReqReserved(id, st.idx, uint32(declared), payload, chunkReserved), nil, MsgTypeStreamChunk)
		steps.WriteString(replyChar(mt))
	}

	data := "-"
	if len(delivered) > 0 {
		data = strings.ToUpper(hex.EncodeToString(delivered[len(delivered)-1]))
		if data == "" {
			data = "empty"
		}
	}
	return fmt.Sprintf("%s|start=%s|steps=%s|del=%d|data=%s",
		tc.name, startReply, steps.String(), len(delivered), data)
}

// parityCases is duplicated verbatim (modulo syntax) in
// tests/test_reassembler_parity_table.cpp.
var parityCases = []parityCase{
	{
		name: "in_order", totalSize: 4, totalChunks: 2,
		steps: []parityStep{{idx: 0, size: 2, declared: -1}, {idx: 1, size: 2, declared: -1}},
		want:  "in_order|start=N|steps=NN|del=1|data=41414242",
	},
	{
		name: "reverse", totalSize: 4, totalChunks: 2,
		steps: []parityStep{{idx: 1, size: 2, declared: -1}, {idx: 0, size: 2, declared: -1}},
		want:  "reverse|start=N|steps=NN|del=1|data=41414242",
	},
	{
		// Duplicate of an already-consumed index: ACK-ignored.
		name: "dup_consumed_same_size", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1},
			{idx: 0, size: 2, declared: -1},
			{idx: 1, size: 2, declared: -1},
		},
		want: "dup_consumed_same_size|start=N|steps=NNN|del=1|data=41414242",
	},
	{
		// First arrival wins: the duplicate's bytes are discarded, not merged.
		name: "dup_consumed_other_content", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1},
			{idx: 0, size: 2, declared: -1, marker: 'Z'},
			{idx: 1, size: 2, declared: -1},
		},
		want: "dup_consumed_other_content|start=N|steps=NNN|del=1|data=41414242",
	},
	{
		// A duplicate that declares a *larger* payloadSize than the first
		// arrival is still ACK-ignored: the dedup gate precedes the
		// running-length guard, so the 99 extra bytes are never counted and the
		// stream is not dropped for overflow.
		name: "dup_consumed_larger_size", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1},
			{idx: 0, size: 99, declared: -1, marker: 'Z'},
			{idx: 1, size: 2, declared: -1},
		},
		want: "dup_consumed_larger_size|start=N|steps=NNN|del=1|data=41414242",
	},
	{
		// Same, for an index still parked ahead of the cursor.
		name: "dup_parked_other_content", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 1, size: 2, declared: -1},
			{idx: 1, size: 2, declared: -1, marker: 'Z'},
			{idx: 0, size: 2, declared: -1},
		},
		want: "dup_parked_other_content|start=N|steps=NNN|del=1|data=41414242",
	},
	{
		name: "dup_parked_larger_size", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 1, size: 2, declared: -1},
			{idx: 1, size: 99, declared: -1, marker: 'Z'},
			{idx: 0, size: 2, declared: -1},
		},
		want: "dup_parked_larger_size|start=N|steps=NNN|del=1|data=41414242",
	},
	{
		// Zero-length chunk is counted exactly once (presence, not emptiness).
		name: "zero_len_then_dup", totalSize: 3, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 0, declared: -1},
			{idx: 0, size: 0, declared: -1},
			{idx: 1, size: 3, declared: -1},
		},
		want: "zero_len_then_dup|start=N|steps=NNN|del=1|data=424242",
	},
	{
		// totalSize == 0 with totalChunks > 0 is not the "empty stream"
		// shortcut: the context is created and completes once all zero-length
		// chunks arrive.
		name: "all_zero_len_zero_total", totalSize: 0, totalChunks: 3,
		steps: []parityStep{
			{idx: 0, size: 0, declared: -1},
			{idx: 2, size: 0, declared: -1},
			{idx: 1, size: 0, declared: -1},
		},
		want: "all_zero_len_zero_total|start=N|steps=NNN|del=1|data=empty",
	},
	{
		// Under-sized completion: every index filled but Σ payloadSize < totalSize.
		name: "undersize_completion_drops", totalSize: 6, totalChunks: 2,
		steps: []parityStep{{idx: 0, size: 2, declared: -1}, {idx: 1, size: 2, declared: -1}},
		want:  "undersize_completion_drops|start=N|steps=NE|del=0|data=-",
	},
	{
		name: "oversize_in_order_drops", totalSize: 3, totalChunks: 2,
		steps: []parityStep{{idx: 0, size: 2, declared: -1}, {idx: 1, size: 2, declared: -1}},
		want:  "oversize_in_order_drops|start=N|steps=NE|del=0|data=-",
	},
	{
		// Accumulated parked bytes overflow; no single chunk does.
		name: "ooo_accumulated_overflow_drops", totalSize: 8, totalChunks: 4,
		steps: []parityStep{
			{idx: 3, size: 4, declared: -1},
			{idx: 2, size: 4, declared: -1},
			{idx: 1, size: 1, declared: -1},
		},
		want: "ooo_accumulated_overflow_drops|start=N|steps=NNE|del=0|data=-",
	},
	{
		// Tiny advertisement, huge ahead-of-cursor chunk: refused at arrival,
		// and the stream is gone so the next chunk is refused as unknown.
		name: "ooo_first_overflow_drops", totalSize: 1, totalChunks: 1000,
		steps: []parityStep{
			{idx: 1, size: 4096, declared: -1},
			{idx: 2, size: 4096, declared: -1},
		},
		want: "ooo_first_overflow_drops|start=N|steps=EE|del=0|data=-",
	},
	{
		name: "zero_len_park_then_overflow_drops", totalSize: 4, totalChunks: 3,
		steps: []parityStep{
			{idx: 2, size: 0, declared: -1},
			{idx: 1, size: 5, declared: -1},
		},
		want: "zero_len_park_then_overflow_drops|start=N|steps=NE|del=0|data=-",
	},
	{
		// Exact fill is accepted (the guard is `>`, not `>=`).
		name: "exact_fill_parked_first", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 1, size: 4, declared: -1},
			{idx: 0, size: 0, declared: -1},
		},
		want: "exact_fill_parked_first|start=N|steps=NN|del=1|data=42424242",
	},
	{
		// Out-of-range chunkIndex is rejected but does NOT drop the stream:
		// the remaining valid chunks still complete it.
		name: "index_out_of_range_keeps_stream", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 2, size: 2, declared: -1},
			{idx: 0, size: 2, declared: -1},
			{idx: 1, size: 2, declared: -1},
		},
		want: "index_out_of_range_keeps_stream|start=N|steps=ENN|del=1|data=41414242",
	},
	{
		// Framing violation (declared payloadSize exceeds the bytes supplied)
		// is rejected before the stream is even looked up, so the stream
		// survives and completes.
		name: "truncated_framing_keeps_stream", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: 4},
			{idx: 0, size: 2, declared: -1},
			{idx: 1, size: 2, declared: -1},
		},
		want: "truncated_framing_keeps_stream|start=N|steps=ENN|del=1|data=41414242",
	},
	{
		// A chunk for a stream id that was never started is rejected and does
		// not disturb the live stream.
		name: "unknown_stream_id_keeps_stream", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1, otherID: true},
			{idx: 0, size: 2, declared: -1},
			{idx: 1, size: 2, declared: -1},
		},
		want: "unknown_stream_id_keeps_stream|start=N|steps=ENN|del=1|data=41414242",
	},
	{
		// A repeated STREAM_START for the same id replaces the context: the
		// already-consumed chunk 0 is accepted again and the stream completes
		// exactly once.
		name: "restart_replaces_context", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1},
			{restart: true, idx: 0, size: 2, declared: -1},
			{idx: 1, size: 2, declared: -1},
		},
		want: "restart_replaces_context|start=N|steps=NsNNN|del=1|data=41414242",
	},
	{
		// Single chunk that exactly fills the advertisement.
		name: "single_chunk_exact_fill", totalSize: 3, totalChunks: 1,
		steps: []parityStep{{idx: 0, size: 3, declared: -1}},
		want:  "single_chunk_exact_fill|start=N|steps=N|del=1|data=414141",
	},
}

// TestStreamReassembly_ParityTable pins the accept/reject digest of every case
// above. See the file header: the same table and the same digests are asserted
// by the C++ half.
func TestStreamReassembly_ParityTable(t *testing.T) {
	for i, tc := range parityCases {
		t.Run(tc.name, func(t *testing.T) {
			got := runParityCase(tc, uint64(1000+i))
			if got != tc.want {
				t.Errorf("digest mismatch\n got: %s\nwant: %s", got, tc.want)
			}
		})
	}
}

// TestStreamReassembly_ParityTableDump prints every digest so the C++ half's
// output can be diffed against it mechanically. Run with -v.
func TestStreamReassembly_ParityTableDump(t *testing.T) {
	for i, tc := range parityCases {
		t.Logf("%s", runParityCase(tc, uint64(1000+i)))
	}
}

// TestStreamReassembly_ReservedFieldsAreInert is a TRIPWIRE, not a feature test.
// The C++ half carries the identical check at the end of
// tests/test_reassembler_parity_table.cpp.
//
// ChunkHeader.reserved@16 and StreamHeader.reserved@20 carry no meaning as of
// SHM_VERSION 0x00070000, and both shipped senders write them as literal 0
// (putChunkHeader / putStreamHeader here, `ch.reserved = 0` in C++ Stream.h).
// Every parity case is replayed with both fields set to garbage and the digest
// must be IDENTICAL.
//
// WHY. There is a queued proposal to give ChunkHeader@16 meaning (an absolute
// dstOffset) and StreamHeader@20 meaning (an appMsgType). That is a WIRE BREAK
// exactly because old senders write 0 and 0 is a LEGAL offset for chunk 0: a new
// reader against an old sender would place every chunk at offset 0 and corrupt the
// stream SILENTLY, with an attach-time SHM_VERSION mismatch as the only safe
// detector.
//
// So this passes today and FAILS THE MOMENT EITHER FIELD IS GIVEN MEANING -- which
// is the alarm. It forces the SHM_VERSION bump, the SPEC update and both parity
// halves to be revised deliberately and together, instead of a field quietly
// acquiring semantics that one implementation honours and the other ignores.
//
// If you are here because this failed: you are making a wire change. Bump
// SHM_VERSION, update SPECIFICATION.md, update BOTH parity halves, and replace
// this test with the new field's own cases.
func TestStreamReassembly_ReservedFieldsAreInert(t *testing.T) {
	const garbageChunk = uint32(0xDEADBEEF)
	const garbageStart = uint32(0xCAFEBABE)
	for i, tc := range parityCases {
		t.Run(tc.name, func(t *testing.T) {
			plain := runParityCaseReserved(tc, uint64(5000+i), 0, 0)
			dirty := runParityCaseReserved(tc, uint64(5000+i), garbageChunk, garbageStart)
			if plain != dirty {
				t.Errorf("reserved fields are NOT inert -- this is a WIRE BEHAVIOR CHANGE "+
					"(see the comment above this test)\n  reserved=0:       %s\n  reserved=garbage: %s",
					plain, dirty)
			}
		})
	}
}
