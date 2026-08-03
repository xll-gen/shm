// Regression test for the SPEC §3.3.4 "per-stream residency is O(totalSize)"
// bound, PEAK dimension, plus the "out-of-order arrival parks bookkeeping only,
// never payload bytes" MUST. It measures the kernel's commit charge
// (PROCESS_MEMORY_COUNTERS), which records the high water mark even though every
// intermediate copy is freed before the callback runs. It lives in its own binary
// so the measurement starts from a pristine process; sharing a process with
// another allocation-heavy case would poison the peak.
//
// THREE PHASES, and why each is needed:
//
//   PHASE 1, reverse order — the out-of-order park. Up to SHM_VERSION 0x00070000
//   an ahead-of-cursor chunk was copied into the parked map as PAYLOAD and copied a
//   second time into the destination when the gap filled, so a stream delivered
//   back-to-front held ~2 x totalSize at once (63.5 MiB of parked copies plus the
//   64 MiB destination here). Since 0x00080000 every chunk is written straight to
//   buf[dstOffset] on arrival whatever the order, and the parked map holds 16 bytes
//   of bookkeeping per chunk instead of the bytes themselves. An IN-ORDER stream
//   cannot see this difference at all — which is why this phase runs reverse.
//
//   PHASE 2, in-order — the completion concatenation. The pre-v0.8.17 C++
//   reassembler stored each chunk in its own vector (ctx.chunks[idx].assign(...))
//   and, at completion, concatenated all of them into a SECOND full-size buffer
//   (fullData.reserve(totalSize) + insert) while the per-chunk copies were still
//   alive: 2 GiB for a 1 GiB stream, a risk the Go reassembler never carried
//   because it copies straight into a preallocated destination. The destination is
//   now handed out by move.
//
//   PHASE 3, incomplete stream — the parked-record cost, per record. Phases 1 and 2
//   are whole-stream budgets, so they can only see a regression once its total size
//   clears their slack. Phase 3 holds 127 records parked simultaneously and divides:
//   it bounds what ONE parked record may cost, which is the property SPEC §3.3.4
//   actually states. It uses CURRENT commit rather than peak, so it is immune to
//   the monotonicity problem below.
//
// BUDGETS ARE PROPORTIONAL TO THE BOOKKEEPING, NOT TO THE STREAM.
// This test previously allowed 0.5 x totalSize (32 MiB of slack on a 64 MiB
// stream). A FULL reintroduction of parked payloads lands at ~1.99x and did go red,
// so it was not vacuous — but a PARTIAL regression (payload copies on only a subset
// of chunks, or only on the drain path) fits inside 32 MiB and shipped green. The
// budgets below are a fixed allocator/CRT slack plus a per-parked-record allowance;
// observed overhead on the reference host is ~180 KiB for phases 1-2 and ~8 KiB for
// phase 3.
//
// PeakPagefileUsage IS MONOTONE, which used to mean phase 2 (measured against
// phase 1's baseline) could only detect a peak HIGHER than phase 1 ever reached.
// Two things fix that: the budget is now tight enough that phase 1's own peak sits
// just above one stream, and phase 2 additionally asserts an INCREMENTAL bound —
// peak growth between the two phases — which is ~4 KiB in practice because phase
// 1's destination is released before phase 2 allocates its own.
//
// A STRUCTURAL guard complements all of this and cannot be evaded by staying under
// a byte budget: `static_assert(sizeof(StreamContext::Placed) == 16)` and
// `is_trivially_copyable` in include/shm/StreamReassembler.h — the C++ analogue of
// Go's TestStreamReassembly_ParkedRecordHoldsNoPayload. Placed is private, so the
// assert lives with the type; every TU that includes the header (this one
// included) fails to compile if a payload-owning member is added.

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
    StreamHeader h{};
    h.streamId = streamId;
    h.totalSize = totalSize;
    h.totalChunks = totalChunks;
    h.appMsgType = 0;
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

const size_t kChunkSize = 512u * 1024u;
const uint32_t kTotalChunks = 128;
const uint64_t kTotalSize = static_cast<uint64_t>(kChunkSize) * kTotalChunks; // 64 MiB

// Writes the chunk header for `idx` into `req` (which already holds the payload
// bytes) and hands it to the reassembler.
MsgType sendChunk(StreamReassembler& r, uint64_t streamId, uint32_t idx,
                  std::vector<uint8_t>& req) {
    ChunkHeader h{};
    h.streamId = streamId;
    h.chunkIndex = idx;
    h.payloadSize = static_cast<uint32_t>(kChunkSize);
    // Uniform chunk sizes, so the absolute destination a conforming sender
    // declares is exactly idx * kChunkSize. This is what lets the reverse phase
    // place every chunk on arrival instead of parking its payload.
    h.dstOffset = static_cast<uint32_t>(idx * kChunkSize);
    std::memcpy(req.data(), &h, sizeof(h));
    return handle(r, MsgType::STREAM_CHUNK, req.data(), req.size());
}

