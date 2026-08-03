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
//     mismatch, a self-contradictory dstOffset) and which leave it able to
//     complete (framing-truncated chunk, out-of-range chunkIndex, chunk for an
//     unknown stream id),
//   - first-arrival-wins duplicate handling, including a duplicate that
//     declares a *different* payloadSize — the dedup gate runs before the
//     overflow guard AND before the dstOffset guards on both sides, so such a
//     duplicate is ACK-ignored rather than rejected even when its dstOffset is
//     out of bounds,
//   - a repeated STREAM_START for the same id replaces the context,
//   - ChunkHeader.dstOffset (SHM_VERSION 0x00080000, formerly reserved@16) is
//     REJECTED when it is self-contradictory: bounds-checked against totalSize
//     with wrap-safe arithmetic, and required to equal the in-order cursor at the
//     moment its index becomes in-order (caught at either detection site — the
//     in-order arrival and the drain).
//
// WHAT THIS TABLE CANNOT DO, and it is important that nobody reads it as doing
// so: it CANNOT distinguish a receiver that writes each chunk to buf[dstOffset]
// from one that ignores dstOffset and appends at its own running cursor. The
// invariant above forces dstOffset == cursor for every index at the moment that
// index becomes in-order, so for every stream that is NOT dropped the two shapes
// put every byte in the same place and produce byte-identical output, identical
// replies, and therefore identical digests. That is a theorem about the
// invariant, not an artifact of the chosen arrival orders — reversing arrival
// order does not help, because a parking receiver drains in INDEX order too. It
// is also why all 20 pre-0x00080000 digests below survived the rework unchanged:
// no coincidence was involved.
//
// The guards for direct placement are therefore elsewhere, and they are the ONLY
// guards for it:
//
//   - go/stream_dstoffset_placement_test.go — the semantics: an ahead-of-cursor
//     chunk's bytes are already at buf[dstOffset] while the stream is still
//     incomplete (the one observable that differs; it requires mid-stream access
//     to the context, which is why it is not in this table).
//   - go/stream_dstoffset_residency_test.go — the cost: a parked record holds no
//     payload and parked residency does not grow with chunk size.
//
// Do not "fix" a digest to make a change pass. A digest change is a wire
// behavior change and must be reflected in SPECIFICATION.md §3.3.4 and in the
// C++ half in the same commit.

// deriveOff is the parityStep.off sentinel: derive ChunkHeader.dstOffset as
// idx*size. Most cases here send uniform 2-byte payloads, for which that is
// exactly what a conforming sender writes. It is a NEGATIVE sentinel, not the
// zero value, because 0 is a perfectly legal dstOffset — a step that forgot to
// set the field must not silently become "offset 0".
const deriveOff = int64(-1)

