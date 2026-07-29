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
    // (go/stream.go: MaxStreamSize / MaxStreamChunks / MaxConcurrentStreams /
    // MaxParkedChunks) and the SPECIFICATION.md §3.3.4 Reassembly Limits &
    // Completion Contract. A stream accepted by one peer must be accepted by
    // the other.
    size_t maxStreamSize = 1 << 30;   // 1 GiB (bounds StreamHeader::totalSize)
    size_t maxStreamChunks = 1 << 20; // bounds StreamHeader::totalChunks
    size_t maxStreams = 1024;         // max concurrent in-flight streams
    /**
     * @brief Max chunks a single stream may hold parked ahead of its cursor.
     *
     * The §3.3.4 running-byte guard bounds the *payload* a stream can make
     * resident by its advertised totalSize. It cannot bound per-chunk
     * bookkeeping, for two independent reasons: the overhead of a parked entry
     * exists at zero payload, and a zero-length chunk passes the byte guard by
     * construction (`Fits(0)` holds for any live stream). Without a
     * count bound a peer advertises `totalSize = 1` with
     * `totalChunks = maxStreamChunks` and parks 2^20-1 zero-length chunks —
     * measured at ~80 B/entry on the Go side, i.e. ~80 MiB per stream, times
     * maxStreams.
     *
     * The default is derived as `maxStreamChunks / maxStreams`, which is the
     * bound that makes the *aggregate* parked-entry count across all in-flight
     * streams no larger than the chunk count of one maximal stream. With the
     * defaults that is 2^20 / 2^10 = 1024 entries per stream (~64 KiB of
     * bookkeeping here, ~80 KiB in Go) and ~64-80 MiB across 1024 streams. It
     * leaves two orders of magnitude of headroom over any legitimate sender:
     * StreamSender parks at most `maxInFlight - 1` chunks (default in-flight
     * is 1, so normally zero).
     *
     * Keep this value equal to Go's MaxParkedChunks — it is part of the §3.3.4
     * accept/reject parity contract, not a local tuning knob.
     */
    size_t maxParkedChunks = 1024;
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
    /**
     * @brief Reassembly state for one in-flight stream.
     *
     * Mirrors go/stream.go's streamContext: the destination buffer is allocated
     * once at STREAM_START and in-order chunks are copied straight into it at a
     * running cursor, while only chunks that arrive *ahead* of the cursor are
     * parked in a sparse map. There is deliberately no totalChunks-sized array:
     * an array indexed by chunk index costs O(totalChunks) even when the stream
     * advertises one byte (the old `chunks.resize(totalChunks)` +
     * `seen.resize(totalChunks)` layout measured ~24 MiB per stream at
     * totalChunks = 2^20), and the per-chunk vectors plus the completion-time
     * concatenation made peak residency 2 x totalSize. Regression tests:
     * tests/test_reassembler_tiny_stream_footprint.cpp,
     * tests/test_reassembler_completion_peak.cpp.
     */
    struct StreamContext {
        uint64_t totalSize = 0;
        uint32_t totalChunks = 0;
        uint32_t next = 0;    ///< next in-order chunk index to consume into buf
        uint64_t offset = 0;  ///< bytes written into buf so far (Σ payloadSize)
        /**
         * @brief Σ size of the chunks currently parked ahead of the cursor.
         *
         * `offset + oooBytes` is the running *received* length of the stream,
         * which is what the SPEC §3.3.4 per-chunk overflow guard bounds by
         * totalSize. Without counting parked bytes, a peer that only ever sends
         * ahead-of-cursor indices parks unbounded memory and is refused only
         * once the gap fills — after every byte is resident. Released exactly
         * when a parked chunk is drained into buf.
         */
        uint64_t oooBytes = 0;
        std::vector<uint8_t> buf;  ///< destination, size == totalSize
        /**
         * @brief Chunks received ahead of `next`, keyed by chunk index.
         *
         * Presence in the map (not emptiness of the value) is the dedup marker,
         * so a legitimately zero-length payload is counted exactly once —
         * matching Go's `ooo` map and the old `seen[]` bitmap.
         */
        std::unordered_map<uint32_t, std::vector<uint8_t>> ooo;
        std::chrono::steady_clock::time_point startTime;

        /**
         * @brief Whether accepting `n` more bytes keeps the running received
         *        length (assembled + parked) within totalSize.
         */
        bool Fits(size_t n) const {
            return offset + oooBytes + static_cast<uint64_t>(n) <= totalSize;
        }

        /** @brief Appends at the cursor. False if the running-length guard trips. */
        bool Consume(const uint8_t* data, size_t n) {
            if (!Fits(n)) return false;
            if (n > 0) std::memcpy(buf.data() + offset, data, n);
            offset += n;
            ++next;
            return true;
        }

        /**
         * @brief Buffers a chunk that arrived ahead of the cursor.
         *
         * Both guards run BEFORE the copy is allocated, so an over-advertised or
         * over-fragmented stream is rejected at arrival time instead of after
         * its bytes are resident. False means the caller must drop the stream.
         */
        bool Park(uint32_t index, const uint8_t* data, size_t n, size_t maxParked) {
            if (!Fits(n)) return false;
            if (ooo.size() >= maxParked) return false;
            ooo.emplace(index, std::vector<uint8_t>(data, data + n));
            oooBytes += n;
            return true;
        }
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
            // header must not drive a huge allocation. Both bounds are checked
            // before the destination buffer below is sized.
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
            ctx.startTime = std::chrono::steady_clock::now();

            try {
                // The one allocation this stream makes: exactly what the peer
                // advertised. Sized by totalSize, never by totalChunks.
                ctx.buf.resize(static_cast<size_t>(header->totalSize));
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

            if (header->chunkIndex >= ctx.totalChunks) {
                 msgType = MsgType::SYSTEM_ERROR;
                 respSize = 0;
                 return true;
            }

            // Idempotency (SPEC §3.3.4 "first arrival wins"): an index that is
            // already assembled or already parked is ACK-ignored without
            // consulting its payloadSize, so a duplicate that declares a
            // different length neither re-counts nor trips the overflow guard.
            // Presence in `ooo`, not emptiness of its value, is the marker so a
            // zero-length payload is counted exactly once — matching go/stream.go.
            if (header->chunkIndex < ctx.next ||
                ctx.ooo.find(header->chunkIndex) != ctx.ooo.end()) {
                msgType = MsgType::NORMAL;
                respSize = 0;
                return true;
            }

            const uint8_t* payloadPtr = static_cast<const uint8_t*>(req) + sizeof(ChunkHeader);
            bool corrupt = false;
            try {
                if (header->chunkIndex == ctx.next) {
                    // In-order fast path: copy straight from the slot buffer
                    // into the preallocated destination, then drain any parked
                    // chunks the cursor has caught up to.
                    bool ok = ctx.Consume(payloadPtr, header->payloadSize);
                    while (ok) {
                        auto parked = ctx.ooo.find(ctx.next);
                        if (parked == ctx.ooo.end()) break;
                        std::vector<uint8_t> buffered = std::move(parked->second);
                        ctx.ooo.erase(parked);
                        // These bytes move from "parked" to "assembled";
                        // releasing the count first keeps offset+oooBytes
                        // invariant across the drain, so a legitimately-sized
                        // stream is not rejected for bytes counted twice.
                        ctx.oooBytes -= buffered.size();
                        ok = ctx.Consume(buffered.data(), buffered.size());
                    }
                    if (!ok) corrupt = true;
                } else if (!ctx.Park(header->chunkIndex, payloadPtr,
                                     header->payloadSize, config.maxParkedChunks)) {
                    // Ahead of the cursor (pipelined sender): the destination
                    // offset is unknown until the gap fills, so park a copy —
                    // but only while the running received length still fits
                    // totalSize and the parked-entry bound holds.
                    corrupt = true;
                }
            } catch (...) {
                // Local allocation failure while parking. SPEC §3.3.4: a
                // reassembler that cannot store an accepted chunk drops the
                // stream. A retained context could never be completed — both
                // senders treat a chunk SYSTEM_ERROR as terminal for the whole
                // stream and never retry the index — so keeping it would only
                // pin totalSize bytes until the timeout prune, and would leave
                // this the one error path in the function that does not retire
                // the stream.
                corrupt = true;
            }

            if (!corrupt && ctx.next == ctx.totalChunks) {
                // Completion contract (SPEC §3.3.4): every index filled exactly
                // once AND Σ payloadSize == totalSize. The equality check is
                // what catches an UNDER-sized stream, which no per-chunk guard
                // can see; an over-sized one was already refused at arrival.
                if (ctx.offset == ctx.totalSize) {
                    // Hand the destination out by move: the old layout built a
                    // second full-size buffer here, doubling peak residency.
                    std::vector<uint8_t> fullData = std::move(ctx.buf);
                    streams.erase(it);
                    lock.unlock(); // Unlock before callback to prevent deadlock

                    onStream(header->streamId, fullData);

                    msgType = MsgType::NORMAL;
                    respSize = 0;
                    return true;
                }
                corrupt = true;
            }

            if (corrupt) {
                // Drop rather than deliver truncated/garbled data, and never
                // leave a context that can no longer complete.
                streams.erase(it);
                msgType = MsgType::SYSTEM_ERROR;
                respSize = 0;
                return true;
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
