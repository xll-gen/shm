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
    h.appMsgType = 0;
    std::vector<uint8_t> buf(sizeof(StreamHeader));
    std::memcpy(buf.data(), &h, sizeof(h));
    return buf;
}

// Build a STREAM_CHUNK request buffer: header + payload.
//
// dstOffset (SHM_VERSION 0x00080000, formerly reserved@16) is the ABSOLUTE
// destination of the payload inside the stream and is REQUIRED here rather than
// defaulted, because 0 is a legal offset for chunk 0: a call site that forgot
// the field would silently claim "offset 0" and either pass vacuously or fail
// as a misplacement for the wrong reason. Every caller below passes the value a
// conforming sender would write (the prefix sum of the preceding payloadSizes).
std::vector<uint8_t> chunkReq(uint64_t streamId, uint32_t chunkIndex,
                              uint32_t dstOffset,
                              const std::vector<uint8_t>& payload) {
    ChunkHeader h;
    h.streamId = streamId;
    h.chunkIndex = chunkIndex;
    h.payloadSize = static_cast<uint32_t>(payload.size());
    h.dstOffset = dstOffset;
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
                     chunkReq(2, 0, 0, {0xAA, 0xBB})) == MsgType::NORMAL,
              "chunk0 ACKs");
        // Final chunk completes count (2/2) but total is 4 != 6.
        MsgType mt =
            handle(r, MsgType::STREAM_CHUNK, chunkReq(2, 1, 2, {0xCC, 0xDD}));
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
                     chunkReq(3, 0, 0, {0x01, 0x02})) == MsgType::NORMAL,
              "chunk0 ACKs");
        MsgType mt =
            handle(r, MsgType::STREAM_CHUNK, chunkReq(3, 1, 2, {0x03, 0x04}));
        CHECK(mt == MsgType::SYSTEM_ERROR,
              "assembled-length overflow must be SYSTEM_ERROR");
        CHECK(!fired, "no callback on overflow drop");
    }

    // ---- Guard 3 (overflow), out-of-order arrival ----
    // Same guard, but no chunk ever arrives at the in-order cursor: an
    // implementation whose running-length check only covers the in-order path
    // buffers every chunk and rejects only at completion, after the bytes are
    // resident. Both peers must reject at arrival. This block is the Go/C++
    // parity marker; the depth cases live in
    // tests/test_reassembler_ooo_overflow.cpp and go/stream_ooo_guard_test.go.
    {
        bool fired = false;
        StreamReassembler r(
            [&](uint64_t, const std::vector<uint8_t>&) { fired = true; });
        CHECK(handle(r, MsgType::STREAM_START, startReq(30, 3, 3)) ==
                  MsgType::NORMAL,
              "valid start ACKs");
        // dstOffset 1 keeps chunk2 in bounds (1+2 == totalSize), so what refuses
        // the next chunk is unambiguously the running-length guard and not the
        // dstOffset bounds guard.
        CHECK(handle(r, MsgType::STREAM_CHUNK,
                     chunkReq(30, 2, 1, {0x01, 0x02})) == MsgType::NORMAL,
              "ooo chunk2 within totalSize ACKs");
        MsgType mt =
            handle(r, MsgType::STREAM_CHUNK, chunkReq(30, 1, 0, {0x03, 0x04}));
        CHECK(mt == MsgType::SYSTEM_ERROR,
              "ooo running-length overflow must be SYSTEM_ERROR at arrival");
        CHECK(!fired, "no callback on ooo overflow drop");
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
                     chunkReq(6, 0, 0, {'A', 'B', 'C'})) == MsgType::NORMAL,
              "chunk0 ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(6, 1, 3, {'D', 'E'})) ==
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
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(7, 1, 3, {'D', 'E'})) ==
                  MsgType::NORMAL,
              "chunk1 (first) ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK,
                     chunkReq(7, 0, 0, {'A', 'B', 'C'})) == MsgType::NORMAL,
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
    // Mirrors go/stream.go. A zero-length chunk0 followed by a DUPLICATE
    // zero-length chunk0 must be counted exactly once; the stream then completes
    // when chunk1 arrives. Any dedup marker that cannot distinguish "received,
    // empty" from "not received" — an emptiness test over stored payloads —
    // lets the duplicate re-pass the gate, double-counts the chunk, forces
    // premature completion, and drops the stream (SYSTEM_ERROR), diverging from
    // Go, which accepts it. Both sides therefore track presence explicitly: the
    // cursor for assembled indices plus membership in the parked map.
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
        // Chunk 0 is empty, so chunk 1 legitimately starts at dstOffset 0.
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(10, 0, 0, {})) ==
                  MsgType::NORMAL,
              "zero-length chunk0 ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(10, 0, 0, {})) ==
                  MsgType::NORMAL,
              "duplicate zero-length chunk0 ACKs (ignored, not re-counted)");
        CHECK(fired == 0, "not complete after zero-length chunk0 + its dup");
        CHECK(handle(r, MsgType::STREAM_CHUNK,
                     chunkReq(10, 1, 0, {'X', 'Y', 'Z'})) == MsgType::NORMAL,
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
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(11, 0, 0, {'A', 'B'})) ==
                  MsgType::NORMAL,
              "chunk0 ACKs");
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(11, 0, 0, {'A', 'B'})) ==
                  MsgType::NORMAL,
              "duplicate chunk0 ignored");
        CHECK(fired == 0, "stream not complete after a duplicate of chunk0");
        CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(11, 1, 2, {'C', 'D'})) ==
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
