// C++ half of the SPEC §3.3.4 accept/reject parity table. The Go half is
// go/stream_parity_table_test.go; the case list and the expected digests are
// duplicated verbatim in both files on purpose. Each side asserts against the
// literal digest, so a divergence in either reassembler shows up as a mismatch
// against a constant rather than against the other implementation's current
// behavior (which could drift in lockstep). Run
// `go test ./go/ -run ParityTableDump -v` and this binary and diff the printed
// lines for a mechanical cross-check.
//
// The table freezes the *observable* contract across internal storage reworks.
// It pins behavior the SPEC prose does not spell out chunk-by-chunk:
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
//     honoured as the ABSOLUTE destination of the payload, is bounds-checked
//     against totalSize with wrap-safe arithmetic, and MUST equal the in-order
//     cursor at the moment its index becomes in-order.
//
// Do not "fix" a digest to make a change pass. A digest change is a wire
// behavior change and must be reflected in SPECIFICATION.md §3.3.4 and in the
// Go half in the same commit.

#include <shm/StreamReassembler.h>
#include <shm/Stream.h>
#include <shm/IPCUtils.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace shm;

namespace {

int g_failures = 0;

// kDeriveOff is the Step::off sentinel: derive ChunkHeader.dstOffset as
// idx*size. Most cases here send uniform 2-byte payloads, for which that is
// exactly what a conforming sender writes. It is a NEGATIVE sentinel, not the
// zero value, because 0 is a perfectly legal dstOffset — a step that forgot to
// set the field must not silently become "offset 0".
const int64_t kDeriveOff = -1;

// One STREAM_CHUNK delivery (optionally preceded by a repeated STREAM_START
// for the same stream). `declared < 0` means "declared payloadSize == size".
// `marker == 0` means "fill with 'A' + idx".
struct Step {
    bool restart;
    uint32_t idx;
    size_t size;
    int declared;
    int64_t off;      ///< ChunkHeader.dstOffset; kDeriveOff means idx*size
    bool otherId;
    uint8_t marker;
};

struct Case {
    const char* name;
    uint64_t totalSize;
    uint32_t totalChunks;
    std::vector<Step> steps;
    const char* want;
};

// Convenience constructors keeping the case table readable.
Step chunk(uint32_t idx, size_t size) {
    return Step{false, idx, size, -1, kDeriveOff, false, 0};
}
Step chunkAt(uint32_t idx, size_t size, int64_t off) {
    return Step{false, idx, size, -1, off, false, 0};
}
Step chunkMarked(uint32_t idx, size_t size, uint8_t marker) {
    return Step{false, idx, size, -1, kDeriveOff, false, marker};
}
Step chunkDeclared(uint32_t idx, size_t size, int declared) {
    return Step{false, idx, size, declared, kDeriveOff, false, 0};
}
Step chunkOtherId(uint32_t idx, size_t size) {
    return Step{false, idx, size, -1, kDeriveOff, true, 0};
}
Step restartThenChunk(uint32_t idx, size_t size) {
    return Step{true, idx, size, -1, kDeriveOff, false, 0};
}

std::vector<uint8_t> startReq(uint64_t streamId, uint64_t totalSize,
                              uint32_t totalChunks, uint32_t appMsgType = 0) {
    StreamHeader h;
    h.streamId = streamId;
    h.totalSize = totalSize;
    h.totalChunks = totalChunks;
    h.appMsgType = appMsgType;
    std::vector<uint8_t> buf(sizeof(StreamHeader));
    std::memcpy(buf.data(), &h, sizeof(h));
    return buf;
}

// Builds a STREAM_CHUNK request whose declared payloadSize may deliberately
// disagree with the number of bytes supplied (declared > size exercises the
// framing guard) and whose dstOffset is written verbatim, so a case can
// misplace a chunk or point it out of bounds on purpose.
std::vector<uint8_t> chunkReq(uint64_t streamId, uint32_t idx, uint32_t declared,
                              uint32_t dstOffset,
                              const std::vector<uint8_t>& payload) {
    ChunkHeader h;
    h.streamId = streamId;
    h.chunkIndex = idx;
    h.payloadSize = declared;
    h.dstOffset = dstOffset;
    h.padding = 0;
    std::vector<uint8_t> buf(sizeof(ChunkHeader) + payload.size());
    std::memcpy(buf.data(), &h, sizeof(h));
    if (!payload.empty()) {
        std::memcpy(buf.data() + sizeof(h), payload.data(), payload.size());
    }
    return buf;
}

MsgType handle(StreamReassembler& r, MsgType type,
               const std::vector<uint8_t>& req) {
    uint8_t resp[64];
    size_t respSize = 0;
    MsgType mt = type;
    if (!r.Handle(req.data(), req.size(), resp, respSize, mt)) {
        std::cerr << "FAIL: Handle did not claim a stream message" << std::endl;
        ++g_failures;
    }
    return mt;
}

const char* replyChar(MsgType mt) {
    if (mt == MsgType::NORMAL) return "N";
    if (mt == MsgType::SYSTEM_ERROR) return "E";
    return "?";
}

std::string toHexUpper(const std::vector<uint8_t>& d) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(d.size() * 2);
    for (uint8_t b : d) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0xF]);
    }
    return out;
}

