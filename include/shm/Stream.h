#pragma once

#include "DirectHost.h"
#include <queue>
#include <vector>
#include <cstring>

namespace shm {

/** @brief Header payload for STREAM_START message. */
struct StreamHeader {
    uint64_t streamId;
    uint64_t totalSize;
    uint32_t totalChunks;
    uint32_t reserved;
};

// ABI safety: StreamHeader must remain exactly 24 bytes. Layout is frozen.
static_assert(sizeof(StreamHeader) == 24, "StreamHeader must be exactly 24 bytes (ABI)");

/** @brief Header payload for STREAM_CHUNK message. */
struct ChunkHeader {
    uint64_t streamId;
    uint32_t chunkIndex;
    uint32_t payloadSize;
    uint32_t reserved;
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
     */
    uint32_t padding;
};

// ABI safety: ChunkHeader must remain exactly 24 bytes for cross-language
// parity with Go's `ChunkHeader` (see `go/stream.go`) and SPECIFICATION.md
// §3.3.2. Layout is frozen.
static_assert(sizeof(ChunkHeader) == 24,
              "ChunkHeader must be 24 bytes for cross-language parity with Go and SPECIFICATION.md §3.3.2");

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
    Result<void> Send(const void* data, size_t size, uint64_t streamId) {
        if (!host || !data) return Result<void>::Failure(Error::InvalidArgs);

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

            StreamHeader header;
            header.streamId = streamId;
            header.totalSize = size;
            header.totalChunks = totalChunks;
            header.reserved = 0;

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

                 ChunkHeader ch;
                 ch.streamId = streamId;
                 ch.chunkIndex = chunkIndex;
                 ch.payloadSize = currentChunkSize;
                 ch.reserved = 0;

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
