// Regression test for the SPEC §3.3.4 "per-stream residency is O(totalSize)"
// bound, count dimension.
//
// The v0.8.17 running-byte guard bounds the *payload* a stream may make
// resident by its advertised totalSize. It says nothing about per-chunk
// bookkeeping overhead, which the old C++ reassembler sized by totalChunks:
// `ctx.chunks.resize(totalChunks)` built one std::vector<uint8_t> element per
// chunk (24-32 bytes each, MSVC/libstdc++) plus a std::vector<bool> presence
// bitmap. With the maximum totalChunks (2^20) that is ~24 MiB committed at
// STREAM_START for a stream that advertises totalSize = 1, and maxStreams
// (1024) of them is ~24 GiB. The byte guard cannot see this: the overhead
// exists at zero payload, and zero-length chunks pass the byte guard by
// construction (a running length that already fits still fits after 0 bytes).
//
// The fix mirrors the Go reassembler (go/stream.go): preallocate a destination
// buffer of totalSize, copy in-order chunks straight into it at a running
// cursor, and park only ahead-of-cursor chunks in a sparse map bounded by
// maxParkedChunks. No totalChunks-sized array exists.
//
// This test measures actual committed private bytes (Win32
// PROCESS_MEMORY_COUNTERS_EX::PrivateUsage) for streams advertising 1 byte with
// the maximum chunk count. It is the C++ analogue of
// go/stream_ooo_guard_test.go::TestStreamReassembly_OutOfOrderOverflowBoundsMemory.

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

MsgType handle(StreamReassembler& r, MsgType type,
               const std::vector<uint8_t>& req) {
    uint8_t resp[64];
    size_t respSize = 0;
    MsgType mt = type;
    r.Handle(req.data(), req.size(), resp, respSize, mt);
    return mt;
}

} // namespace

int main() {
    StreamReassemblerConfig cfg;
    const uint32_t maxChunks = static_cast<uint32_t>(cfg.maxStreamChunks);
    const int numStreams = 8;

    // The reassembler must outlive the measurement: every stream stays open, so
    // nothing can be reclaimed and the delta is pure reassembly state.
    StreamReassembler r([](uint64_t, const std::vector<uint8_t>&) {
        std::cerr << "FAIL: no stream should complete here" << std::endl;
    });

    // Touch the allocator once so first-call heap growth is not attributed to
    // the streams under test.
    (void)handle(r, MsgType::STREAM_START, startReq(0, 8, 2));

    const size_t before = privateUsage();
    for (int i = 0; i < numStreams; ++i) {
        MsgType mt = handle(r, MsgType::STREAM_START,
                            startReq(100 + i, /*totalSize=*/1, maxChunks));
        if (mt != MsgType::NORMAL) {
            std::cerr << "FAIL: STREAM_START with totalSize=1, totalChunks="
                      << maxChunks << " was rejected" << std::endl;
            return 1;
        }
    }
    const size_t after = privateUsage();

    if (before == 0 || after == 0) {
        std::cerr << "FAIL: could not read process memory counters" << std::endl;
        return 1;
    }

    const size_t grew = after > before ? after - before : 0;
    const size_t perStream = grew / numStreams;

    std::cout << "streams=" << numStreams << " totalSize=1 totalChunks="
              << maxChunks << " private_bytes_delta=" << grew << " per_stream="
              << perStream << std::endl;

    // Budget: 64 KiB per stream. A correct implementation allocates the
    // destination buffer (1 byte, rounded to a heap granule) plus a
    // StreamContext (a few hundred bytes) and an empty parked map, so the real
    // figure is two orders of magnitude below this. The old totalChunks-sized
    // layout allocated ~24 MiB per stream and fails by ~400x.
    const size_t budget = 64u * 1024u;
    if (perStream > budget) {
        std::cerr << "FAIL: per-stream residency " << perStream
                  << " bytes exceeds the " << budget
                  << " byte budget for a stream advertising 1 byte — "
                     "reassembly state is sized by totalChunks, not totalSize"
                  << std::endl;
        return 1;
    }

    std::cout << "Test Passed" << std::endl;
    return 0;
}
