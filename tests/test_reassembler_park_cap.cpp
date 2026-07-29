// Unit test for the SPEC §3.3.4 parked-chunk bound — the *count* dimension of
// "per-stream reassembly residency is O(totalSize)". Mirrors
// go/stream_park_cap_test.go so both peers accept and reject the same streams.
//
// The v0.8.17 running-byte guard bounds the payload a stream can make resident
// by its advertised totalSize. It cannot bound per-chunk bookkeeping, for two
// independent reasons: a parked entry costs memory at zero payload, and a
// zero-length chunk passes the byte guard by construction
// (offset + oooBytes + 0 <= totalSize holds for any live stream). Without a
// count bound a peer advertises totalSize = 1 with totalChunks =
// maxStreamChunks and parks 2^20-1 zero-length chunks: tens of MiB of map
// entries per stream, times maxStreams.
//
// Drives StreamReassembler::Handle() directly with crafted wire buffers — no
// DirectHost, no Go subprocess — so it is fast and deterministic.

#include <shm/StreamReassembler.h>
#include <shm/Stream.h>
#include <shm/IPCUtils.h>

#include <windows.h>
#include <psapi.h>

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

// K32GetProcessMemoryInfo is exported by kernel32.dll (Windows 7+), so the
// process byte counters are readable without linking psapi.
size_t privateUsage() {
    using Fn = BOOL(WINAPI*)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
    static Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo")));
    if (!fn) return 0;
    PROCESS_MEMORY_COUNTERS_EX pmc;
    std::memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (!fn(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
            sizeof(pmc))) {
        return 0;
    }
    return static_cast<size_t>(pmc.PrivateUsage);
}

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

// A zero-length STREAM_CHUNK request for the given index.
std::vector<uint8_t> emptyChunkReq(uint64_t streamId, uint32_t idx) {
    ChunkHeader h;
    h.streamId = streamId;
    h.chunkIndex = idx;
    h.payloadSize = 0;
    h.reserved = 0;
    h.padding = 0;
    std::vector<uint8_t> buf(sizeof(ChunkHeader));
    std::memcpy(buf.data(), &h, sizeof(h));
    return buf;
}

MsgType handle(StreamReassembler& r, MsgType type, const uint8_t* req,
               size_t reqSize) {
    uint8_t resp[64];
    size_t respSize = 0;
    MsgType mt = type;
    bool handled = r.Handle(req, reqSize, resp, respSize, mt);
    CHECK(handled, "Handle should claim stream messages");
    return mt;
}

MsgType handle(StreamReassembler& r, MsgType type,
               const std::vector<uint8_t>& req) {
    return handle(r, type, req.data(), req.size());
}

} // namespace

int main() {
    StreamReassemblerConfig cfg;
    const size_t maxParked = cfg.maxParkedChunks;

    // ---- Boundary: exactly maxParkedChunks entries are admitted; the next
    // ahead-of-cursor chunk is refused and the stream is dropped. Zero-length
    // payloads isolate the count bound from the byte bound.
    {
        int fired = 0;
        StreamReassembler r(
            [&](uint64_t, const std::vector<uint8_t>&) { ++fired; });
        // totalChunks must exceed the bound for it to be reachable; index 0 is
        // never sent, so the cursor never advances and nothing is drained.
        const uint32_t totalChunks = static_cast<uint32_t>(maxParked) + 8;
        CHECK(handle(r, MsgType::STREAM_START, startReq(1, 1, totalChunks)) ==
                  MsgType::NORMAL,
              "valid start ACKs");

        for (size_t i = 1; i <= maxParked; ++i) {
            MsgType mt = handle(r, MsgType::STREAM_CHUNK,
                                emptyChunkReq(1, static_cast<uint32_t>(i)));
            if (mt != MsgType::NORMAL) {
                std::cerr << "FAIL: parked chunk " << i << "/" << maxParked
                          << " rejected; the bound must admit exactly "
                             "maxParkedChunks entries"
                          << std::endl;
                ++g_failures;
                break;
            }
        }

        CHECK(handle(r, MsgType::STREAM_CHUNK,
                     emptyChunkReq(1, static_cast<uint32_t>(maxParked) + 1)) ==
                  MsgType::SYSTEM_ERROR,
              "chunk past the parked bound must be SYSTEM_ERROR");
        // Dropped, so any further chunk is answered as an unknown stream.
        CHECK(handle(r, MsgType::STREAM_CHUNK, emptyChunkReq(1, 1)) ==
                  MsgType::SYSTEM_ERROR,
              "stream must be dropped once the parked bound trips");
        CHECK(fired == 0, "over-fragmented stream must never be delivered");
    }

    // ---- Residency: flood every ahead-of-cursor index with a zero-length
    // chunk. Each one slips through the byte guard, so without the count bound
    // the parked map grows to ~2^20 entries for a stream that advertised one
    // byte.
    {
        const uint32_t totalChunks = static_cast<uint32_t>(cfg.maxStreamChunks);
        StreamReassembler r([](uint64_t, const std::vector<uint8_t>&) {
            std::cerr << "FAIL: over-advertised stream must never be delivered"
                      << std::endl;
        });
        CHECK(handle(r, MsgType::STREAM_START, startReq(2, 1, totalChunks)) ==
                  MsgType::NORMAL,
              "valid start ACKs");

        std::vector<uint8_t> req = emptyChunkReq(2, 0);
        ChunkHeader* h = reinterpret_cast<ChunkHeader*>(req.data());

        const size_t before = privateUsage();
        for (uint32_t i = 1; i < totalChunks; ++i) {
            h->chunkIndex = i;
            handle(r, MsgType::STREAM_CHUNK, req.data(), req.size());
        }
        const size_t after = privateUsage();

        CHECK(before != 0 && after != 0, "could not read process memory counters");
        const size_t grew = after > before ? after - before : 0;
        std::cout << "flooded " << (totalChunks - 1)
                  << " zero-length ooo chunks, private_bytes_delta=" << grew
                  << std::endl;

        // Budget: 1 MiB. With the bound in place at most maxParkedChunks (1024)
        // empty entries are ever resident — tens of KiB.
        const size_t budget = 1u << 20;
        if (grew > budget) {
            std::cerr << "FAIL: reassembler retained " << grew
                      << " bytes of parked-chunk bookkeeping for a stream "
                         "advertising 1 byte (budget "
                      << budget << ") — the parked-entry count is unbounded"
                      << std::endl;
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
