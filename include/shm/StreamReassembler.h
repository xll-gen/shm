#pragma once

#include "Stream.h"
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstring>
#include <mutex>
#include <chrono>

namespace shm {

/**
 * @struct StreamReassemblerConfig
 * @brief Configuration limits for StreamReassembler.
 */
struct StreamReassemblerConfig {
    size_t maxStreamSize = 128 * 1024 * 1024; // 128MB
    size_t maxStreams = 100;
    uint32_t streamTimeoutMs = 10000;
};

/**
 * @class StreamReassembler
 * @brief Helper class to reassemble streams received via Guest Calls.
 * Thread-safe.
 */
class StreamReassembler {
public:
    /**
     * @brief Callback function invoked when a stream is fully reassembled.
     * @param streamId The ID of the stream.
     * @param data The reassembled data.
     */
    using OnStreamFn = std::function<void(uint64_t streamId, const std::vector<uint8_t>& data)>;

private:
    struct StreamContext {
        uint64_t totalSize;
        uint32_t totalChunks;
        uint32_t receivedChunks;
        std::vector<std::vector<uint8_t>> chunks;
        std::chrono::steady_clock::time_point startTime;
    };

    std::unordered_map<uint64_t, StreamContext> streams;
    std::mutex streamsMutex;
    OnStreamFn onStream;
    StreamReassemblerConfig config;

    void PruneInternal() {
        auto now = std::chrono::steady_clock::now();
        for (auto it = streams.begin(); it != streams.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.startTime).count();
            if (elapsed > config.streamTimeoutMs) {
                it = streams.erase(it);
            } else {
                ++it;
            }
        }
    }

public:
    /**
     * @brief Constructs a StreamReassembler.
     * @param callback Function to call when a stream is completed.
     * @param cfg Configuration object.
     */
    StreamReassembler(OnStreamFn callback, StreamReassemblerConfig cfg = StreamReassemblerConfig())
        : onStream(callback), config(cfg) {}

    /**
     * @brief Handles a guest call message.
     * Use this within your DirectHost::ProcessGuestCalls handler.
     *
     * @param req Pointer to request buffer.
     * @param reqSize Size of request.
     * @param resp Pointer to response buffer.
     * @param respSize Reference to response size (output).
     * @param msgType Reference to message type (input/output).
     * @return true if the message was a stream message and was handled.
     */
    bool Handle(const void* req, size_t reqSize, void* resp, size_t& respSize, MsgType& msgType) {
        if (msgType == MsgType::STREAM_START) {
            if (reqSize < sizeof(StreamHeader)) {
                msgType = MsgType::SYSTEM_ERROR;
                respSize = 0;
                return true;
            }

            const StreamHeader* header = static_cast<const StreamHeader*>(req);

            // Checks
            if (header->totalSize > config.maxStreamSize) {
                 msgType = MsgType::SYSTEM_ERROR;
                 respSize = 0;
                 return true;
            }

            // Empty Stream or Zero Chunks
            if (header->totalSize == 0 || header->totalChunks == 0) {
                 std::vector<uint8_t> empty;
                 onStream(header->streamId, empty);
                 msgType = MsgType::NORMAL;
                 respSize = 0;
                 return true;
            }

            std::lock_guard<std::mutex> lock(streamsMutex);
            if (streams.size() >= config.maxStreams) {
                PruneInternal();
                if (streams.size() >= config.maxStreams) {
                     msgType = MsgType::SYSTEM_ERROR; // Too many streams
                     respSize = 0;
                     return true;
                }
            }

            StreamContext ctx;
            ctx.totalSize = header->totalSize;
            ctx.totalChunks = header->totalChunks;
            ctx.receivedChunks = 0;
            ctx.startTime = std::chrono::steady_clock::now();

            try {
                ctx.chunks.resize(ctx.totalChunks);
            } catch (...) {
                msgType = MsgType::SYSTEM_ERROR; // OOM
                respSize = 0;
                return true;
            }

            streams[header->streamId] = std::move(ctx);

            msgType = MsgType::NORMAL;
            respSize = 0;
            return true;
        }

        if (msgType == MsgType::STREAM_CHUNK) {
            if (reqSize < sizeof(ChunkHeader)) {
                msgType = MsgType::SYSTEM_ERROR;
                respSize = 0;
                return true;
            }

            const ChunkHeader* header = static_cast<const ChunkHeader*>(req);
            if (reqSize < sizeof(ChunkHeader) + header->payloadSize) {
                 msgType = MsgType::SYSTEM_ERROR;
                 respSize = 0;
                 return true;
            }

            std::unique_lock<std::mutex> lock(streamsMutex);
            auto it = streams.find(header->streamId);
            if (it == streams.end()) {
                 msgType = MsgType::SYSTEM_ERROR;
                 respSize = 0;
                 return true;
            }

            StreamContext& ctx = it->second;

            if (header->chunkIndex >= ctx.chunks.size()) {
                 msgType = MsgType::SYSTEM_ERROR;
                 respSize = 0;
                 return true;
            }

            // Idempotency: If already received, ignore
            if (!ctx.chunks[header->chunkIndex].empty()) {
                msgType = MsgType::NORMAL;
                respSize = 0;
                return true;
            }

            const uint8_t* payloadPtr = static_cast<const uint8_t*>(req) + sizeof(ChunkHeader);
            try {
                ctx.chunks[header->chunkIndex].assign(payloadPtr, payloadPtr + header->payloadSize);
            } catch (...) {
                 msgType = MsgType::SYSTEM_ERROR;
                 respSize = 0;
                 return true;
            }

            ctx.receivedChunks++;

            if (ctx.receivedChunks == ctx.totalChunks) {
                // Assemble
                std::vector<uint8_t> fullData;
                try {
                    fullData.reserve(ctx.totalSize);
                    for (const auto& chunk : ctx.chunks) {
                        fullData.insert(fullData.end(), chunk.begin(), chunk.end());
                    }
                } catch(...) {
                     streams.erase(it);
                     msgType = MsgType::SYSTEM_ERROR;
                     respSize = 0;
                     return true;
                }

                streams.erase(it);
                lock.unlock(); // Unlock before callback to prevent deadlock

                onStream(header->streamId, fullData);
            }

            msgType = MsgType::NORMAL;
            respSize = 0;
            return true;
        }

        return false;
    }

    /**
     * @brief Prunes timed-out streams.
     */
    void Prune() {
        std::lock_guard<std::mutex> lock(streamsMutex);
        PruneInternal();
    }
};

}