// Drives one case with StreamHeader.appMsgType set to appMsgType. appMsgType is
// opaque to shm and MUST NOT change the digest.
std::string runCase(const Case& tc, uint64_t streamId, uint32_t appMsgType = 0) {
    std::vector<std::vector<uint8_t>> delivered;
    StreamReassembler r([&](uint64_t, const std::vector<uint8_t>& d) {
        delivered.push_back(d);
    });

    auto start = [&]() {
        return replyChar(
            handle(r, MsgType::STREAM_START,
                   startReq(streamId, tc.totalSize, tc.totalChunks, appMsgType)));
    };

    std::string startReply = start();

    std::string steps;
    for (const Step& st : tc.steps) {
        if (st.restart) {
            steps += "s";
            steps += start();
        }
        uint8_t marker = st.marker != 0
                             ? st.marker
                             : static_cast<uint8_t>('A' + st.idx);
        std::vector<uint8_t> payload(st.size, marker);
        uint32_t declared = st.declared < 0
                                ? static_cast<uint32_t>(st.size)
                                : static_cast<uint32_t>(st.declared);
        int64_t off = st.off == kDeriveOff
                          ? static_cast<int64_t>(st.idx) * static_cast<int64_t>(st.size)
                          : st.off;
        uint64_t id = st.otherId ? (streamId ^ 0xDEADull) : streamId;
        steps += replyChar(handle(r, MsgType::STREAM_CHUNK,
                                  chunkReq(id, st.idx, declared,
                                           static_cast<uint32_t>(off), payload)));
    }

    std::string data = "-";
    if (!delivered.empty()) {
        data = toHexUpper(delivered.back());
        if (data.empty()) data = "empty";
    }

    std::ostringstream os;
    os << tc.name << "|start=" << startReply << "|steps=" << steps
       << "|del=" << delivered.size() << "|data=" << data;
    return os.str();
}