// parityStep is one STREAM_CHUNK delivery (optionally preceded by a repeated
// STREAM_START for the same stream).
type parityStep struct {
	restart  bool   // re-send the case's STREAM_START before this chunk
	idx      uint32 // ChunkHeader.chunkIndex
	size     int    // payload bytes actually appended after the header
	declared int    // ChunkHeader.payloadSize; -1 means "same as size"
	off      int64  // ChunkHeader.dstOffset; deriveOff means idx*size
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

// parityChunkReqAt builds a STREAM_CHUNK whose declared payloadSize may
// deliberately disagree with the number of bytes supplied (declared > size
// exercises the framing guard) and whose dstOffset is written verbatim, so a
// case can misplace a chunk or point it out of bounds on purpose.
func parityChunkReqAt(streamID uint64, idx uint32, declared uint32, dstOffset uint32, payload []byte) []byte {
	req := make([]byte, chunkHeaderSize+len(payload))
	binary.LittleEndian.PutUint64(req[0:8], streamID)
	binary.LittleEndian.PutUint32(req[8:12], idx)
	binary.LittleEndian.PutUint32(req[12:16], declared)
	binary.LittleEndian.PutUint32(req[16:20], dstOffset)
	copy(req[chunkHeaderSize:], payload)
	return req
}

// parityStartReqApp is streamStartReq with StreamHeader.appMsgType@20 set, so
// TestStreamReassembly_AppMsgTypeIsInertToReassembly can fill it with garbage.
func parityStartReqApp(streamID, totalSize uint64, totalChunks uint32, appMsgType uint32) []byte {
	req := streamStartReq(streamID, totalSize, totalChunks)
	binary.LittleEndian.PutUint32(req[20:24], appMsgType)
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
	return runParityCaseApp(tc, streamID, 0)
}

// runParityCaseApp drives one case with StreamHeader.appMsgType set to
// appMsgType. appMsgType is opaque to shm and MUST NOT change the digest.
func runParityCaseApp(tc parityCase, streamID uint64, appMsgType uint32) string {
	var delivered [][]byte
	handler := NewStreamReassembler(func(_ uint64, data []byte) {
		cp := make([]byte, len(data))
		copy(cp, data)
		delivered = append(delivered, cp)
	}, nil)

	start := func() string {
		_, mt := handler(parityStartReqApp(streamID, tc.totalSize, tc.totalChunks, appMsgType), nil, MsgTypeStreamStart)
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
		dstOffset := st.off
		if dstOffset == deriveOff {
			dstOffset = int64(st.idx) * int64(st.size)
		}
		id := streamID
		if st.otherID {
			id = streamID ^ 0xDEAD
		}
		_, mt := handler(parityChunkReqAt(id, st.idx, uint32(declared), uint32(dstOffset), payload), nil, MsgTypeStreamChunk)
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
		steps: []parityStep{{idx: 0, size: 2, declared: -1, off: deriveOff}, {idx: 1, size: 2, declared: -1, off: deriveOff}},
		want:  "in_order|start=N|steps=NN|del=1|data=41414242",
	},
	{
		name: "reverse", totalSize: 4, totalChunks: 2,
		steps: []parityStep{{idx: 1, size: 2, declared: -1, off: deriveOff}, {idx: 0, size: 2, declared: -1, off: deriveOff}},
		want:  "reverse|start=N|steps=NN|del=1|data=41414242",
	},
	{
		// Duplicate of an already-consumed index: ACK-ignored.
		name: "dup_consumed_same_size", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1, off: deriveOff},
			{idx: 0, size: 2, declared: -1, off: deriveOff},
			{idx: 1, size: 2, declared: -1, off: deriveOff},
		},
		want: "dup_consumed_same_size|start=N|steps=NNN|del=1|data=41414242",
	},
	{
		// First arrival wins: the duplicate's bytes are discarded, not merged.
		name: "dup_consumed_other_content", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1, off: deriveOff},
			{idx: 0, size: 2, declared: -1, off: deriveOff, marker: 'Z'},
			{idx: 1, size: 2, declared: -1, off: deriveOff},
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
			{idx: 0, size: 2, declared: -1, off: deriveOff},
			{idx: 0, size: 99, declared: -1, off: deriveOff, marker: 'Z'},
			{idx: 1, size: 2, declared: -1, off: deriveOff},
		},
		want: "dup_consumed_larger_size|start=N|steps=NNN|del=1|data=41414242",
	},
	{
		// Same, for an index still parked ahead of the cursor. Note the derived
		// dstOffset of the duplicate is 1*99 = 99, far past totalSize=4: the
		// dedup gate precedes the dstOffset bounds guard too, so this is ACKed
		// rather than dropped. That ordering is part of the parity contract.
		name: "dup_parked_other_content", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 1, size: 2, declared: -1, off: deriveOff},
			{idx: 1, size: 2, declared: -1, off: deriveOff, marker: 'Z'},
			{idx: 0, size: 2, declared: -1, off: deriveOff},
		},
		want: "dup_parked_other_content|start=N|steps=NNN|del=1|data=41414242",
	},
	{
		name: "dup_parked_larger_size", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 1, size: 2, declared: -1, off: deriveOff},
			{idx: 1, size: 99, declared: -1, off: deriveOff, marker: 'Z'},
			{idx: 0, size: 2, declared: -1, off: deriveOff},
		},
		want: "dup_parked_larger_size|start=N|steps=NNN|del=1|data=41414242",
	},
	{
		// Zero-length chunk is counted exactly once (presence, not emptiness).
		// Chunk 1 carries all 3 bytes, so its dstOffset is 0, not 1*3.
		name: "zero_len_then_dup", totalSize: 3, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 0, declared: -1, off: 0},
			{idx: 0, size: 0, declared: -1, off: 0},
			{idx: 1, size: 3, declared: -1, off: 0},
		},
		want: "zero_len_then_dup|start=N|steps=NNN|del=1|data=424242",
	},
	{
		// totalSize == 0 with totalChunks > 0 is not the "empty stream"
		// shortcut: the context is created and completes once all zero-length
		// chunks arrive. Every dstOffset is 0 — the only legal value here.
		name: "all_zero_len_zero_total", totalSize: 0, totalChunks: 3,
		steps: []parityStep{
			{idx: 0, size: 0, declared: -1, off: 0},
			{idx: 2, size: 0, declared: -1, off: 0},
			{idx: 1, size: 0, declared: -1, off: 0},
		},
		want: "all_zero_len_zero_total|start=N|steps=NNN|del=1|data=empty",
	},
	{
		// Under-sized completion: every index filled but Σ payloadSize < totalSize.
		name: "undersize_completion_drops", totalSize: 6, totalChunks: 2,
		steps: []parityStep{{idx: 0, size: 2, declared: -1, off: deriveOff}, {idx: 1, size: 2, declared: -1, off: deriveOff}},
		want:  "undersize_completion_drops|start=N|steps=NE|del=0|data=-",
	},
	{
		name: "oversize_in_order_drops", totalSize: 3, totalChunks: 2,
		steps: []parityStep{{idx: 0, size: 2, declared: -1, off: deriveOff}, {idx: 1, size: 2, declared: -1, off: deriveOff}},
		want:  "oversize_in_order_drops|start=N|steps=NE|del=0|data=-",
	},
	{
		// Accumulated parked bytes overflow; no single chunk does. The two
		// parked chunks are placed at in-bounds offsets (4 and 0) so the
		// rejection is unambiguously the running-length guard and not the
		// dstOffset bounds guard.
		name: "ooo_accumulated_overflow_drops", totalSize: 8, totalChunks: 4,
		steps: []parityStep{
			{idx: 3, size: 4, declared: -1, off: 4},
			{idx: 2, size: 4, declared: -1, off: 0},
			{idx: 1, size: 1, declared: -1, off: 0},
		},
		want: "ooo_accumulated_overflow_drops|start=N|steps=NNE|del=0|data=-",
	},
	{
		// Tiny advertisement, huge ahead-of-cursor chunk: refused at arrival,
		// and the stream is gone so the next chunk is refused as unknown.
		name: "ooo_first_overflow_drops", totalSize: 1, totalChunks: 1000,
		steps: []parityStep{
			{idx: 1, size: 4096, declared: -1, off: 0},
			{idx: 2, size: 4096, declared: -1, off: 0},
		},
		want: "ooo_first_overflow_drops|start=N|steps=EE|del=0|data=-",
	},
	{
		// A zero-length chunk parked at the very end of the advertisement
		// contributes nothing; the 5-byte chunk that follows overflows it.
		name: "zero_len_park_then_overflow_drops", totalSize: 4, totalChunks: 3,
		steps: []parityStep{
			{idx: 2, size: 0, declared: -1, off: 4},
			{idx: 1, size: 5, declared: -1, off: 0},
		},
		want: "zero_len_park_then_overflow_drops|start=N|steps=NE|del=0|data=-",
	},
	{
		// Exact fill is accepted (the guard is `>`, not `>=`). Chunk 0 is
		// zero-length, so chunk 1 legitimately starts at offset 0.
		name: "exact_fill_parked_first", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 1, size: 4, declared: -1, off: 0},
			{idx: 0, size: 0, declared: -1, off: 0},
		},
		want: "exact_fill_parked_first|start=N|steps=NN|del=1|data=42424242",
	},
	{
		// Out-of-range chunkIndex is rejected but does NOT drop the stream:
		// the remaining valid chunks still complete it.
		name: "index_out_of_range_keeps_stream", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 2, size: 2, declared: -1, off: deriveOff},
			{idx: 0, size: 2, declared: -1, off: deriveOff},
			{idx: 1, size: 2, declared: -1, off: deriveOff},
		},
		want: "index_out_of_range_keeps_stream|start=N|steps=ENN|del=1|data=41414242",
	},
	{
		// Framing violation (declared payloadSize exceeds the bytes supplied)
		// is rejected before the stream is even looked up, so the stream
		// survives and completes.
		name: "truncated_framing_keeps_stream", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: 4, off: deriveOff},
			{idx: 0, size: 2, declared: -1, off: deriveOff},
			{idx: 1, size: 2, declared: -1, off: deriveOff},
		},
		want: "truncated_framing_keeps_stream|start=N|steps=ENN|del=1|data=41414242",
	},
	{
		// A chunk for a stream id that was never started is rejected and does
		// not disturb the live stream.
		name: "unknown_stream_id_keeps_stream", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1, off: deriveOff, otherID: true},
			{idx: 0, size: 2, declared: -1, off: deriveOff},
			{idx: 1, size: 2, declared: -1, off: deriveOff},
		},
		want: "unknown_stream_id_keeps_stream|start=N|steps=ENN|del=1|data=41414242",
	},
	{
		// A repeated STREAM_START for the same id replaces the context: the
		// already-consumed chunk 0 is accepted again and the stream completes
		// exactly once.
		name: "restart_replaces_context", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1, off: deriveOff},
			{restart: true, idx: 0, size: 2, declared: -1, off: deriveOff},
			{idx: 1, size: 2, declared: -1, off: deriveOff},
		},
		want: "restart_replaces_context|start=N|steps=NsNNN|del=1|data=41414242",
	},
	{
		// Single chunk that exactly fills the advertisement.
		name: "single_chunk_exact_fill", totalSize: 3, totalChunks: 1,
		steps: []parityStep{{idx: 0, size: 3, declared: -1, off: deriveOff}},
		want:  "single_chunk_exact_fill|start=N|steps=N|del=1|data=414141",
	},

	// ---- SHM_VERSION 0x00080000: ChunkHeader.dstOffset cases ----
	//
	// These replace TestStreamReassembly_ReservedFieldsAreInert, whose own
	// comment required exactly that once reserved@16 acquired meaning.

	{
		// A stream of UNEQUAL chunk sizes delivered back-to-front still
		// assembles and completes: chunk 0 is 1 byte at 0, chunk 1 is 2 at 1,
		// chunk 2 is 3 at 3, so dstOffset is idx*size for no index and a
		// receiver that derived the offset from the index instead of reading the
		// field cannot land chunk 2 at 3. It also exercises the park/drain path
		// under non-uniform sizes, where an off-by-one in the oooBytes
		// release/re-account pair would show up as a spurious drop.
		//
		// WHAT IT DOES NOT PIN: that the payload is written to buf[dstOffset]
		// rather than appended at the receiver's own cursor. A cursor-appending
		// receiver produces this exact digest — not by coincidence of the arrival
		// order, but necessarily, because dstOffset is required to equal the
		// cursor whenever an index becomes in-order (see this file's header).
		// This case is a regression guard on assembly and completion; the
		// placement discriminator lives in
		// go/stream_dstoffset_placement_test.go.
		name: "nonuniform_offsets_reverse_completes", totalSize: 6, totalChunks: 3,
		steps: []parityStep{
			{idx: 2, size: 3, declared: -1, off: 3},
			{idx: 1, size: 2, declared: -1, off: 1},
			{idx: 0, size: 1, declared: -1, off: 0},
		},
		want: "nonuniform_offsets_reverse_completes|start=N|steps=NNN|del=1|data=414242434343",
	},
	{
		// CONTRADICTORY dstOffset, caught on the immediately-in-order arrival:
		// chunk 1 is in-bounds and fits the advertisement, but declares offset 1
		// when the cursor stands at 2. The peer contradicted itself, so the
		// stream is DROPPED — the third step proves the drop (it is answered as
		// an unknown stream, not merely as a rejected chunk).
		name: "misplaced_in_order_drops", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1, off: 0},
			{idx: 1, size: 2, declared: -1, off: 1},
			{idx: 1, size: 2, declared: -1, off: 2},
		},
		want: "misplaced_in_order_drops|start=N|steps=NEE|del=0|data=-",
	},
	{
		// Same contradiction, but detected on the DRAIN path: chunk 1 arrives
		// first and parks at offset 1 (in-bounds, fits), and only when chunk 0
		// advances the cursor to 2 does the mismatch become visible. Both
		// detection sites must drop the stream.
		name: "misplaced_parked_drops_on_drain", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 1, size: 2, declared: -1, off: 1},
			{idx: 0, size: 2, declared: -1, off: 0},
			{idx: 0, size: 2, declared: -1, off: 0},
		},
		want: "misplaced_parked_drops_on_drain|start=N|steps=NEE|del=0|data=-",
	},
	{
		// dstOffset strictly past the end of the destination.
		name: "dstoffset_past_end_drops", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1, off: 5},
			{idx: 0, size: 2, declared: -1, off: 0},
		},
		want: "dstoffset_past_end_drops|start=N|steps=EE|del=0|data=-",
	},
	{
		// dstOffset itself is in bounds but dstOffset+payloadSize overhangs.
		name: "dstoffset_overhang_drops", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1, off: 3},
			{idx: 0, size: 2, declared: -1, off: 0},
		},
		want: "dstoffset_overhang_drops|start=N|steps=EE|del=0|data=-",
	},
	{
		// The same overhang on the ahead-of-cursor (park) path: the bounds
		// guard is not an in-order-only check.
		name: "dstoffset_overhang_parked_drops", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 1, size: 2, declared: -1, off: 4},
			{idx: 0, size: 2, declared: -1, off: 0},
		},
		want: "dstoffset_overhang_parked_drops|start=N|steps=EE|del=0|data=-",
	},
	{
		// WRAP. dstOffset = 0xFFFFFFFE with payloadSize 4 sums to 2 in 32-bit
		// arithmetic, so a receiver that tests `dstOffset + payloadSize <=
		// totalSize` in the field's own width accepts it and then writes 4 GiB
		// past the buffer. The check must SUBTRACT (`payloadSize <= totalSize -
		// dstOffset` after proving `dstOffset <= totalSize`) or widen both
		// operands. The wire field is u32, so 64-bit wrap is unreachable from
		// the wire — widening to u64 is what makes it unreachable.
		name: "dstoffset_wrap32_drops", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 4, declared: -1, off: 0xFFFFFFFE},
			{idx: 0, size: 4, declared: -1, off: 0},
		},
		want: "dstoffset_wrap32_drops|start=N|steps=EE|del=0|data=-",
	},
	{
		// The extreme of the same family: every bit set.
		name: "dstoffset_max_u32_drops", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 0, size: 2, declared: -1, off: 0xFFFFFFFF},
			{idx: 0, size: 2, declared: -1, off: 0},
		},
		want: "dstoffset_max_u32_drops|start=N|steps=EE|del=0|data=-",
	},
	{
		// A zero-length chunk pointed out of bounds is still rejected: the
		// bounds guard must not be short-circuited by an empty payload (a
		// `len(data) > 0` fast path would let a nonsense offset through and
		// leave it in the ooo map as a future drain-time contradiction).
		name: "dstoffset_zero_len_past_end_drops", totalSize: 4, totalChunks: 2,
		steps: []parityStep{
			{idx: 1, size: 0, declared: -1, off: 5},
			{idx: 0, size: 4, declared: -1, off: 0},
		},
		want: "dstoffset_zero_len_past_end_drops|start=N|steps=EE|del=0|data=-",
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

// TestStreamReassembly_AppMsgTypeIsInertToReassembly is a TRIPWIRE, not a
// feature test. The C++ half carries the identical check at the end of
// tests/test_reassembler_parity_table.cpp.
//
// StreamHeader.appMsgType@20 (formerly reserved@20; named as of SHM_VERSION
// 0x00080000) is an application-defined routing tag that is OPAQUE to shm.
// Senders write it — Go StreamSender.SetAppMsgType, C++ StreamSender::Send's
// appMsgType parameter — and the wire slot was claimed at the 0x00080000 bump
// specifically so that a future receive-side API needs no second bump.
// SURFACING IT TO THE STREAM HANDLER IS DEFERRED AND OUT OF SCOPE. So the one
// checkable property today is exactly this: the value MUST NOT alter
// reassembly. Every parity case is replayed with the field set to garbage and
// every digest must be byte-identical.
//
// Note what is NOT asserted here any more, and why. Its predecessor,
// TestStreamReassembly_ReservedFieldsAreInert, also replayed the table with
// ChunkHeader@16 set to garbage. That field is now dstOffset and is
// LOAD-BEARING, so an equivalent check would be asserting the opposite of the
// contract; the dstOffset cases at the end of parityCases replace it.
//
// If you are here because this failed: you are making a wire change. When
// appMsgType is finally surfaced to the receive side it stops being inert, and
// at that point bump SHM_VERSION only if the *wire* changes (it does not — the
// slot is already claimed), but do update SPECIFICATION.md §3.3.1, both parity
// halves, and replace this tripwire with the routing tag's own cases. Do not
// "fix" it by loosening the comparison.
func TestStreamReassembly_AppMsgTypeIsInertToReassembly(t *testing.T) {
	const garbageAppMsgType = uint32(0xCAFEBABE)
	for i, tc := range parityCases {
		t.Run(tc.name, func(t *testing.T) {
			plain := runParityCaseApp(tc, uint64(5000+i), 0)
			dirty := runParityCaseApp(tc, uint64(5000+i), garbageAppMsgType)
			if plain != dirty {
				t.Errorf("appMsgType is NOT inert to reassembly -- this is a WIRE BEHAVIOR CHANGE "+
					"(see the comment above this test)\n  appMsgType=0:       %s\n  appMsgType=garbage: %s",
					plain, dirty)
			}
			// The plain replay must also still match the frozen digest, so a
			// change that perturbs BOTH replays identically cannot hide here.
			if plain != tc.want {
				t.Errorf("digest mismatch on the appMsgType=0 replay\n got: %s\nwant: %s", plain, tc.want)
			}
		})
	}
}
