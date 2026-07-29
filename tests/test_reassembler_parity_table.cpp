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

// One STREAM_CHUNK delivery (optionally preceded by a repeated STREAM_START
// for the same stream). `declared < 0` means "declared payloadSize == size".
// `marker == 0` means "fill with 'A' + idx".
struct Step {
    bool restart;
    uint32_t idx;
    size_t size;
    int declared;
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
Step chunk(uint32_t idx, size_t size) { return Step{false, idx, size, -1, false, 0}; }
Step chunkMarked(uint32_t idx, size_t size, uint8_t marker) {
    return Step{false, idx, size, -1, false, marker};
}
Step chunkDeclared(uint32_t idx, size_t size, int declared) {
    return Step{false, idx, size, declared, false, 0};
}
Step chunkOtherId(uint32_t idx, size_t size) { return Step{false, idx, size, -1, true, 0}; }
Step restartThenChunk(uint32_t idx, size_t size) { return Step{true, idx, size, -1, false, 0}; }

std::vector<uint8_t> startReq(uint64_t streamId, uint64_t totalSize,
                              uint32_t totalChunks) {
    StreamHeader h;
    h.streamId = streamId;
    h.totalSize = totalSize;
    h.totalChunks = totalChunks;
    h.reserved = 0;
    std::vector<uint8_t> buf(sizeof(StreamHeader));
    std::memcpy(buf.data(), &h, sizeof(h));
    return buf;
}

// Builds a STREAM_CHUNK request whose declared payloadSize may deliberately
// disagree with the number of bytes supplied (declared > size exercises the
// framing guard).
std::vector<uint8_t> chunkReq(uint64_t streamId, uint32_t idx, uint32_t declared,
                              const std::vector<uint8_t>& payload) {
    ChunkHeader h;
    h.streamId = streamId;
    h.chunkIndex = idx;
    h.payloadSize = declared;
    h.reserved = 0;
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

std::string runCase(const Case& tc, uint64_t streamId) {
    std::vector<std::vector<uint8_t>> delivered;
    StreamReassembler r([&](uint64_t, const std::vector<uint8_t>& d) {
        delivered.push_back(d);
    });

    auto start = [&]() {
        return replyChar(
            handle(r, MsgType::STREAM_START,
                   startReq(streamId, tc.totalSize, tc.totalChunks)));
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
        uint64_t id = st.otherId ? (streamId ^ 0xDEADull) : streamId;
        steps += replyChar(handle(r, MsgType::STREAM_CHUNK,
                                  chunkReq(id, st.idx, declared, payload)));
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

        {"dup_parked_larger_size", 4, 2,
         {chunk(1, 2), chunkMarked(1, 99, 'Z'), chunk(0, 2)},
         "dup_parked_larger_size|start=N|steps=NNN|del=1|data=41414242"},

        // Zero-length chunk is counted exactly once (presence, not emptiness).
        {"zero_len_then_dup", 3, 2, {chunk(0, 0), chunk(0, 0), chunk(1, 3)},
         "zero_len_then_dup|start=N|steps=NNN|del=1|data=424242"},

        // totalSize == 0 with totalChunks > 0 is not the "empty stream"
        // shortcut: the context is created and completes once all zero-length
        // chunks arrive.
        {"all_zero_len_zero_total", 0, 3, {chunk(0, 0), chunk(2, 0), chunk(1, 0)},
         "all_zero_len_zero_total|start=N|steps=NNN|del=1|data=empty"},

        // Under-sized completion: every index filled but Σ payloadSize < totalSize.
        {"undersize_completion_drops", 6, 2, {chunk(0, 2), chunk(1, 2)},
         "undersize_completion_drops|start=N|steps=NE|del=0|data=-"},

        {"oversize_in_order_drops", 3, 2, {chunk(0, 2), chunk(1, 2)},
         "oversize_in_order_drops|start=N|steps=NE|del=0|data=-"},

        // Accumulated parked bytes overflow; no single chunk does.
        {"ooo_accumulated_overflow_drops", 8, 4,
         {chunk(3, 4), chunk(2, 4), chunk(1, 1)},
         "ooo_accumulated_overflow_drops|start=N|steps=NNE|del=0|data=-"},

        // Tiny advertisement, huge ahead-of-cursor chunk: refused at arrival,
        // and the stream is gone so the next chunk is refused as unknown.
        {"ooo_first_overflow_drops", 1, 1000, {chunk(1, 4096), chunk(2, 4096)},
         "ooo_first_overflow_drops|start=N|steps=EE|del=0|data=-"},

        {"zero_len_park_then_overflow_drops", 4, 3, {chunk(2, 0), chunk(1, 5)},
         "zero_len_park_then_overflow_drops|start=N|steps=NE|del=0|data=-"},

        // Exact fill is accepted (the guard is `>`, not `>=`).
        {"exact_fill_parked_first", 4, 2, {chunk(1, 4), chunk(0, 0)},
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

    if (g_failures == 0) {
        std::cout << "Test Passed" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " check(s) failed" << std::endl;
    return 1;
}