// Duplicated verbatim (modulo syntax) in go/stream_parity_table_test.go.
std::vector<Case> cases() {
    return {
        {"in_order", 4, 2, {chunk(0, 2), chunk(1, 2)},
         "in_order|start=N|steps=NN|del=1|data=41414242"},

        {"reverse", 4, 2, {chunk(1, 2), chunk(0, 2)},
         "reverse|start=N|steps=NN|del=1|data=41414242"},

        // Duplicate of an already-consumed index: ACK-ignored.
        {"dup_consumed_same_size", 4, 2, {chunk(0, 2), chunk(0, 2), chunk(1, 2)},
         "dup_consumed_same_size|start=N|steps=NNN|del=1|data=41414242"},

        // First arrival wins: the duplicate's bytes are discarded, not merged.
        {"dup_consumed_other_content", 4, 2,
         {chunk(0, 2), chunkMarked(0, 2, 'Z'), chunk(1, 2)},
         "dup_consumed_other_content|start=N|steps=NNN|del=1|data=41414242"},

        // A duplicate that declares a *larger* payloadSize than the first
        // arrival is still ACK-ignored: the dedup gate precedes the
        // running-length guard, so the 99 extra bytes are never counted and the
        // stream is not dropped for overflow.
        {"dup_consumed_larger_size", 4, 2,
         {chunk(0, 2), chunkMarked(0, 99, 'Z'), chunk(1, 2)},
         "dup_consumed_larger_size|start=N|steps=NNN|del=1|data=41414242"},

        // Same, for an index still parked ahead of the cursor.
        {"dup_parked_other_content", 4, 2,
         {chunk(1, 2), chunkMarked(1, 2, 'Z'), chunk(0, 2)},
         "dup_parked_other_content|start=N|steps=NNN|del=1|data=41414242"},

        // The duplicate's derived dstOffset is 1*99 = 99, far past totalSize=4:
        // the dedup gate precedes the dstOffset bounds guard too, so this is
        // ACKed rather than dropped. That ordering is part of the parity
        // contract — a reassembler that bounds-checks before dedup flips this
        // case's second step from N to E.
        {"dup_parked_larger_size", 4, 2,
         {chunk(1, 2), chunkMarked(1, 99, 'Z'), chunk(0, 2)},
         "dup_parked_larger_size|start=N|steps=NNN|del=1|data=41414242"},

        // Zero-length chunk is counted exactly once (presence, not emptiness).
        // Chunk 1 carries all 3 bytes, so its dstOffset is 0, not 1*3.
        {"zero_len_then_dup", 3, 2,
         {chunkAt(0, 0, 0), chunkAt(0, 0, 0), chunkAt(1, 3, 0)},
         "zero_len_then_dup|start=N|steps=NNN|del=1|data=424242"},

        // totalSize == 0 with totalChunks > 0 is not the "empty stream"
        // shortcut: the context is created and completes once all zero-length
        // chunks arrive. Every dstOffset is 0 — the only legal value here.
        {"all_zero_len_zero_total", 0, 3,
         {chunkAt(0, 0, 0), chunkAt(2, 0, 0), chunkAt(1, 0, 0)},
         "all_zero_len_zero_total|start=N|steps=NNN|del=1|data=empty"},

        // Under-sized completion: every index filled but Σ payloadSize < totalSize.
        {"undersize_completion_drops", 6, 2, {chunk(0, 2), chunk(1, 2)},
         "undersize_completion_drops|start=N|steps=NE|del=0|data=-"},

        {"oversize_in_order_drops", 3, 2, {chunk(0, 2), chunk(1, 2)},
         "oversize_in_order_drops|start=N|steps=NE|del=0|data=-"},

        // Accumulated parked bytes overflow; no single chunk does. The two
        // parked chunks are placed at in-bounds offsets (4 and 0) so the
        // rejection is unambiguously the running-length guard and not the
        // dstOffset bounds guard.
        {"ooo_accumulated_overflow_drops", 8, 4,
         {chunkAt(3, 4, 4), chunkAt(2, 4, 0), chunkAt(1, 1, 0)},
         "ooo_accumulated_overflow_drops|start=N|steps=NNE|del=0|data=-"},

        // Tiny advertisement, huge ahead-of-cursor chunk: refused at arrival,
        // and the stream is gone so the next chunk is refused as unknown.
        {"ooo_first_overflow_drops", 1, 1000,
         {chunkAt(1, 4096, 0), chunkAt(2, 4096, 0)},
         "ooo_first_overflow_drops|start=N|steps=EE|del=0|data=-"},

        // A zero-length chunk parked at the very end of the advertisement
        // contributes nothing; the 5-byte chunk that follows overflows it.
        {"zero_len_park_then_overflow_drops", 4, 3,
         {chunkAt(2, 0, 4), chunkAt(1, 5, 0)},
         "zero_len_park_then_overflow_drops|start=N|steps=NE|del=0|data=-"},

        // Exact fill is accepted (the guard is `>`, not `>=`). Chunk 0 is
        // zero-length, so chunk 1 legitimately starts at offset 0.
        {"exact_fill_parked_first", 4, 2, {chunkAt(1, 4, 0), chunkAt(0, 0, 0)},
         "exact_fill_parked_first|start=N|steps=NN|del=1|data=42424242"},

        // Out-of-range chunkIndex is rejected but does NOT drop the stream:
        // the remaining valid chunks still complete it.
        {"index_out_of_range_keeps_stream", 4, 2,
         {chunk(2, 2), chunk(0, 2), chunk(1, 2)},
         "index_out_of_range_keeps_stream|start=N|steps=ENN|del=1|data=41414242"},

        // Framing violation (declared payloadSize exceeds the bytes supplied)
        // is rejected before the stream is even looked up, so the stream
        // survives and completes.
        {"truncated_framing_keeps_stream", 4, 2,
         {chunkDeclared(0, 2, 4), chunk(0, 2), chunk(1, 2)},
         "truncated_framing_keeps_stream|start=N|steps=ENN|del=1|data=41414242"},

        // A chunk for a stream id that was never started is rejected and does
        // not disturb the live stream.
        {"unknown_stream_id_keeps_stream", 4, 2,
         {chunkOtherId(0, 2), chunk(0, 2), chunk(1, 2)},
         "unknown_stream_id_keeps_stream|start=N|steps=ENN|del=1|data=41414242"},

        // A repeated STREAM_START for the same id replaces the context: the
        // already-consumed chunk 0 is accepted again and the stream completes
        // exactly once.
        {"restart_replaces_context", 4, 2,
         {chunk(0, 2), restartThenChunk(0, 2), chunk(1, 2)},
         "restart_replaces_context|start=N|steps=NsNNN|del=1|data=41414242"},

        // Single chunk that exactly fills the advertisement.
        {"single_chunk_exact_fill", 3, 1, {chunk(0, 3)},
         "single_chunk_exact_fill|start=N|steps=N|del=1|data=414141"},

        // ---- SHM_VERSION 0x00080000: ChunkHeader.dstOffset cases ----
        //
        // These replace the reserved-fields-are-inert tripwire, whose own
        // comment required exactly that once reserved@16 acquired meaning.

        // dstOffset is HONOURED, not merely tolerated: a stream of unequal
        // chunk sizes delivered back-to-front assembles at the offsets the
        // sender declared (1 byte at 0, 2 at 1, 3 at 3). A receiver that
        // ignored dstOffset and appended at its own cursor would produce a
        // DIFFERENT byte string here — which is why the order is reversed.
        {"nonuniform_offsets_reverse_completes", 6, 3,
         {chunkAt(2, 3, 3), chunkAt(1, 2, 1), chunkAt(0, 1, 0)},
         "nonuniform_offsets_reverse_completes|start=N|steps=NNN|del=1|data=414242434343"},

        // CONTRADICTORY dstOffset, caught on the immediately-in-order arrival:
        // chunk 1 is in-bounds and fits the advertisement, but declares offset 1
        // when the cursor stands at 2. The peer contradicted itself, so the
        // stream is DROPPED — the third step proves the drop (it is answered as
        // an unknown stream, not merely as a rejected chunk).
        {"misplaced_in_order_drops", 4, 2,
         {chunkAt(0, 2, 0), chunkAt(1, 2, 1), chunkAt(1, 2, 2)},
         "misplaced_in_order_drops|start=N|steps=NEE|del=0|data=-"},

        // Same contradiction, but detected on the DRAIN path: chunk 1 arrives
        // first and parks at offset 1 (in-bounds, fits), and only when chunk 0
        // advances the cursor to 2 does the mismatch become visible. Both
        // detection sites must drop the stream.
        {"misplaced_parked_drops_on_drain", 4, 2,
         {chunkAt(1, 2, 1), chunkAt(0, 2, 0), chunkAt(0, 2, 0)},
         "misplaced_parked_drops_on_drain|start=N|steps=NEE|del=0|data=-"},

        // dstOffset strictly past the end of the destination.
        {"dstoffset_past_end_drops", 4, 2,
         {chunkAt(0, 2, 5), chunkAt(0, 2, 0)},
         "dstoffset_past_end_drops|start=N|steps=EE|del=0|data=-"},

        // dstOffset itself is in bounds but dstOffset+payloadSize overhangs.
        {"dstoffset_overhang_drops", 4, 2,
         {chunkAt(0, 2, 3), chunkAt(0, 2, 0)},
         "dstoffset_overhang_drops|start=N|steps=EE|del=0|data=-"},

        // The same overhang on the ahead-of-cursor (park) path: the bounds
        // guard is not an in-order-only check.
        {"dstoffset_overhang_parked_drops", 4, 2,
         {chunkAt(1, 2, 4), chunkAt(0, 2, 0)},
         "dstoffset_overhang_parked_drops|start=N|steps=EE|del=0|data=-"},

        // WRAP. dstOffset = 0xFFFFFFFE with payloadSize 4 sums to 2 in 32-bit
        // arithmetic, so a receiver that tests `dstOffset + payloadSize <=
        // totalSize` in the field's own width accepts it and then writes 4 GiB
        // past the buffer. The check must SUBTRACT (`payloadSize <= totalSize -
        // dstOffset` after proving `dstOffset <= totalSize`) or widen both
        // operands. The wire field is u32, so 64-bit wrap is unreachable from
        // the wire — widening to u64 is what makes it unreachable.
        {"dstoffset_wrap32_drops", 4, 2,
         {chunkAt(0, 4, 0xFFFFFFFEll), chunkAt(0, 4, 0)},
         "dstoffset_wrap32_drops|start=N|steps=EE|del=0|data=-"},

        // The extreme of the same family: every bit set.
        {"dstoffset_max_u32_drops", 4, 2,
         {chunkAt(0, 2, 0xFFFFFFFFll), chunkAt(0, 2, 0)},
         "dstoffset_max_u32_drops|start=N|steps=EE|del=0|data=-"},

        // A zero-length chunk pointed out of bounds is still rejected: the
        // bounds guard must not be short-circuited by an empty payload (an
        // `if (n > 0)` fast path would let a nonsense offset through and leave
        // it in the parked map as a future drain-time contradiction).
        {"dstoffset_zero_len_past_end_drops", 4, 2,
         {chunkAt(1, 0, 5), chunkAt(0, 4, 0)},
         "dstoffset_zero_len_past_end_drops|start=N|steps=EE|del=0|data=-"},
    };
}

} // namespace

