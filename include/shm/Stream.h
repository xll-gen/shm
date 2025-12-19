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

/** @brief Header payload for STREAM_CHUNK message. */
struct ChunkHeader {
    uint64_t streamId;
    uint32_t chunkIndex;
    uint32_t payloadSize;
    uint32_t reserved;
};

/**
 * @class StreamSender
 * @brief Helper class to send large data streams using double-buffering (or N-buffering).
 */
class StreamSender {
    DirectHost* host;
    int maxInFlight;

public:
    /**
     * @brief Constructs a StreamSender.
     * @param h Pointer to the initialized DirectHost.
     * @param inFlight Max number of slots to use concurrently (default 2 for double buffering).
     */
    StreamSender(DirectHost* h, int inFlight = 2) : host(h), maxInFlight(inFlight) {}

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

        // 1. Send Stream Start (Synchronous)
        {
            int slotIdx = host->AcquireSlot();
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
                 int32_t slotIdx = host->AcquireSlot();
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
