#pragma once

#include "Stream.h"
#include "Logger.h"
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstring>
#include <mutex>
#include <chrono>
#include <type_traits>
#include <utility>   // std::pair (StreamSnapshot::placed)

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
    /**
     * @brief Bounds StreamHeader::totalSize. 1 GiB, and it MUST NOT be raised.
     *
     * This member is public and mutable for symmetry with the rest of the config,
     * but unlike the others it is NOT a tuning knob — see `shm::kMaxStreamSize`
     * (Stream.h) for the two contracts that pin it (accept/reject parity with
     * Go's hard `MaxStreamSize` const, and `ChunkHeader::dstOffset` being a
     * uint32). A value above `kMaxStreamSize` is refused: `IsValid()` reports it
     * and the StreamReassembler constructor clamps it back down, so a host that
     * raises it cannot end up accepting a STREAM_START its Go peer rejects. Before
     * that clamp existed the knob silently violated a SPEC MUST-precondition.
     */
    size_t maxStreamSize = kMaxStreamSize; // 1 GiB (bounds StreamHeader::totalSize)
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

    /**
     * @brief Whether this configuration is expressible on the wire.
     *
     * Currently exactly one condition: `maxStreamSize <= shm::kMaxStreamSize`.
     * Callers that want to detect the mistake themselves can check this; callers
     * that do not get the clamp in StreamReassembler's constructor instead.
     */
    bool IsValid() const { return maxStreamSize <= kMaxStreamSize; }
};

