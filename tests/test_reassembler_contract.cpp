// Unit test for the StreamReassembler reassembly limits & completion contract
// (SPECIFICATION.md §3.3.4). Drives StreamReassembler::Handle() directly with
// crafted wire buffers — no DirectHost, no Go subprocess — so it is fast and
// deterministic. Verifies the four guards that bring the C++ reassembler into
// parity with the Go reference (go/stream.go):
//   1. totalChunks > maxStreamChunks  -> SYSTEM_ERROR (new guard)
//   2. all chunks arrive but Sum(payload) != totalSize -> SYSTEM_ERROR, no callback
//   3. running assembled length exceeds totalSize -> SYSTEM_ERROR
//   4. totalChunks == 0 && totalSize != 0 -> SYSTEM_ERROR
// plus normal completion (in-order and reverse-order) firing onStream once,
// presence-based dedup parity (zero-length chunk + duplicate, and a plain
// duplicate, must not double-count), and the maxStreams concurrent bound.

#include <shm/StreamReassembler.h>
#include <shm/Stream.h>
#include <shm/IPCUtils.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using namespace shm;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL: " << (msg) << " (" << #cond << ") at line "    \
                      << __LINE__ << std::endl;                                \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Build a STREAM_START request buffer.
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

// Build a STREAM_CHUNK request buffer: header + payload.
std::vector<uint8_t> chunkReq(uint64_t streamId, uint32_t chunkIndex,
                              const std::vector<uint8_t>& payload) {
    ChunkHeader h;
    h.streamId = streamId;
    h.chunkIndex = chunkIndex;
    h.payloadSize = static_cast<uint32_t>(payload.size());
    h.reserved = 0;
    h.padding = 0;
    std::vector<uint8_t> buf(sizeof(ChunkHeader) + payload.size());
    std::memcpy(buf.data(), &h, sizeof(h));
    if (!payload.empty()) {
        std::memcpy(buf.data() + sizeof(h), payload.data(), payload.size());
    }
    return buf;
}

// Drive one message through the reassembler; returns the resulting MsgType.
MsgType handle(StreamReassembler& r, MsgType type,
               const std::vector<uint8_t>& req) {
    uint8_t resp[64];
    size_t respSize = 0;
    MsgType mt = type;
    bool handled = r.Handle(req.data(), req.size(), resp, respSize, mt);
    CHECK(handled, "Handle should claim stream messages");
    return mt;
}

} // namespace

