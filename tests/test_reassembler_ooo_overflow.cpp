// Unit test for the SPECIFICATION.md §3.3.4 per-chunk overflow guard on the
// OUT-OF-ORDER path of StreamReassembler. Mirrors the Go regression tests in
// go/stream_ooo_guard_test.go so both peers accept and reject the same streams.
//
// Before the fix the C++ reassembler had no running-length accounting at all:
// every chunk was copied into ctx.chunks[idx] on arrival and the byte-sum was
// only validated in the completion assembly loop. A peer could therefore
// advertise totalSize=1 with totalChunks=1000 and stream 512 KiB chunks, and
// each one was stored — the rejection came only after all of the bytes were
// already resident, which is exactly the unbounded allocation §3.3.4 forbids.
// (The equivalent Go scenario measured 33 MB retained for a stream advertising
// 1024 bytes.) The guard must reject at arrival time, in any arrival order,
// while a well-formed stream whose byte-sum equals totalSize exactly must still
// complete.
//
// Drives StreamReassembler::Handle() directly with crafted wire buffers — no
// DirectHost, no Go subprocess — so it is fast and deterministic.

#include <shm/StreamReassembler.h>
#include <shm/Stream.h>
#include <shm/IPCUtils.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
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

MsgType handle(StreamReassembler& r, MsgType type,
               const std::vector<uint8_t>& req) {
    uint8_t resp[64];
    size_t respSize = 0;
    MsgType mt = type;
    bool handled = r.Handle(req.data(), req.size(), resp, respSize, mt);
    CHECK(handled, "Handle should claim stream messages");
    return mt;
}

// A payload of `n` bytes filled with a per-index marker so misplacement is
// detectable in the assembled result.
std::vector<uint8_t> payload(size_t n, uint8_t marker) {
    return std::vector<uint8_t>(n, marker);
}

struct Step {
    uint32_t index;
    size_t size;
    MsgType want;
};

// Runs one advertised header plus a sequence of chunk deliveries, checking the
// reply of each step. Asserts the callback never fired.
void runRejectCase(const char* name, uint64_t streamId, uint64_t totalSize,
                   uint32_t totalChunks, const std::vector<Step>& steps) {
    int fired = 0;
    StreamReassembler r(
        [&](uint64_t, const std::vector<uint8_t>&) { ++fired; });
    CHECK(handle(r, MsgType::STREAM_START,
                 startReq(streamId, totalSize, totalChunks)) ==
              MsgType::NORMAL,
          std::string(name) + ": valid start must ACK");
    for (size_t i = 0; i < steps.size(); ++i) {
        MsgType mt = handle(r, MsgType::STREAM_CHUNK,
                            chunkReq(streamId, steps[i].index,
                                     payload(steps[i].size, 0x5A)));
        CHECK(mt == steps[i].want,
              std::string(name) + ": step " + std::to_string(i) + " (chunk " +
                  std::to_string(steps[i].index) + ", " +
                  std::to_string(steps[i].size) + " bytes) reply mismatch");
    }
    CHECK(fired == 0, std::string(name) + ": corrupt stream must not deliver");
}

// Runs a well-formed stream whose chunk sizes sum exactly to totalSize,
// delivered in `order`, and checks it completes once with the in-index-order
// bytes.
void runCompleteCase(const char* name, uint64_t streamId,
                     const std::vector<size_t>& sizes,
                     const std::vector<uint32_t>& order) {
    uint64_t totalSize = 0;
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<uint8_t> want;
    for (size_t i = 0; i < sizes.size(); ++i) {
        payloads.push_back(payload(sizes[i], static_cast<uint8_t>('A' + i)));
        want.insert(want.end(), payloads[i].begin(), payloads[i].end());
        totalSize += sizes[i];
    }

    int fired = 0;
    std::vector<uint8_t> got;
    StreamReassembler r([&](uint64_t, const std::vector<uint8_t>& d) {
        ++fired;
        got = d;
    });
    CHECK(handle(r, MsgType::STREAM_START,
                 startReq(streamId, totalSize,
                          static_cast<uint32_t>(sizes.size()))) ==
              MsgType::NORMAL,
          std::string(name) + ": valid start must ACK");
    for (size_t s = 0; s < order.size(); ++s) {
        uint32_t idx = order[s];
        MsgType mt =
            handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, idx, payloads[idx]));
        CHECK(mt == MsgType::NORMAL,
              std::string(name) + ": step " + std::to_string(s) + " (chunk " +
                  std::to_string(idx) +
                  ") was rejected — a valid out-of-order stream must complete");
    }
    CHECK(fired == 1, std::string(name) + ": must deliver exactly once");
    CHECK(got == want, std::string(name) + ": assembled bytes wrong");
}

} // namespace

