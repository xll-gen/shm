#pragma once

#include "DirectHost.h"
#include <queue>
#include <vector>
#include <cstring>

#include <cstddef>   // offsetof

namespace shm {

/**
 * @brief Normative ceiling on a stream's `totalSize` (SPEC §3.3.4), 1 GiB.
 *
 * This is NOT a tuning knob. Two separate contracts pin it:
 *
 *  1. **Accept/reject parity.** Go's `MaxStreamSize` (go/stream.go) is a hard
 *     `const` at 1 GiB. SPEC §3.3.4 requires that a stream accepted by one peer
 *     is accepted by the other, so a C++ reassembler configured above this value
 *     would accept a `STREAM_START` its Go peer rejects outright.
 *  2. **`ChunkHeader::dstOffset` is a uint32.** It can only address the first
 *     4 GiB of a destination buffer; past that, both senders would truncate the
 *     offset and every chunk beyond the wrap would be placed at the wrong
 *     address. SPEC §3.3.2 states this dependency explicitly.
 *
 * The 1 GiB bound is the binding one — it is the stricter of the two and the one
 * SPEC states normatively — so it, not 4 GiB, is what is enforced. Raising it
 * requires a SPEC change plus the matching Go const, and past 4 GiB a
 * `dstOffset` widening, i.e. a wire version bump.
 */
static constexpr size_t kMaxStreamSize = static_cast<size_t>(1) << 30;

/** @brief Header payload for STREAM_START message. */
struct StreamHeader {
    uint64_t streamId;
    uint64_t totalSize;
    uint32_t totalChunks;
    /**
     * @brief Application-defined routing tag, OPAQUE to shm (0 = unspecified).
     *
     * Was `reserved` up to SHM_VERSION 0x00070000. shm neither interprets nor
     * validates it; it is carried so a consumer can route a completed stream
     * without inventing a second framing layer inside the payload.
     */
    uint32_t appMsgType;
};

// ABI safety: StreamHeader must remain exactly 24 bytes. Layout is frozen.
static_assert(sizeof(StreamHeader) == 24, "StreamHeader must be exactly 24 bytes (ABI)");
// R25 offset discipline, extended to the stream structs when appMsgType/dstOffset
// stopped being reserved (SHM_VERSION 0x00080000). A size assert alone cannot catch
// a field REORDERING, and both of these are now read by the peer.
static_assert(offsetof(StreamHeader, streamId) == 0, "StreamHeader.streamId@0");
static_assert(offsetof(StreamHeader, totalSize) == 8, "StreamHeader.totalSize@8");
static_assert(offsetof(StreamHeader, totalChunks) == 16, "StreamHeader.totalChunks@16");
static_assert(offsetof(StreamHeader, appMsgType) == 20, "StreamHeader.appMsgType@20");

/** @brief Header payload for STREAM_CHUNK message. */
struct ChunkHeader {
    uint64_t streamId;
    uint32_t chunkIndex;
    uint32_t payloadSize;
    /**
     * @brief ABSOLUTE byte offset of this chunk's payload within the stream.
     *
     * Was `reserved` up to SHM_VERSION 0x00070000. Giving it meaning is exactly
     * why 0x00080000 exists: pre-0x00080000 senders write 0 here, and 0 is a
     * LEGAL offset for chunk 0, so a new reader against an old sender would place
     * every chunk at offset 0 and corrupt the stream with nothing to detect it.
     * The version mismatch at attach is the only safe discriminator.
     *
     * INVARIANT the receiver enforces: `dstOffset + payloadSize <= totalSize`,
     * and `dstOffset` equals the sum of the payloadSizes of all lower indices
     * (checked as each index becomes in-order, so a misplacement anywhere is
     * caught before the stream can complete).
     *
     * u32 is sufficient only because maxStreamSize is 1 GiB. That is no longer
     * documentation-only: `shm::kMaxStreamSize` above is the enforced bound —
     * StreamSender::Send refuses a larger stream instead of truncating this cast,
     * and StreamReassembler's constructor refuses a config that raised it.
     */
    uint32_t dstOffset;
    /**
     * @brief Explicit padding to ensure cross-language size parity.
     *
     * Without this field the C++ struct would be 20 bytes (no trailing padding
     * is required by the natural alignment of `uint64_t streamId` on its own,
     * since the largest member is 8 bytes and the total of the prior fields is
     * already a multiple of 8). However, the Go counterpart in
     * `go/stream.go` carries an explicit `Padding uint32` field, making
     * `unsafe.Sizeof(ChunkHeader{}) == 24`. SPECIFICATION.md §3.3.2 likewise
     * defines the wire layout as 24 bytes, with `padding` at offset 20.
     *
     * The C++ sender writes the chunk payload at `offset = sizeof(ChunkHeader)`
     * and the Go reassembler reads it at the same offset on its side. Without
     * matching sizes the two ends disagree by 4 bytes on every chunk, silently
     * corrupting cross-language streams. This field forces parity and must not
     * be removed without updating Go and the SPECIFICATION in lockstep.
     *
     * A SENDER MUST WRITE ZERO HERE. This is the last reserved slot in
     * ChunkHeader, and SPEC §2.1's whole argument for why a field carved from
     * reserved space needs no version bump is that older peers wrote zero into it.
     * Shipping garbage — e.g. by default-initializing this struct and relying on
     * the stack — forfeits that carve-out for good, and leaks host stack bytes to
     * the peer process besides. Value-initialize (`ChunkHeader ch{}`) rather than
     * assigning field by field. Pinned by tests/test_stream_chunk_padding.cpp;
     * Go's putChunkHeader writes an explicit 0 (go/stream_sender.go).
     */
    uint32_t padding;
};

// ABI safety: ChunkHeader must remain exactly 24 bytes for cross-language
// parity with Go's `ChunkHeader` (see `go/stream.go`) and SPECIFICATION.md
// §3.3.2. Layout is frozen.
static_assert(sizeof(ChunkHeader) == 24,
              "ChunkHeader must be 24 bytes for cross-language parity with Go and SPECIFICATION.md §3.3.2");
static_assert(offsetof(ChunkHeader, streamId) == 0, "ChunkHeader.streamId@0");
static_assert(offsetof(ChunkHeader, chunkIndex) == 8, "ChunkHeader.chunkIndex@8");
static_assert(offsetof(ChunkHeader, payloadSize) == 12, "ChunkHeader.payloadSize@12");
static_assert(offsetof(ChunkHeader, dstOffset) == 16, "ChunkHeader.dstOffset@16");
// padding@20 is normative too (SPEC §3.3.2: senders MUST write 0, receivers MUST
// ignore it), and it is the last reserved slot in ChunkHeader — the only place the
// §2.1 "an older peer wrote zero here" carve-out can ever be used again. Go pins
// Padding@20 bidirectionally (go/stream.go), so this keeps the two guard sets
// literal mirrors (AGENTS.md R25) rather than one field short.
static_assert(offsetof(ChunkHeader, padding) == 20, "ChunkHeader.padding@20");

/**
 * @class StreamSender
 * @brief Helper class to send large data streams using double-buffering (or N-buffering).
 */
class StreamSender {
    DirectHost* host;
    int maxInFlight;
    int32_t baseSlot;

public:
    /**
     * @brief Constructs a StreamSender.
     * @param h Pointer to the initialized DirectHost.
     * @param inFlight Max number of slots to use concurrently. Default 1
     *        (v0.8.9, was 2): pipelining depth 2 measured strictly slower than
     *        depth 1 on every stream cell on the reference host, both with the
     *        shared pool AND with a co-located fixed slot range — the stream
     *        plateau is memory-controller-bound, so overlapping host/guest
     *        memcpys buys nothing, while a second slot doubles the working set
     *        and (with baseSlot) spans a second physical core, breaking the
     *        endpoint co-location. See BENCHMARK_RESULTS.md §2026-07-04 and
     *        the 2026-07-09 re-measure. Callers with topologies where overlap
     *        does help can still pass 2+ explicitly.
     * @param base Optional fixed slot range (v0.8.7). When >= 0 the sender draws
     *        its slots round-robin from [base, base+inFlight) via
     *        AcquireSpecificSlot instead of the shared pool (AcquireSlot). With
     *        one sender per pinned worker and inFlight==1, base == the worker's
     *        slot index co-locates the host sender thread and the Go guest
     *        worker that services the slot on the two SMT LPs of one physical
     *        core (shared L1d), the same win Direct-Exchange gets from
     *        AffinitySibling — the pool path instead hands out arbitrary slots
     *        whose Go workers are pinned to other cores, adding cross-core
     *        coherence traffic on every chunk. Default -1 keeps the pool path.
     *        For inFlight>1 co-location is only partial (the range spans several
     *        Go workers on different cores).
     */
    StreamSender(DirectHost* h, int inFlight = 1, int32_t base = -1)
        : host(h), maxInFlight(inFlight), baseSlot(base) {}