// Drives one complete stream, delivering chunk indices in `order`. One reusable
// request buffer serves every chunk so the measurement reflects the
// reassembler's residency, not the harness's. Returns false on any rejection or
// a delivery that is not exactly one full-size buffer.
bool driveStream(uint64_t streamId, const std::vector<uint32_t>& order,
                 std::vector<uint8_t>& req) {
    int delivered = 0;
    size_t deliveredSize = 0;
    StreamReassembler r([&](uint64_t, const std::vector<uint8_t>& d) {
        ++delivered;
        deliveredSize = d.size();
    });

    auto startBuf = startReq(streamId, kTotalSize, kTotalChunks);
    if (handle(r, MsgType::STREAM_START, startBuf.data(), startBuf.size()) !=
        MsgType::NORMAL) {
        std::cerr << "FAIL: STREAM_START rejected" << std::endl;
        return false;
    }

    for (uint32_t idx : order) {
        if (sendChunk(r, streamId, idx, req) != MsgType::NORMAL) {
            std::cerr << "FAIL: chunk " << idx << " rejected" << std::endl;
            return false;
        }
    }

    if (delivered != 1 || deliveredSize != kTotalSize) {
        std::cerr << "FAIL: expected one delivery of " << kTotalSize
                  << " bytes, got " << delivered << " delivery/deliveries of "
                  << deliveredSize << " bytes" << std::endl;
        return false;
    }
    return true;
}

// Fixed slack for allocator/CRT behavior that is not attributable to the stream:
// heap growth, iostream buffers, page granularity. Measured at ~180 KiB on the
// reference host across phases 1-2. NOT proportional to totalSize — that was the
// defect in the previous version of this test.
const size_t kFixedSlack = 1u * 1024u * 1024u;

// What ONE ahead-of-cursor record may cost. StreamContext::Placed is 16 bytes; an
// unordered_map node plus bucket slot brings the real figure to roughly 56-72
// bytes. 512 leaves a large margin over that while still being three orders of
// magnitude below one chunk's 512 KiB payload, so any reintroduced payload copy —
// on all chunks or on a subset — blows it.
const size_t kMaxBytesPerParkedRecord = 512;

// Phase 1/2 budget: one stream plus fixed slack plus the bookkeeping for the 127
// records the reverse order parks at its worst moment.
const size_t kBudget = static_cast<size_t>(kTotalSize) + kFixedSlack +
                       kMaxBytesPerParkedRecord * (kTotalChunks - 1);

// Peak growth allowed BETWEEN phase 1 and phase 2. Phase 1's destination is freed
// before phase 2 allocates its own, so the legitimate figure is ~0 (0-4 KiB
// measured across runs). This is what makes phase 2 a real check despite
// PeakPagefileUsage being monotone: without it, a phase-2 regression smaller than
// phase 1's own peak is invisible no matter how tight kBudget is.
//
// Deliberately much tighter than kFixedSlack. That gap is the point: a completion
// path regression somewhere between this and (kFixedSlack - phase 1's own overhead)
// trips ONLY this bound, so the two are not redundant.
const size_t kInterPhaseBudget = 256u * 1024u;

bool checkPeak(const char* phase, const MemInfo& base, const MemInfo& now) {
    const size_t peakDelta =
        now.peakPagefile > base.pagefile ? now.peakPagefile - base.pagefile : 0;
    const double ratio =
        static_cast<double>(peakDelta) / static_cast<double>(kTotalSize);
    std::cout << phase << ": stream_bytes=" << kTotalSize
              << " peak_commit_delta=" << peakDelta << " ratio=" << ratio
              << " budget=" << kBudget << std::endl;
    if (peakDelta > kBudget) {
        std::cerr << "FAIL: " << phase << ": peak commit grew by " << peakDelta
                  << " bytes for a " << kTotalSize << " byte stream (budget "
                  << kBudget << ", i.e. one stream + " << kFixedSlack
                  << " slack + " << kMaxBytesPerParkedRecord
                  << " per parked record). Payload bytes are being held somewhere "
                     "other than the destination buffer."
                  << std::endl;
        return false;
    }
    return true;
}