// The default configuration must itself be expressible: this catches a future
// edit that raises the default past the wire ceiling at COMPILE time, which no
// runtime clamp or test can do. StreamReassemblerConfig is a literal type, so the
// default member initializers are readable in a constant expression.
static_assert(StreamReassemblerConfig{}.maxStreamSize <= kMaxStreamSize,
              "default maxStreamSize exceeds what a uint32 ChunkHeader::dstOffset "
              "can address / what Go's MaxStreamSize accepts (see kMaxStreamSize)");

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
        uint32_t next = 0;    ///< next in-order chunk index to account for
        uint64_t offset = 0;  ///< bytes accounted in order (Σ payloadSize of 0..next-1)
        /**
         * @brief Σ payloadSize of chunks PLACED but not yet accounted in order.
         *
         * `offset + oooBytes` is the running *received* length, which is what the
         * SPEC §3.3.4 per-chunk overflow guard bounds by totalSize. Without
         * counting these, a peer that only ever sends ahead-of-cursor indices
         * would be refused only once the gap filled — after every byte is
         * resident. Released as each index is accounted in order.
         */
        uint64_t oooBytes = 0;
        std::vector<uint8_t> buf;  ///< destination, size == totalSize

        /**
         * @brief Ahead-of-cursor chunks: index -> (dstOffset, payloadSize).
         *
         * SINCE SHM_VERSION 0x00080000 THIS HOLDS NO PAYLOAD. Every chunk is
         * written straight to buf[dstOffset] on arrival, whatever the order, so
         * an out-of-order chunk costs 12 bytes of bookkeeping instead of a full
         * copy of itself — and the drain no longer memcpys a second time. Before
         * this, an out-of-order chunk was duplicated in a parked vector and then
         * copied again into buf.
         *
         * The map REMAINS, and that is deliberate: presence is the dedup marker
         * (so a legitimately zero-length payload is counted exactly once), and it
         * keeps the count-dimension bound meaningful. Replacing it with "just add
         * up the bytes" is unsound — with dstOffset placement and no dedup, two
         * copies of chunk 0 (len 5) of a totalSize=10/totalChunks=2 stream give
         * receivedChunks=2 and receivedBytes=10 == totalSize, so the stream
         * COMPLETES with its second half never written.
         *
         * In-order arrivals never enter this map, so a conforming sender
         * (ascending, in-flight 1 — both library defaults) keeps it empty no
         * matter how many chunks the stream has.
         */
        struct Placed {
            uint64_t dstOffset;
            uint32_t size;
        };
        /**
         * STRUCTURAL guard for the SPEC §3.3.4 "parks bookkeeping only, never
         * payload bytes" MUST, and the C++ analogue of Go's
         * TestStreamReassembly_ParkedRecordHoldsNoPayload. A byte-budget test can
         * only notice a payload copy once the copies are large enough to clear its
         * slack; these two notice the *type*, at compile time, before any byte is
         * copied.
         *
         * WHAT THEY ACTUALLY PIN, precisely: the record stays 16 bytes and stays
         * trivially copyable. Together those exclude every OWNING member — a
         * std::vector or std::string is both larger than 16 and non-trivially
         * copyable — and any inline array big enough to matter. They do NOT
         * exclude a parked copy in general: `{uint64 dstOffset; uint32 size;
         * uint8 payload[4];}` is still 16 bytes and still trivially copyable, and
         * a side container in StreamReassembler holding payloads per index is
         * outside this type entirely. So these asserts bound the *shape* of the
         * per-index record; they are not a proof that no payload is copied, and
         * they say nothing at all about the residual risk that a chunk is written
         * at the running cursor instead of at its dstOffset. That risk is guarded
         * only by tests/test_reassembler_dstoffset_placement.cpp (C++) and
         * go/stream_dstoffset_placement_test.go (Go), via the PeekStream seam.
         *
         * 16 = 8 (dstOffset) + 4 (size) + 4 tail padding to the uint64 alignment.
         * A field added here is a deliberate act: update the number AND SPEC
         * §3.3.4, or do not add it.
         */
        static_assert(sizeof(Placed) == 16,
                      "StreamContext::Placed must stay a 16-byte payload-free record "
                      "(SPEC 3.3.4: out-of-order arrival parks bookkeeping only)");
        static_assert(std::is_trivially_copyable<Placed>::value,
                      "StreamContext::Placed must stay trivially copyable — an owning "
                      "member (vector/string) is how a parked payload copy comes back");
        std::unordered_map<uint32_t, Placed> ooo;
        std::chrono::steady_clock::time_point startTime;

        /**
         * @brief Whether accepting `n` more bytes keeps the running received
         *        length (assembled + placed) within totalSize.
         */
        bool Fits(size_t n) const {
            return offset + oooBytes + static_cast<uint64_t>(n) <= totalSize;
        }

        /**
         * @brief Whether [dstOffset, dstOffset+n) lies inside the advertised buffer.
         *
         * dstOffset is PEER-CONTROLLED, so this is subtraction rather than
         * addition: `dstOffset + n` could wrap. totalSize is the buffer's actual
         * size, so this is the bounds check for the memcpy below.
         */
        bool InBounds(uint64_t dstOffset, size_t n) const {
            if (dstOffset > totalSize) return false;
            return static_cast<uint64_t>(n) <= totalSize - dstOffset;
        }

        /** @brief Writes a chunk to its absolute destination. Bounds-checked by the caller. */
        void Write(uint64_t dstOffset, const uint8_t* data, size_t n) {
            if (n > 0) std::memcpy(buf.data() + dstOffset, data, n);
        }

        /** @brief Accounts an in-order chunk that has already been written. */
        void AccountInOrder(size_t n) {
            offset += n;
            ++next;
        }

        /**
         * @brief Records an ahead-of-cursor chunk that has already been written.
         *
         * Both guards run BEFORE anything is recorded, so an over-advertised or
         * over-fragmented stream is refused at arrival time. False means the
         * caller must drop the stream.
         */
        bool NotePlaced(uint32_t index, uint64_t dstOffset, size_t n, size_t maxParked) {
            if (ooo.size() >= maxParked) return false;
            ooo.emplace(index, Placed{dstOffset, static_cast<uint32_t>(n)});
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
        : onStream(callback), config(cfg) {
        // maxStreamSize is a wire-format precondition, not a knob (SPEC §3.3.4,
        // shm::kMaxStreamSize). A host that raised it would accept a STREAM_START
        // the Go peer rejects outright — an accept/reject parity break — and, past
        // 4 GiB, would accept a stream whose later chunks cannot even state their
        // destination in ChunkHeader::dstOffset's 32 bits. So the over-set value
        // is clamped here rather than honoured: refusing the configuration is the
        // only outcome that preserves parity, and the alternative (honour it and
        // let the peers disagree) is precisely the silent violation being fixed.
        if (!config.IsValid()) {
            SHM_LOG_WARN("StreamReassemblerConfig::maxStreamSize ", config.maxStreamSize,
                         " exceeds the wire ceiling ", kMaxStreamSize,
                         " (SPEC 3.3.4); clamped. Raising it breaks accept/reject "
                         "parity with the Go peer and overflows ChunkHeader::dstOffset.");
            config.maxStreamSize = kMaxStreamSize;
        }
    }

    /**
     * @brief The configuration actually in force, after the constructor's clamp.
     *
     * Exposed so a caller (and tests/test_stream_size_ceiling.cpp) can observe
     * that an out-of-range `maxStreamSize` did not survive construction.
     */
    const StreamReassemblerConfig& Config() const { return config; }

    /**
     * @brief Copy of one in-flight stream's reassembly state, as returned by
     *        PeekStream.
     *
     * Deliberately a snapshot of plain values, not a handle on StreamContext: the
     * observation must not hand out a reference into state the handler keeps
     * mutating under streamsMutex, and StreamContext itself stays private.
     */
    struct StreamSnapshot {
        uint64_t totalSize = 0;
        uint32_t totalChunks = 0;
        uint32_t next = 0;     ///< next in-order chunk index to account for
        uint64_t offset = 0;   ///< bytes accounted in order
        uint64_t oooBytes = 0; ///< bytes placed ahead of the cursor
        std::vector<uint8_t> buf; ///< the destination buffer AS IT STANDS RIGHT NOW
        /** @brief index -> (dstOffset, size) for every index recorded ahead of the cursor. */
        std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> placed;
    };

    /**
     * @brief Observation seam: snapshots an in-flight stream. False if there is no
     *        stream with that id (delivered, dropped, never started).
     *
     * It exists for ONE property that is unobservable from outside, and it is the
     * C++ counterpart of Go's `newStreamReassemblerPeek` (go/stream.go): whether an
     * ahead-of-cursor chunk's payload is ALREADY at `buf[dstOffset]` while the
     * stream is still incomplete. Every other observable — delivered bytes, parity
     * digests, accept/reject replies — is identical between a dstOffset-honouring
     * receiver and one that ignores dstOffset and appends at its own cursor,
     * because placement check 2 forces dstOffset to equal the cursor for every
     * index at the moment it becomes in-order (SPEC §3.3.4). `buf` is handed to
     * onStream only at completion, by which point both shapes have filled it, so
     * the distinguishing observation has to be taken MID-STREAM.
     *
     * Takes streamsMutex, so it is safe to call while other threads drive Handle.
     * Regression: tests/test_reassembler_dstoffset_placement.cpp. Copying `buf`
     * costs totalSize, which is why this is a diagnostic/test entry point and not
     * something to call on a hot path.
     */
    bool PeekStream(uint64_t streamId, StreamSnapshot& out) {
        std::lock_guard<std::mutex> lock(streamsMutex);
        auto it = streams.find(streamId);
        if (it == streams.end()) return false;
        const StreamContext& ctx = it->second;
        out.totalSize = ctx.totalSize;
        out.totalChunks = ctx.totalChunks;
        out.next = ctx.next;
        out.offset = ctx.offset;
        out.oooBytes = ctx.oooBytes;
        out.buf = ctx.buf;
        out.placed.clear();
        for (const auto& entry : ctx.ooo) {
            out.placed.emplace(entry.first,
                               std::make_pair(entry.second.dstOffset, entry.second.size));
        }
        return true;
    }

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
            // Age-prune on EVERY start, matching the Go reassembler (go/stream.go).
            // Previously this ran only on hitting maxStreams, so a host that never
            // calls Prune() and stays under the cap held every abandoned context —
            // each one pinning its full advertised totalSize — for the life of the
            // process. SPEC §3.3.4 leaves the reclaim mechanism
            // implementation-defined, so that was not a contract violation, but it
            // was an asymmetry with real memory behind it: the two implementations
            // are supposed to be interchangeable peers.
            //
            // Cost is one pass over at most maxStreams (1024) entries, on a path
            // that is already allocating totalSize bytes for the new stream.
            PruneInternal();
            if (streams.size() >= config.maxStreams) {
                 // The pre-existing second prune here is now redundant: the cap
                 // check is reached with a freshly pruned map.
                 msgType = MsgType::SYSTEM_ERROR; // Too many streams
                 respSize = 0;
                 return true;
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
                // Guards BEFORE the copy, in the order SPEC §3.3.4 fixes: the
                // running-length bound, then the destination bounds. dstOffset is
                // peer-controlled, so InBounds is the check that keeps the memcpy
                // inside buf.
                if (!ctx.Fits(header->payloadSize) ||
                    !ctx.InBounds(header->dstOffset, header->payloadSize)) {
                    corrupt = true;
                } else {
                    // EVERY chunk goes straight to its final home, whatever the
                    // arrival order. This is what dstOffset bought: an
                    // out-of-order chunk is no longer copied into a parked vector
                    // and then copied again at drain time.
                    ctx.Write(header->dstOffset, payloadPtr, header->payloadSize);

                    if (header->chunkIndex == ctx.next) {
                        // In-order. The offset a conforming sender puts on chunk
                        // `next` is exactly the running cursor, so this equality
                        // is the misplacement check -- and it is exact, not
                        // heuristic: it runs for every index once that index
                        // becomes in-order, so a sender that misplaces ANY chunk
                        // is caught by the time the stream would complete. That is
                        // what lets this keep index dedup instead of tracking
                        // covered intervals.
                        if (header->dstOffset != ctx.offset) {
                            corrupt = true;
                        } else {
                            ctx.AccountInOrder(header->payloadSize);
                            // Drain the indices the cursor has caught up to. No
                            // payload moves here any more -- the bytes are already
                            // in place; only the bookkeeping advances.
                            while (!corrupt) {
                                auto placed = ctx.ooo.find(ctx.next);
                                if (placed == ctx.ooo.end()) break;
                                const StreamContext::Placed p = placed->second;
                                ctx.ooo.erase(placed);
                                // Move these bytes from "placed" to "assembled"
                                // before re-accounting, so offset+oooBytes stays
                                // invariant and a legitimately-sized stream is not
                                // rejected for bytes counted twice.
                                ctx.oooBytes -= p.size;
                                if (p.dstOffset != ctx.offset) {
                                    corrupt = true;   // misplaced, caught on drain
                                    break;
                                }
                                ctx.AccountInOrder(p.size);
                            }
                        }
                    } else if (!ctx.NotePlaced(header->chunkIndex, header->dstOffset,
                                               header->payloadSize, config.maxParkedChunks)) {
                        // Ahead of the cursor (pipelined sender). The payload is
                        // already written; only the 12-byte record is bounded.
                        corrupt = true;
                    }
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