int main() {
    // ---- Guard 2: totalChunks > maxStreamChunks -> SYSTEM_ERROR ----
    // Pre-fix this passed (or OOM'd on resize); post-fix it must be rejected.
    {
        bool fired = false;
        StreamReassembler r(
            [&](uint64_t, const std::vector<uint8_t>&) { fired = true; });
        StreamReassemblerConfig cfg;
        MsgType mt = handle(r, MsgType::STREAM_START,
                            startReq(1, 100, (uint32_t)cfg.maxStreamChunks + 1));
        CHECK(mt == MsgType::SYSTEM_ERROR,
              "totalChunks over maxStreamChunks must be SYSTEM_ERROR");
        CHECK(!fired, "no callback on rejected oversized totalChunks");
    }

    // ---- Guard 3 (completion): Sum(payload) != totalSize -> SYSTEM_ERROR ----
    // All chunks arrive, but advertised totalSize (6) != actual sum (4).
    {
        bool fired = false;
        StreamReassembler r(
            [&](uint64_t, const std::vector<uint8_t>&) { fired = true; });
        CHECK(handle(r, MsgType::STREAM_START, startReq(2, 6, 2)) ==
                  MsgType::NORMAL,
              "valid start ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK,
                     chunkReq(2, 0, {0xAA, 0xBB})) == MsgType::NORMAL,
              "chunk0 ACKs");
        // Final chunk completes count (2/2) but total is 4 != 6.
        MsgType mt =
            handle(r, MsgType::STREAM_CHUNK, chunkReq(2, 1, {0xCC, 0xDD}));
        CHECK(mt == MsgType::SYSTEM_ERROR,
              "size-mismatch completion must be SYSTEM_ERROR (drop)");
        CHECK(!fired, "no callback on size-mismatch drop");
    }

    // ---- Guard 3 (overflow): running length exceeds totalSize ----
    // totalSize 3, but two 2-byte chunks (=4) would overflow.
    {
        bool fired = false;
        StreamReassembler r(
            [&](uint64_t, const std::vector<uint8_t>&) { fired = true; });
        CHECK(handle(r, MsgType::STREAM_START, startReq(3, 3, 2)) ==
                  MsgType::NORMAL,
              "valid start ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK,
                     chunkReq(3, 0, {0x01, 0x02})) == MsgType::NORMAL,
              "chunk0 ACKs");
        MsgType mt =
            handle(r, MsgType::STREAM_CHUNK, chunkReq(3, 1, {0x03, 0x04}));
        CHECK(mt == MsgType::SYSTEM_ERROR,
              "assembled-length overflow must be SYSTEM_ERROR");
        CHECK(!fired, "no callback on overflow drop");
    }

    // ---- Guard 4: totalChunks == 0 && totalSize != 0 -> SYSTEM_ERROR ----
    {
        bool fired = false;
        StreamReassembler r(
            [&](uint64_t, const std::vector<uint8_t>&) { fired = true; });
        MsgType mt = handle(r, MsgType::STREAM_START, startReq(4, 8, 0));
        CHECK(mt == MsgType::SYSTEM_ERROR,
              "zero chunks with nonzero size must be SYSTEM_ERROR");
        CHECK(!fired, "no callback on malformed empty stream");
    }

    // ---- Empty stream (valid): totalChunks == 0 && totalSize == 0 ----
    {
        int fired = 0;
        size_t sz = 999;
        StreamReassembler r([&](uint64_t id, const std::vector<uint8_t>& d) {
            ++fired;
            sz = d.size();
            (void)id;
        });
        MsgType mt = handle(r, MsgType::STREAM_START, startReq(5, 0, 0));
        CHECK(mt == MsgType::NORMAL, "valid empty stream ACKs");
        CHECK(fired == 1, "empty stream fires callback once");
        CHECK(sz == 0, "empty stream delivers zero bytes");
    }

    // ---- Normal completion, in-order: onStream once, data matches ----
    {
        int fired = 0;
        std::vector<uint8_t> got;
        uint64_t gotId = 0;
        StreamReassembler r([&](uint64_t id, const std::vector<uint8_t>& d) {
            ++fired;
            got = d;
            gotId = id;
        });
        CHECK(handle(r, MsgType::STREAM_START, startReq(6, 5, 2)) ==
                  MsgType::NORMAL,
              "valid start ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK,
                     chunkReq(6, 0, {'A', 'B', 'C'})) == MsgType::NORMAL,
              "chunk0 ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(6, 1, {'D', 'E'})) ==
                  MsgType::NORMAL,
              "chunk1 completes");
        CHECK(fired == 1, "normal stream fires callback exactly once");
        CHECK(gotId == 6, "callback carries correct stream id");
        CHECK(got.size() == 5, "assembled size matches totalSize");
        CHECK(std::memcmp(got.data(), "ABCDE", 5) == 0, "assembled bytes match");
    }

    // ---- Normal completion, reverse order ----
    {
        int fired = 0;
        std::vector<uint8_t> got;
        StreamReassembler r([&](uint64_t, const std::vector<uint8_t>& d) {
            ++fired;
            got = d;
        });
        CHECK(handle(r, MsgType::STREAM_START, startReq(7, 5, 2)) ==
                  MsgType::NORMAL,
              "valid start ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(7, 1, {'D', 'E'})) ==
                  MsgType::NORMAL,
              "chunk1 (first) ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK,
                     chunkReq(7, 0, {'A', 'B', 'C'})) == MsgType::NORMAL,
              "chunk0 (last) completes");
        CHECK(fired == 1, "reverse-order stream fires callback once");
        CHECK(got.size() == 5 && std::memcmp(got.data(), "ABCDE", 5) == 0,
              "reverse-order assembled bytes match");
    }

    // ---- totalSize at the bound is accepted (boundary, not over) ----
    {
        StreamReassembler r([](uint64_t, const std::vector<uint8_t>&) {});
        StreamReassemblerConfig cfg;
        // Exactly maxStreamSize is allowed; only strictly greater is rejected.
        // Use 1 chunk so we don't actually allocate 1 GiB of payload — the
        // START guard runs before any chunk payload is provided. We only check
        // the START is ACKed (allocation is the totalChunks-sized slice, =1).
        MsgType mt = handle(r, MsgType::STREAM_START,
                            startReq(8, cfg.maxStreamSize, 1));
        CHECK(mt == MsgType::NORMAL, "totalSize == maxStreamSize is accepted");
    }

    // ---- totalSize > bound is rejected ----
    {
        StreamReassembler r([](uint64_t, const std::vector<uint8_t>&) {});
        StreamReassemblerConfig cfg;
        MsgType mt = handle(r, MsgType::STREAM_START,
                            startReq(9, (uint64_t)cfg.maxStreamSize + 1, 1));
        CHECK(mt == MsgType::SYSTEM_ERROR,
              "totalSize over maxStreamSize must be SYSTEM_ERROR");
    }

    // ---- Zero-length chunk + duplicate: dedup by presence, not emptiness ----
    // Mirrors go/stream.go's nil-vs-empty sentinel. A zero-length chunk0
    // followed by a DUPLICATE zero-length chunk0 must be counted exactly once;
    // the stream then completes when chunk1 arrives. With an emptiness-based
    // sentinel the duplicate re-passes the gate, double-counts receivedChunks,
    // forces premature assembly, and the stream is dropped (SYSTEM_ERROR) —
    // diverging from Go, which accepts it. This case bites the presence-flag fix.
    {
        int fired = 0;
        std::vector<uint8_t> got;
        StreamReassembler r([&](uint64_t, const std::vector<uint8_t>& d) {
            ++fired;
            got = d;
        });
        CHECK(handle(r, MsgType::STREAM_START, startReq(10, 3, 2)) ==
                  MsgType::NORMAL,
              "valid start ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(10, 0, {})) ==
                  MsgType::NORMAL,
              "zero-length chunk0 ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(10, 0, {})) ==
                  MsgType::NORMAL,
              "duplicate zero-length chunk0 ACKs (ignored, not re-counted)");
        CHECK(fired == 0, "not complete after zero-length chunk0 + its dup");
        CHECK(handle(r, MsgType::STREAM_CHUNK,
                     chunkReq(10, 1, {'X', 'Y', 'Z'})) == MsgType::NORMAL,
              "chunk1 completes the stream");
        CHECK(fired == 1, "zero-length+dup stream fires callback exactly once");
        CHECK(got.size() == 3 && std::memcmp(got.data(), "XYZ", 3) == 0,
              "assembled bytes match after zero-length chunk0");
    }

    // ---- Duplicate (non-empty) chunk is ignored, not double-counted ----
    {
        int fired = 0;
        StreamReassembler r(
            [&](uint64_t, const std::vector<uint8_t>&) { ++fired; });
        CHECK(handle(r, MsgType::STREAM_START, startReq(11, 4, 2)) ==
                  MsgType::NORMAL,
              "valid start ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(11, 0, {'A', 'B'})) ==
                  MsgType::NORMAL,
              "chunk0 ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(11, 0, {'A', 'B'})) ==
                  MsgType::NORMAL,
              "duplicate chunk0 ignored");
        CHECK(fired == 0, "stream not complete after a duplicate of chunk0");
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(11, 1, {'C', 'D'})) ==
                  MsgType::NORMAL,
              "chunk1 completes");
        CHECK(fired == 1, "completes exactly once despite duplicate");
    }

    // ---- maxStreams concurrent in-flight bound -> SYSTEM_ERROR ----
    // Use a small cap so the test is cheap. None of the open streams are stale,
    // so the prune-on-cap-hit frees nothing and the over-cap start is rejected.
    {
        StreamReassemblerConfig cfg;
        cfg.maxStreams = 2;
        StreamReassembler r([](uint64_t, const std::vector<uint8_t>&) {}, cfg);
        CHECK(handle(r, MsgType::STREAM_START, startReq(20, 4, 1)) ==
                  MsgType::NORMAL,
              "stream 1 within cap ACKs");
        CHECK(handle(r, MsgType::STREAM_START, startReq(21, 4, 1)) ==
                  MsgType::NORMAL,
              "stream 2 within cap ACKs");
        MsgType mt = handle(r, MsgType::STREAM_START, startReq(22, 4, 1));
        CHECK(mt == MsgType::SYSTEM_ERROR,
              "third in-flight stream over maxStreams must be SYSTEM_ERROR");
    }

    if (g_failures == 0) {
        std::cout << "Test Passed" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " check(s) failed" << std::endl;
    return 1;
}