int main() {
    const std::vector<Case> table = cases();
    for (size_t i = 0; i < table.size(); ++i) {
        std::string got = runCase(table[i], 1000 + i);
        std::cout << got << std::endl;
        if (got != table[i].want) {
            std::cerr << "FAIL: digest mismatch\n got: " << got
                      << "\nwant: " << table[i].want << std::endl;
            ++g_failures;
        }
    }

    // ---------------------------------------------------------------------
    // appMsgType IS INERT TO REASSEMBLY. This is a TRIPWIRE, not a feature test.
    // ---------------------------------------------------------------------
    // The Go half carries the identical check as
    // TestStreamReassembly_AppMsgTypeIsInertToReassembly.
    //
    // StreamHeader.appMsgType@20 (formerly reserved@20; named as of SHM_VERSION
    // 0x00080000) is an application-defined routing tag that is OPAQUE to shm.
    // Senders write it — StreamSender::Send's appMsgType parameter here, Go
    // StreamSender.SetAppMsgType there — and the wire slot was claimed at the
    // 0x00080000 bump specifically so that a future receive-side API needs no
    // second bump. SURFACING IT TO THE STREAM HANDLER IS DEFERRED AND OUT OF
    // SCOPE. So the one checkable property today is exactly this: the value MUST
    // NOT alter reassembly. Every parity case is replayed with the field set to
    // garbage and every digest must be byte-identical.
    //
    // Note what is NOT asserted here any more, and why. The predecessor of this
    // block also replayed the table with ChunkHeader@16 set to garbage. That
    // field is now dstOffset and is LOAD-BEARING, so an equivalent check would
    // be asserting the opposite of the contract; the dstOffset cases at the end
    // of the table above replace it.
    //
    // If you are here because this failed: you are making a wire change. When
    // appMsgType is finally surfaced to the receive side it stops being inert,
    // and at that point bump SHM_VERSION only if the *wire* changes (it does not
    // — the slot is already claimed), but do update SPECIFICATION.md §3.3.1,
    // both parity halves, and replace this tripwire with the routing tag's own
    // cases. Do not "fix" it by loosening the comparison.
    {
        const uint32_t kGarbageAppMsgType = 0xCAFEBABEu;
        for (size_t i = 0; i < table.size(); ++i) {
            std::string plain = runCase(table[i], 5000 + i, 0);
            std::string dirty = runCase(table[i], 5000 + i, kGarbageAppMsgType);
            if (plain != dirty) {
                std::cerr << "FAIL: appMsgType is NOT inert to reassembly for case '"
                          << table[i].name << "'" << std::endl;
                std::cerr << "  appMsgType=0:       " << plain << std::endl;
                std::cerr << "  appMsgType=garbage: " << dirty << std::endl;
                std::cerr << "  This is a WIRE BEHAVIOR CHANGE. See the comment above "
                             "this check." << std::endl;
                ++g_failures;
            }
            // The plain replay must also still match the frozen digest, so a
            // change that perturbs BOTH replays identically cannot hide here.
            if (plain != table[i].want) {
                std::cerr << "FAIL: digest mismatch on the appMsgType=0 replay\n got: "
                          << plain << "\nwant: " << table[i].want << std::endl;
                ++g_failures;
            }
        }
    }

    if (g_failures == 0) {
        std::cout << "Test Passed" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " check(s) failed" << std::endl;
    return 1;
}