int main() {
    // ---- The reported scenario, scaled down: tiny totalSize, big chunks, and
    // every chunk ahead of index 0 so no in-order path is ever taken. The FIRST
    // chunk must be refused; the stream is then erased, so the next is too.
    // Pre-fix both were stored and ACKed (63 x 512 KiB resident in the full
    // scenario), and rejection waited for index 0.
    {
        const size_t big = 512 * 1024;
        runRejectCase("tiny_total_big_ooo_chunks", 1, /*totalSize=*/1,
                      /*totalChunks=*/1000,
                      {{1, big, MsgType::SYSTEM_ERROR},
                       {2, big, MsgType::SYSTEM_ERROR}});
    }

    // ---- No single parked chunk overflows; their sum does. A per-chunk-only
    // (non-accumulating) guard would miss this.
    runRejectCase("accumulated_ooo_chunks_overflow", 2, /*totalSize=*/8,
                  /*totalChunks=*/4,
                  {{3, 4, MsgType::NORMAL},
                   {2, 4, MsgType::NORMAL},
                   {1, 1, MsgType::SYSTEM_ERROR}});

    // ---- Already-received bytes must also constrain a later chunk that fits
    // on its own.
    runRejectCase("later_chunk_overflows_against_received", 3, /*totalSize=*/8,
                  /*totalChunks=*/3,
                  {{2, 6, MsgType::NORMAL}, {0, 6, MsgType::SYSTEM_ERROR}});

    // ---- A zero-length chunk contributes nothing, so the guard must not fire
    // on it; the following oversized one must.
    runRejectCase("zero_length_then_overflow", 4, /*totalSize=*/4,
                  /*totalChunks=*/3,
                  {{2, 0, MsgType::NORMAL}, {1, 5, MsgType::SYSTEM_ERROR}});

    // ---- No-regression: well-formed streams with uneven chunk sizes
    // (including a zero-length one) must complete in every arrival order. These
    // are what an over-eager accumulator (double-counting a chunk) breaks.
    {
        const std::vector<size_t> sizes = {7, 1, 64, 0, 13, 255, 2, 100};
        std::vector<uint32_t> reverse;
        for (uint32_t i = static_cast<uint32_t>(sizes.size()); i-- > 0;) {
            reverse.push_back(i);
        }
        runCompleteCase("complete_reverse", 10, sizes, reverse);

        std::vector<uint32_t> oddEven;
        for (uint32_t i = 1; i < sizes.size(); i += 2) oddEven.push_back(i);
        for (uint32_t i = 0; i < sizes.size(); i += 2) oddEven.push_back(i);
        runCompleteCase("complete_odd_then_even", 11, sizes, oddEven);

        // Fixed interleave: fill a gap (so the head is assembled) and only then
        // deliver further ahead-of-cursor chunks.
        runCompleteCase("complete_drain_then_park", 12, sizes,
                        {1, 0, 3, 2, 5, 4, 7, 6});

        // Fixed shuffles (hard-coded rather than rand() so the case is
        // reproducible across standard libraries).
        runCompleteCase("complete_shuffle_a", 13, sizes, {5, 4, 2, 6, 7, 0, 3, 1});
        runCompleteCase("complete_shuffle_b", 14, sizes, {2, 0, 5, 3, 1, 6, 7, 4});
        runCompleteCase("complete_shuffle_c", 15, sizes, {2, 5, 3, 0, 1, 7, 4, 6});
    }

    // ---- Boundary: the guard is `>`, not `>=`. A chunk that consumes the whole
    // remaining advertised space must be accepted, whether it arrives first or
    // last.
    runCompleteCase("boundary_ooo_chunk_fills_everything", 20, {0, 4}, {1, 0});
    runCompleteCase("boundary_in_order_chunk_fills_everything", 21, {4, 0}, {1, 0});
    runCompleteCase("boundary_reverse_exact_fill", 22, {2, 2, 2}, {2, 1, 0});
    runCompleteCase("boundary_single_chunk_exact_fill", 23, {3}, {0});

    if (g_failures == 0) {
        std::cout << "Test Passed" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " check(s) failed" << std::endl;
    return 1;
}
