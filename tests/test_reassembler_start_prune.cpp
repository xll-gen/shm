// Age-prune-on-START parity with the Go reassembler (go/stream.go).
//
// WHAT THIS GUARDS. The C++ reassembler used to run PruneInternal ONLY when a
// new stream would exceed maxStreams. A host that stays under the cap and never
// calls Prune() therefore held every abandoned stream context — each one pinning
// its full advertised totalSize — for the life of the process. Go prunes on every
// StreamStart. SPEC §3.3.4 leaves the reclaim mechanism implementation-defined,
// so this was not a contract violation; it was an asymmetry with real memory
// behind it, between two implementations that are meant to be interchangeable.
//
// The test drives it far below maxStreams (default 1024) on purpose: at or above
// the cap the OLD code pruned too, so a test that filled the map would pass
// either way and prove nothing.
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include <shm/StreamReassembler.h>

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

std::vector<uint8_t> startReq(uint64_t streamId, uint64_t totalSize, uint32_t totalChunks) {
    StreamHeader h;
    h.streamId = streamId;
    h.totalSize = totalSize;
    h.totalChunks = totalChunks;
    h.appMsgType = 0;
    std::vector<uint8_t> buf(sizeof(StreamHeader));
    std::memcpy(buf.data(), &h, sizeof(h));
    return buf;
}

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
    if (!payload.empty()) std::memcpy(buf.data() + sizeof(h), payload.data(), payload.size());
    return buf;
}

MsgType handle(StreamReassembler& r, MsgType type, const std::vector<uint8_t>& req) {
    uint8_t resp[64];
    size_t respSize = 0;
    MsgType mt = type;
    r.Handle(req.data(), req.size(), resp, respSize, mt);
    return mt;
}

} // namespace

int main() {
    StreamReassemblerConfig cfg;
    cfg.streamTimeoutMs = 50; // keep the test fast; the mechanism is the same

    int completed = 0;
    StreamReassembler r([&](uint64_t, const std::vector<uint8_t>&) { ++completed; }, cfg);

    // Stream 1: start it and abandon it (never send its chunk).
    CHECK(handle(r, MsgType::STREAM_START, startReq(1, 4, 1)) != MsgType::SYSTEM_ERROR,
          "abandoned stream should start cleanly");

    // Let it age past the timeout.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    // Stream 2 starts. This is the START that must prune stream 1 — and note we
    // are nowhere near maxStreams, which is exactly the case the old code missed.
    CHECK(handle(r, MsgType::STREAM_START, startReq(2, 4, 1)) != MsgType::SYSTEM_ERROR,
          "second stream should start cleanly");

    // Now prove stream 1 is GONE rather than merely stale: a chunk for it must be
    // refused as unknown. If the START had not pruned, this chunk would be
    // accepted into the surviving context and complete it, firing the callback.
    int before = completed;
    handle(r, MsgType::STREAM_CHUNK, chunkReq(1, 0, 0, std::vector<uint8_t>(4, 0xAA)));
    CHECK(completed == before,
          "a chunk for the aged-out stream completed it, so the START did not prune "
          "(the context was still resident)");

    // The live stream must be untouched by the prune.
    handle(r, MsgType::STREAM_CHUNK, chunkReq(2, 0, 0, std::vector<uint8_t>(4, 0xBB)));
    CHECK(completed == before + 1, "the prune dropped the LIVE stream as well");

    if (g_failures == 0) {
        std::cout << "test_reassembler_start_prune: OK" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " failure(s)" << std::endl;
    return 1;
}
