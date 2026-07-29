// Regression test for the SPEC §3.3.4 "per-stream residency is O(totalSize)"
// bound, peak dimension.
//
// The old C++ reassembler stored each chunk in its own vector
// (ctx.chunks[idx].assign(...)) and, at completion, concatenated all of them
// into a second full-size buffer (fullData.reserve(totalSize) + insert) while
// the per-chunk copies were still alive. Peak residency for a completing stream
// was therefore 2 x totalSize — 2 GiB for a 1 GiB stream — a risk the Go
// reassembler does not carry because it copies in-order chunks straight into a
// preallocated destination (go/stream.go: streamContext.buf).
//
// This test drives a 64 MiB in-order stream and reads the kernel's peak commit
// charge (PROCESS_MEMORY_COUNTERS::PeakPagefileUsage), which records the high
// water mark even though the intermediate copies are freed before the callback
// runs. It lives in its own binary so the measurement starts from a pristine
// process; sharing a process with another allocation-heavy case would poison
// the peak.

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

struct MemInfo {
    size_t pagefile;
    size_t peakPagefile;
};

// K32GetProcessMemoryInfo is exported by kernel32.dll (Windows 7+), so the
// process byte counters are readable without linking psapi.
bool readMem(MemInfo& out) {
    using Fn = BOOL(WINAPI*)(HANDLE, PROCESS_MEMORY_COUNTERS*, DWORD);
    static Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo")));
    if (!fn) return false;
    PROCESS_MEMORY_COUNTERS pmc;
    std::memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (!fn(GetCurrentProcess(), &pmc, sizeof(pmc))) return false;
    out.pagefile = static_cast<size_t>(pmc.PagefileUsage);
    out.peakPagefile = static_cast<size_t>(pmc.PeakPagefileUsage);
    return true;
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

MsgType handle(StreamReassembler& r, MsgType type, const uint8_t* req,
               size_t reqSize) {
    uint8_t resp[64];
    size_t respSize = 0;
    MsgType mt = type;
    r.Handle(req, reqSize, resp, respSize, mt);
    return mt;
}

} // namespace

int main() {
    const size_t chunkSize = 512u * 1024u;
    const uint32_t totalChunks = 128;
    const uint64_t totalSize = static_cast<uint64_t>(chunkSize) * totalChunks; // 64 MiB

    size_t deliveredSize = 0;
    int delivered = 0;
    StreamReassembler r([&](uint64_t, const std::vector<uint8_t>& d) {
        ++delivered;
        deliveredSize = d.size();
    });

    // One reusable request buffer for every chunk: the measurement must reflect
    // the reassembler's residency, not the harness's.
    std::vector<uint8_t> req(sizeof(ChunkHeader) + chunkSize, 0xA5);

    MemInfo base;
    if (!readMem(base)) {
        std::cerr << "FAIL: could not read process memory counters" << std::endl;
        return 1;
    }

    auto startBuf = startReq(1, totalSize, totalChunks);
    if (handle(r, MsgType::STREAM_START, startBuf.data(), startBuf.size()) !=
        MsgType::NORMAL) {
        std::cerr << "FAIL: STREAM_START rejected" << std::endl;
        return 1;
    }

    for (uint32_t i = 0; i < totalChunks; ++i) {
        ChunkHeader h;
        h.streamId = 1;
        h.chunkIndex = i;
        h.payloadSize = static_cast<uint32_t>(chunkSize);
        h.reserved = 0;
        h.padding = 0;
        std::memcpy(req.data(), &h, sizeof(h));
        if (handle(r, MsgType::STREAM_CHUNK, req.data(), req.size()) !=
            MsgType::NORMAL) {
            std::cerr << "FAIL: chunk " << i << " rejected" << std::endl;
            return 1;
        }
    }

    MemInfo after;
    if (!readMem(after)) {
        std::cerr << "FAIL: could not read process memory counters" << std::endl;
        return 1;
    }

    if (delivered != 1 || deliveredSize != totalSize) {
        std::cerr << "FAIL: expected one delivery of " << totalSize << " bytes, got "
                  << delivered << " delivery/deliveries of " << deliveredSize
                  << " bytes" << std::endl;
        return 1;
    }

    const size_t peakDelta = after.peakPagefile > base.pagefile
                                 ? after.peakPagefile - base.pagefile
                                 : 0;
    const double ratio = static_cast<double>(peakDelta) /
                         static_cast<double>(totalSize);

    std::cout << "stream_bytes=" << totalSize << " peak_commit_delta=" << peakDelta
              << " ratio=" << ratio << std::endl;

    // Budget: 1.5 x totalSize. A single-destination implementation lands at
    // ~1.0x (the destination plus the 512 KiB request buffer); the old
    // chunks[]-plus-concatenate layout lands at ~2.0x and fails here.
    const size_t budget = static_cast<size_t>(totalSize + totalSize / 2);
    if (peakDelta > budget) {
        std::cerr << "FAIL: peak commit grew by " << peakDelta
                  << " bytes for a " << totalSize
                  << " byte stream (budget " << budget
                  << ") — the completion path holds two full copies at once"
                  << std::endl;
        return 1;
    }

    std::cout << "Test Passed" << std::endl;
    return 0;
}
