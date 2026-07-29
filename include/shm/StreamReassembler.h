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
    // Reassembly bounds. These defaults match the Go reference reassembler
    // (go/stream.go: MaxStreamSize / MaxStreamChunks / MaxConcurrentStreams)
    // and the SPECIFICATION.md §3.3.4 Reassembly Limits & Completion Contract.
    // A stream accepted by one peer must be accepted by the other.
    size_t maxStreamSize = 1 << 30;   // 1 GiB (bounds StreamHeader::totalSize)
    size_t maxStreamChunks = 1 << 20; // bounds StreamHeader::totalChunks
    size_t maxStreams = 1024;         // max concurrent in-flight streams
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
        // Running Sum(payloadSize) over every chunk index accepted so far, in
        // whatever order they arrived. This is what the SPEC §3.3.4 per-chunk
        // overflow guard bounds by totalSize: without it, a peer that advertises
        // a tiny totalSize and then sends only out-of-order indices has every
        // payload stored, and the byte-sum is not questioned until the
        // completion assembly loop — i.e. after all of the bytes are resident.
        uint64_t receivedBytes;
        std::vector<std::vector<uint8_t>> chunks;
        // Per-index presence flag. Mirrors Go's nil-vs-non-nil sentinel
        // (go/stream.go: chunks[i]==nil means "not yet received"): an empty
        // chunk vector is ambiguous because a legitimately zero-length payload
        // is also empty. Tracking presence explicitly keeps the dedup /
        // receivedChunks accounting identical to the Go reference, so the same
        // stream is accepted by both peers (SPEC §3.3.4).
        std::vector<bool> seen;
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

            // Bound checks (SPECIFICATION.md §3.3.4). A corrupt or malicious
            // header must not drive a huge allocation. totalChunks is checked
            // before the vector resize below so an oversized header cannot
            // trigger a giant allocation.
            if (header->totalSize > config.maxStreamSize ||
                header->totalChunks > config.maxStreamChunks) {
                 msgType = MsgType::SYSTEM_ERROR;
                 respSize = 0;
                 return true;
            }

            // Empty stream: totalChunks == 0 ⇔ totalSize == 0. A mismatch
            // (zero chunks but nonzero size, or vice versa) is a protocol
            // error and must be rejected, not delivered as empty.
            if (header->totalChunks == 0) {
                 if (header->totalSize != 0) {
                     msgType = MsgType::SYSTEM_ERROR;
                     respSize = 0;
                     return true;
                 }
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
            ctx.receivedBytes = 0;
            ctx.startTime = std::chrono::steady_clock::now();

            try {
                ctx.chunks.resize(ctx.totalChunks);
                ctx.seen.resize(ctx.totalChunks, false);
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

            // Idempotency: if this index was already received, ignore. Use the
            // explicit presence flag (not vector emptiness) so a zero-length
            // payload is still counted exactly once — matching go/stream.go.
            if (ctx.seen[header->chunkIndex]) {
                msgType = MsgType::NORMAL;
                respSize = 0;
                return true;
            }

            // Per-chunk overflow guard (SPEC §3.3.4), applied BEFORE the payload
            // is copied so an over-advertised stream never becomes resident.
            // The check is order-independent: it counts every accepted chunk,
            // including ones buffered while an earlier index is still missing.
            // Matches go/stream.go's streamContext.fits (offset+oooBytes).
            if (ctx.receivedBytes + static_cast<uint64_t>(header->payloadSize) >
                ctx.totalSize) {
                streams.erase(it);
                msgType = MsgType::SYSTEM_ERROR;
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

            ctx.seen[header->chunkIndex] = true;
            ctx.receivedChunks++;
            ctx.receivedBytes += header->payloadSize;

            if (ctx.receivedChunks == ctx.totalChunks) {
                // Completion validation (SPEC §3.3.4): the assembled byte count
                // must equal the advertised totalSize, else the stream is
                // dropped rather than delivered truncated/garbled. That
                // equality check below is the load-bearing one here — it is what
                // catches an UNDER-sized stream, which no per-chunk guard can
                // see. The `> ctx.totalSize` bound inside the loop cannot fire:
                // the arrival-time receivedBytes guard above already rejected
                // any stream that exceeded totalSize, and Sum(chunk.size()) ==
                // receivedBytes. It is kept as a cheap invariant re-assert over
                // peer-supplied lengths on the wire path.
                std::vector<uint8_t> fullData;
                try {
                    fullData.reserve(ctx.totalSize);
                    for (const auto& chunk : ctx.chunks) {
                        if (fullData.size() + chunk.size() > ctx.totalSize) {
                            streams.erase(it);
                            msgType = MsgType::SYSTEM_ERROR;
                            respSize = 0;
                            return true;
                        }
                        fullData.insert(fullData.end(), chunk.begin(), chunk.end());
                    }
                } catch(...) {
                     streams.erase(it);
                     msgType = MsgType::SYSTEM_ERROR;
                     respSize = 0;
                     return true;
                }

                if (fullData.size() != ctx.totalSize) {
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