    /**
     * @brief Sends a large buffer as a stream.
     * Blocks until all chunks are sent and acknowledged.
     * Uses multiple slots in parallel (pipelining) to maximize throughput.
     *
     * @param data Pointer to the data to send.
     * @param size Size of the data in bytes.
     * @param streamId Unique identifier for this stream.
     * @return Result<void> Success or Error.
     */
    Result<void> Send(const void* data, size_t size, uint64_t streamId,
                      uint32_t appMsgType = 0) {
        if (!host || !data) return Result<void>::Failure(Error::InvalidArgs);

        // A sender MUST refuse a stream this wire format cannot express, rather
        // than truncating. `ch.dstOffset` below is a uint32 cast of an absolute
        // byte position: at size > 4 GiB the cast wraps and every chunk past the
        // wrap is placed at a wrong (but in-bounds, therefore undetectable)
        // address. And already at size > kMaxStreamSize the peer's reassembler
        // rejects the STREAM_START, so proceeding only burns slots on chunks that
        // will be refused. See kMaxStreamSize for why the bound is 1 GiB.
        //
        // ORDER IS LOAD-BEARING: this runs before any slot acquisition and before
        // `data` is dereferenced, so the guard is reachable without committing a
        // 1 GiB buffer (tests/test_stream_size_ceiling.cpp relies on that).
        if (size > kMaxStreamSize) return Result<void>::Failure(Error::InvalidArgs);

        // Slot acquisition: shared pool (baseSlot < 0) or a fixed round-robin
        // range [baseSlot, baseSlot+maxInFlight) for endpoint co-location. The
        // pool path blocks in AcquireSlot until a slot frees; AcquireSpecificSlot
        // likewise waits for its specific slot, which the FIFO drain below keeps
        // free by the time the rotation returns to it.
        uint32_t rr = 0;
        auto acquireSlot = [&]() -> int32_t {
            if (baseSlot < 0) return host->AcquireSlot();
            int32_t idx = baseSlot + (int32_t)(rr % (uint32_t)maxInFlight);
            int32_t got = host->AcquireSpecificSlot(idx);
            // Advance the rotation only on success so a timeout (which cannot
            // occur in the shipped single-sender/disjoint-range config, but
            // could under misuse or a stalled guest) leaves the next retry
            // targeting the same slot — keeping the rotation phase-locked to
            // the FIFO drain instead of skipping to a still-in-flight slot.
            if (got >= 0) rr++;
            return got;
        };

        // 1. Send Stream Start (Synchronous)
        {
            int slotIdx = acquireSlot();
            if (slotIdx < 0) return Result<void>::Failure(Error::ResourceExhausted);

            int32_t maxReq = host->GetMaxReqSize(slotIdx);
            int32_t chunkOverhead = sizeof(ChunkHeader);
            if (maxReq <= chunkOverhead) {
                 host->SendAcquiredAsync(slotIdx, 0, MsgType::SYSTEM_ERROR); // Abort
                 std::vector<uint8_t> dummy;
                 host->WaitForSlot(slotIdx, dummy); // Clean up
                 return Result<void>::Failure(Error::BufferTooSmall);
            }
            int32_t maxPayload = maxReq - chunkOverhead;

            uint32_t totalChunks = (uint32_t)((size + maxPayload - 1) / maxPayload);
            if (size == 0) totalChunks = 0;

            // VALUE-INITIALIZED, not default-initialized: the whole 24 bytes are
            // memcpy'd into the guest-visible segment below, so any byte this
            // function does not assign would put indeterminate HOST STACK on the
            // wire. StreamHeader happens to have all four fields assigned today;
            // `{}` is what keeps that true for the next field somebody adds.
            StreamHeader header{};
            header.streamId = streamId;
            header.totalSize = size;
            header.totalChunks = totalChunks;
            header.appMsgType = appMsgType;

            memcpy(host->GetReqBuffer(slotIdx), &header, sizeof(StreamHeader));

            std::vector<uint8_t> dummy;
            auto res = host->SendAcquired(slotIdx, sizeof(StreamHeader), MsgType::STREAM_START, dummy);
            if (res.HasError()) return Result<void>::Failure(res.GetError());
        }

        // 2. Send Chunks (Pipelined)
        const uint8_t* ptr = (const uint8_t*)data;
        size_t remaining = size;
        uint32_t chunkIndex = 0;

        std::queue<int32_t> pendingSlots;

        while (remaining > 0 || !pendingSlots.empty()) {
             // Fill pipeline
             while (remaining > 0 && (int)pendingSlots.size() < maxInFlight) {
                 int32_t slotIdx = acquireSlot();
                 if (slotIdx < 0) {
                     // No slots free, break to drain
                     break;
                 }

                 int32_t maxReq = host->GetMaxReqSize(slotIdx);
                 int32_t chunkOverhead = sizeof(ChunkHeader);
                 int32_t maxPayload = maxReq - chunkOverhead;

                 uint32_t currentChunkSize = (remaining > (size_t)maxPayload) ? (uint32_t)maxPayload : (uint32_t)remaining;

                 // VALUE-INITIALIZED (`{}`), which is what zeroes `padding`@20.
                 // All 24 bytes cross the process boundary in the memcpy below, so
                 // a field this function leaves unassigned leaks indeterminate host
                 // stack to the peer AND — the reason this is a protocol matter,
                 // not a hygiene one — forfeits the SPEC §2.1 carve-out that lets a
                 // future field be added to reserved space without a version bump.
                 // That argument rests entirely on older peers having written ZERO
                 // there; `padding`@20 is ChunkHeader's last reserved slot. Go's
                 // putChunkHeader writes an explicit 0 (go/stream_sender.go).
                 // Prefer `{}` over one more per-field `= 0`: the assignment is
                 // what the dstOffset change forgot when it replaced
                 // `ch.reserved = 0`. Pinned by tests/test_stream_chunk_padding.cpp.
                 ChunkHeader ch{};
                 ch.streamId = streamId;
                 ch.chunkIndex = chunkIndex;
                 ch.payloadSize = currentChunkSize;
                 // Absolute destination: how far into the stream this chunk sits.
                 // `ptr` has already been advanced past every prior chunk, so the
                 // consumed count is exactly that offset.
                 ch.dstOffset = (uint32_t)(size - remaining);

                 uint8_t* buf = host->GetReqBuffer(slotIdx);
                 memcpy(buf, &ch, sizeof(ChunkHeader));
                 memcpy(buf + sizeof(ChunkHeader), ptr, currentChunkSize);

                 auto res = host->SendAcquiredAsync(slotIdx, sizeof(ChunkHeader) + currentChunkSize, MsgType::STREAM_CHUNK);
                 if (res.HasError()) {
                      while (!pendingSlots.empty()) {
                          int32_t s = pendingSlots.front();
                          pendingSlots.pop();
                          std::vector<uint8_t> dummy;
                          host->WaitForSlot(s, dummy, 1);
                      }
                      return Result<void>::Failure(res.GetError());
                 }

                 pendingSlots.push(slotIdx);

                 ptr += currentChunkSize;
                 remaining -= currentChunkSize;
                 chunkIndex++;
             }

             // Drain one if pending
             if (!pendingSlots.empty()) {
                  int32_t s = pendingSlots.front();
                  std::vector<uint8_t> dummy;
                  auto res = host->WaitForSlot(s, dummy);
                  pendingSlots.pop();

                  if (res.HasError()) {
                       while (!pendingSlots.empty()) {
                           int32_t s2 = pendingSlots.front();
                           pendingSlots.pop();
                           std::vector<uint8_t> d2;
                           host->WaitForSlot(s2, d2, 1);
                       }
                       return Result<void>::Failure(res.GetError());
                  }
             }
        }

        return Result<void>::Success();
    }
};

}
