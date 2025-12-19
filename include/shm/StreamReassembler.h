#pragma once

#include "DirectHost.h"
#include "Stream.h"
#include <map>
#include <vector>
#include <mutex>
#include <functional>
#include <cstring>
#include <chrono>

namespace shm {

/**
 * @struct StreamReassemblerConfig
 * @brief Configuration limits for StreamReassembler.
 */
struct StreamReassemblerConfig {
    uint64_t maxStreamSize = 256 * 1024 * 1024; // 256 MB Limit
    uint32_t maxChunks = 65536;                 // 64k chunks
};

/**
 * @class StreamReassembler
 * @brief Helper class to reassemble streams received via Guest Calls.
 */
class StreamReassembler {
public:
    using OnStreamCallback = std::function<void(uint64_t streamId, const std::vector<uint8_t>& data)>;
    using FallbackHandler = std::function<int32_t(const uint8_t*, int32_t, uint8_t*, uint32_t, MsgType)>;

private:
    struct StreamContext {
        uint64_t totalSize;
        uint32_t totalChunks;
        uint32_t chunksReceived;
        std::vector<std::vector<uint8_t>> chunks; // Store chunks until reassembly
        std::chrono::steady_clock::time_point startTime;
    };

    std::map<uint64_t, StreamContext> streams;
    std::mutex mutex;
    OnStreamCallback onStream;
    FallbackHandler fallback;
    StreamReassemblerConfig config;

public:
    /**
     * @brief Constructs a StreamReassembler.
     * @param onStreamCb Callback invoked when a stream is fully reassembled.
     * @param fallbackCb Callback invoked for non-stream messages.
     * @param cfg Configuration limits.
     */
    StreamReassembler(OnStreamCallback onStreamCb, FallbackHandler fallbackCb = nullptr, StreamReassemblerConfig cfg = StreamReassemblerConfig())
        : onStream(onStreamCb), fallback(fallbackCb), config(cfg) {}

    /**
     * @brief Handles a Guest Call message.
     * Matches the signature required by DirectHost::ProcessGuestCalls.
     */
    int32_t Handle(const uint8_t* reqData, int32_t reqSize, uint8_t* respBuf, uint32_t maxRespSize, MsgType msgType) {
        if (msgType == MsgType::STREAM_START) {
            if (reqSize < (int32_t)sizeof(StreamHeader)) {
                return 0; // Error, but we just ACK with 0 to clear slot
            }
            StreamHeader header;
            memcpy(&header, reqData, sizeof(StreamHeader));

            if (header.totalSize > config.maxStreamSize) {
                // Reject: Too large
                return 0;
            }
            if (header.totalChunks > config.maxChunks) {
                // Reject: Too many chunks (DoS protection for vector resize)
                return 0;
            }
            if (header.totalChunks == 0 && header.totalSize > 0) return 0; // Invalid

            std::unique_lock<std::mutex> lock(mutex);
            StreamContext& ctx = streams[header.streamId];
            ctx.totalSize = header.totalSize;
            ctx.totalChunks = header.totalChunks;
            ctx.chunksReceived = 0;
            try {
                ctx.chunks.resize(header.totalChunks);
            } catch (...) {
                streams.erase(header.streamId);
                return 0; // Alloc fail
            }
            ctx.startTime = std::chrono::steady_clock::now();

            return 0; // ACK
        }

        if (msgType == MsgType::STREAM_CHUNK) {
            if (reqSize < (int32_t)sizeof(ChunkHeader)) {
                return 0;
            }
            ChunkHeader header;
            memcpy(&header, reqData, sizeof(ChunkHeader));

            uint32_t headerSize = sizeof(ChunkHeader);
            if ((int32_t)header.payloadSize > reqSize - (int32_t)headerSize) {
                // Truncated payload?
                return 0;
            }

            std::unique_lock<std::mutex> lock(mutex);
            auto it = streams.find(header.streamId);
            if (it == streams.end()) {
                return 0; // Unknown stream (or timed out/rejected)
            }

            StreamContext& ctx = it->second;
            if (header.chunkIndex >= ctx.chunks.size()) {
                return 0; // Invalid index
            }

            if (ctx.chunks[header.chunkIndex].empty()) {
                ctx.chunks[header.chunkIndex].assign(reqData + headerSize, reqData + headerSize + header.payloadSize);
                ctx.chunksReceived++;
            }

            if (ctx.chunksReceived == ctx.totalChunks) {
                // Reassemble
                std::vector<uint8_t> fullData;
                try {
                    fullData.reserve(ctx.totalSize);
                    for (const auto& chunk : ctx.chunks) {
                        fullData.insert(fullData.end(), chunk.begin(), chunk.end());
                    }
                } catch(...) {
                     streams.erase(it);
                     return 0;
                }

                uint64_t sId = header.streamId;
                streams.erase(it);
                lock.unlock(); // Explicit unlock before callback

                if (onStream) {
                    onStream(sId, fullData);
                }
            }

            return 0; // ACK
        }

        if (fallback) {
            return fallback(reqData, reqSize, respBuf, maxRespSize, msgType);
        }

        return 0; // Default ACK
    }

    /**
     * @brief Removes incomplete streams that have exceeded the timeout.
     * Call this method periodically.
     * @param timeout Max duration a stream can stay in memory.
     */
    void Prune(std::chrono::milliseconds timeout) {
        auto now = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(mutex);
        for (auto it = streams.begin(); it != streams.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.startTime);
            if (elapsed > timeout) {
                 it = streams.erase(it);
            } else {
                 ++it;
            }
        }
    }
};

}