// PHASE 3: bound the cost of a SINGLE parked record.
//
// Chunk 0 is withheld so all 127 remaining chunks stay parked ahead of the cursor
// and are alive when the measurement is taken. CURRENT commit is used, not peak,
// so this phase is independent of everything the earlier phases did. A conforming
// reassembler holds exactly the 64 MiB destination plus 127 x {dstOffset, size};
// a reassembler that parks payload holds up to 127 extra chunk copies, and even a
// PARTIAL regression (payload on every Nth chunk only) shows up because the
// allowance is per record, not per stream.
bool checkParkedRecordCost(std::vector<uint8_t>& req) {
    const uint64_t streamId = 3;
    const uint32_t parked = kTotalChunks - 1;

    // Scoped so the destination is released before the function returns.
    {
        int delivered = 0;
        StreamReassembler r([&](uint64_t, const std::vector<uint8_t>&) { ++delivered; });

        auto startBuf = startReq(streamId, kTotalSize, kTotalChunks);
        if (handle(r, MsgType::STREAM_START, startBuf.data(), startBuf.size()) !=
            MsgType::NORMAL) {
            std::cerr << "FAIL: parked_record_cost: STREAM_START rejected" << std::endl;
            return false;
        }

        // Baseline is taken AFTER STREAM_START, i.e. with the destination buffer
        // already committed. Everything measured from here on is attributable to
        // the arriving chunks and nothing else — no destination allocation slack,
        // no heap growth from earlier phases. Commit charge does not change when
        // already-committed pages are written, so the chunk payloads landing in the
        // destination contribute exactly zero to this delta; a payload copy held
        // ANYWHERE ELSE contributes all of itself.
        MemInfo before;
        if (!readMem(before)) return false;

        for (uint32_t idx = 1; idx < kTotalChunks; ++idx) {
            if (sendChunk(r, streamId, idx, req) != MsgType::NORMAL) {
                std::cerr << "FAIL: parked_record_cost: chunk " << idx << " rejected"
                          << std::endl;
                return false;
            }
        }

        if (delivered != 0) {
            std::cerr << "FAIL: parked_record_cost: stream completed with chunk 0 "
                         "never delivered" << std::endl;
            return false;
        }

        MemInfo after;
        if (!readMem(after)) return false;

        const size_t overhead =
            after.pagefile > before.pagefile ? after.pagefile - before.pagefile : 0;
        const size_t allowance = kMaxBytesPerParkedRecord * parked;
        const double perRecord =
            static_cast<double>(overhead) / static_cast<double>(parked);

        std::cout << "parked_record_cost: parked=" << parked
                  << " commit_delta_after_destination=" << overhead
                  << " bytes_per_record=" << perRecord
                  << " allowance=" << allowance << std::endl;

        if (overhead > allowance) {
            std::cerr << "FAIL: parked_record_cost: " << parked
                      << " ahead-of-cursor chunks cost " << overhead
                      << " bytes of state beyond the already-committed "
                      << kTotalSize << " byte destination (" << perRecord
                      << " per record, budget " << kMaxBytesPerParkedRecord
                      << "). SPEC 3.3.4 requires an out-of-order chunk to park "
                         "{dstOffset, payloadSize} and NOTHING ELSE; this much state "
                         "means payload bytes are being copied into the parked record."
                      << std::endl;
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    std::vector<uint8_t> req(sizeof(ChunkHeader) + kChunkSize, 0xA5);

    MemInfo base;
    if (!readMem(base)) {
        std::cerr << "FAIL: could not read process memory counters" << std::endl;
        return 1;
    }

    // ---- PHASE 1: reverse order. Every chunk but the last arrives ahead of the
    // cursor, so this is the phase that sees the parked-payload duplication.
    // It runs first because PeakPagefileUsage never falls: the phase with the
    // larger old-behavior peak has to be measured against a clean baseline.
    MemInfo afterPhase1;
    {
        std::vector<uint32_t> reverse;
        for (uint32_t i = kTotalChunks; i-- > 0;) reverse.push_back(i);
        if (!driveStream(1, reverse, req)) return 1;
        if (!readMem(afterPhase1)) {
            std::cerr << "FAIL: could not read process memory counters" << std::endl;
            return 1;
        }
        if (!checkPeak("reverse_order_park", base, afterPhase1)) return 1;
    }

    // ---- PHASE 2: in-order. Nothing is ever parked, so this phase isolates the
    // completion path: the destination must be handed to the callback by move,
    // not concatenated into a second full-size buffer.
    {
        std::vector<uint32_t> inOrder;
        for (uint32_t i = 0; i < kTotalChunks; ++i) inOrder.push_back(i);
        if (!driveStream(2, inOrder, req)) return 1;
        MemInfo after;
        if (!readMem(after)) {
            std::cerr << "FAIL: could not read process memory counters" << std::endl;
            return 1;
        }
        if (!checkPeak("in_order_completion", base, after)) return 1;

        // Incremental bound — see kInterPhaseBudget. Phase 2 allocates the same
        // 64 MiB phase 1 already released, so its peak must not move.
        const size_t growth = after.peakPagefile > afterPhase1.peakPagefile
                                  ? after.peakPagefile - afterPhase1.peakPagefile
                                  : 0;
        std::cout << "in_order_completion: peak_growth_since_phase1=" << growth
                  << " budget=" << kInterPhaseBudget << std::endl;
        if (growth > kInterPhaseBudget) {
            std::cerr << "FAIL: in_order_completion: peak commit grew by " << growth
                      << " bytes over phase 1 (budget " << kInterPhaseBudget
                      << "). Phase 2 reuses memory phase 1 released, so any growth "
                         "is a second copy of the stream on the completion path."
                      << std::endl;
            return 1;
        }
    }

    // ---- PHASE 3: per-parked-record cost. Current commit, so unaffected by the
    // monotone peak the phases above raised.
    if (!checkParkedRecordCost(req)) return 1;

    std::cout << "Test Passed" << std::endl;
    return 0;
}
