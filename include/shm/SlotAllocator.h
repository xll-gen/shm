#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <functional>
#include <atomic>
#include <chrono>
#include "Platform.h"
#include "IPCUtils.h"
#include "WaitStrategy.h"
#include "Result.h"
#include "Logger.h"

namespace shm {

/**
 * @brief Internal representation of a Slot.
 *
 * Promoted to namespace scope (R44 refactor) because all three cooperating
 * pieces — SlotAllocator, GuestCallWorker, and the DirectHost façade — refer
 * to it. Fields, default constructor, and move constructor are unchanged from
 * the original DirectHost::Slot.
 */
struct Slot {
    SlotHeader* header;
    uint8_t* reqBuffer;
    uint8_t* respBuffer;
    EventHandle hReqEvent;  // Signaled by Host (Wake Guest)
    EventHandle hRespEvent; // Signaled by Guest (Wake Host)
    uint32_t maxReqSize;
    uint32_t maxRespSize;
    WaitStrategy waitStrategy;
    uint32_t msgSeq;
    std::atomic<bool> activeWait;

    Slot() : header(nullptr), reqBuffer(nullptr), respBuffer(nullptr),
             hReqEvent(nullptr), hRespEvent(nullptr),
             maxReqSize(0), maxRespSize(0), msgSeq(0), activeWait(false) {}

    Slot(Slot&& other) noexcept : waitStrategy(other.waitStrategy) {
        header = other.header;
        reqBuffer = other.reqBuffer;
        respBuffer = other.respBuffer;
        hReqEvent = other.hReqEvent;
        hRespEvent = other.hRespEvent;
        maxReqSize = other.maxReqSize;
        maxRespSize = other.maxRespSize;
        msgSeq = other.msgSeq;
        activeWait.store(other.activeWait.load());

        other.header = nullptr;
        other.hReqEvent = nullptr;
        other.hRespEvent = nullptr;
    }
};

/**
 * @class SlotAllocator
 * @brief Owns the slot state machine (R44 refactor — extracted from DirectHost).
 *
 * Holds the slot vector, round-robin hint, msgSeq stride, response timeout,
 * and the auto-reclaim threshold. Implements slot acquisition, abandoned-slot
 * reclamation, the shared adaptive wait, and the acquired-slot send paths.
 *
 * This is a pure mechanical, behavior-preserving extraction: every atomic's
 * memory_order, the slot state machine (SLOT_FREE/BUSY/REQ_READY/RESP_READY),
 * lease/heartbeat handshake, msgSeq stride, and load-bearing comments are
 * carried over verbatim. The `running` flag remains owned by the DirectHost
 * façade and is observed here through a pointer to avoid any drift between the
 * two lifetime views.
 */
class SlotAllocator {
public:
    static const uint32_t USE_DEFAULT_TIMEOUT = 0xFFFFFFFF;

    std::vector<Slot> slots;
    std::atomic<uint32_t> nextSlot{0}; // Round-robin hint

    uint32_t numSlots = 0;
    uint32_t numGuestSlots = 0;

    /**
     * @brief Stride for Message Sequence generation.
     * Used to ensure global uniqueness of msgSeq without atomic contention.
     * msgSeq = (previous_seq) + msgSeqStride.
     */
    uint32_t msgSeqStride = 0;

    uint32_t responseTimeoutMs = 10000;

    // v0.7.2: auto-reclaim threshold. When AcquireSlot's slow path
    // completes a configurable number of full sweeps without finding a
    // free slot, it attempts TryReclaimAbandonedSlot on each non-FREE
    // slot with this threshold. Zero (default) disables auto-reclaim —
    // callers who want it must SetAutoReclaimTimeoutNs(N).
    std::atomic<uint64_t> autoReclaimTimeoutNs{0};

    // Lifetime gate owned by the DirectHost façade. AcquireSlot /
    // AcquireSpecificSlot reject when the host is not running. Pointed at
    // DirectHost::running by DirectHost so there is a single source of truth.
    const bool* running = nullptr;

    /**
     * @brief Shared adaptive wait for SLOT_RESP_READY on an already-armed slot.
     *
     * Both the synchronous send path (WaitResponse) and the async completion
     * path (WaitForSlot) need the identical spin/yield/event-wait loop, with its
     * 0xFFFFFFFF infinite-timeout handling and 100ms wait chunks. They used to
     * open-code byte-identical copies whose infinite-timeout branches could
     * silently drift apart — the classic "one path hangs forever" hazard. This
     * is now the single definition (R44).
     *
     * Precondition: the caller has already published the request
     * (state==SLOT_REQ_READY) and signaled the guest, and has set
     * hostState=ACTIVE + activeWait=true. This performs ONLY the wait — it does
     * NOT publish the request, signal the guest, or clear activeWait; the caller
     * owns those (the two paths arm the slot differently).
     *
     * @param slot Pointer to the slot to wait on.
     * @param timeoutMs Timeout in milliseconds (0xFFFFFFFF = infinite).
     * @return true if response is ready, false on timeout.
     */
    bool WaitForRespReady(Slot* slot, uint32_t timeoutMs) {
        auto checkReady = [&]() -> bool {
            return slot->header->state.load(std::memory_order_acquire) == SLOT_RESP_READY;
        };

        auto sleepAction = [&]() {
            slot->header->hostState.store(HOST_STATE_WAITING, std::memory_order_seq_cst);

            if (checkReady()) {
                slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
                return;
            }

            auto start = std::chrono::steady_clock::now();
            while (!checkReady()) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

                if (timeoutMs != 0xFFFFFFFF && elapsed >= timeoutMs) {
                    SHM_LOG_DEBUG("WaitForRespReady timeout after ", elapsed, "ms");
                    break; // Timeout, return and let caller fail
                }

                uint32_t remaining = (timeoutMs == 0xFFFFFFFF) ? 0xFFFFFFFF : (timeoutMs - (uint32_t)elapsed);
                uint32_t waitTime = (remaining > 100) ? 100 : remaining;
                if (timeoutMs == 0xFFFFFFFF) waitTime = 100; // Cap infinite wait chunks

                Platform::WaitEvent(slot->hRespEvent, waitTime);
            }
            slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
        };

        return slot->waitStrategy.Wait(checkReady, sleepAction);
    }

    /**
     * @brief Internal helper to wait for a response on a specific slot.
     * Publishes the request + signals the guest, then runs the shared wait
     * (see WaitForRespReady).
     * @param slot Pointer to the slot to wait on.
     * @param timeoutMs Timeout in milliseconds.
     * @return true if response is ready, false if error/timeout.
     */
    bool WaitResponse(Slot* slot, uint32_t timeoutMs) {
        slot->activeWait.store(true, std::memory_order_relaxed);

        slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);

        slot->header->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

        if (slot->header->guestState.load(std::memory_order_seq_cst) == GUEST_STATE_WAITING) {
            Platform::SignalEvent(slot->hReqEvent);
        }

        bool ready = WaitForRespReady(slot, timeoutMs);

        slot->activeWait.store(false, std::memory_order_release);
        return ready;
    }

    /**
     * @brief Acquires a free slot for Zero-Copy usage.
     * @return The index of the acquired slot, or -1 if not running.
     */
    int32_t AcquireSlot() {
        if (!*running) return -1;

        static thread_local uint32_t cachedSlotIdx = 0xFFFFFFFF;
        Slot* slot = nullptr;
        int32_t resultIdx = -1;

        if (cachedSlotIdx < numSlots) {
            Slot& s = slots[cachedSlotIdx];
            uint32_t current = s.header->state.load(std::memory_order_acquire);
            bool canClaim = (current == SLOT_FREE);
            if (!canClaim) {
                 if (current == SLOT_RESP_READY || current == SLOT_REQ_READY || current == SLOT_GUEST_BUSY) {
                      if (!s.activeWait.load(std::memory_order_acquire)) {
                          canClaim = true;
                      }
                 }
            }

            if (canClaim) {
                // Bump the claim generation BEFORE the claiming CAS so a
                // concurrent reclaimer's generation handshake sees the
                // in-flight claim (SPECIFICATION.md §3.6.1).
                s.header->gen.fetch_add(1, std::memory_order_acq_rel);
                if (s.header->state.compare_exchange_strong(current, SLOT_BUSY, std::memory_order_acquire)) {
                    s.header->lease.store(Platform::MonotonicNanos(), std::memory_order_release);
                    slot = &s;
                    resultIdx = (int32_t)cachedSlotIdx;
                }
            }
        }

        if (!slot) {
            int retries = 0;
            uint32_t idx = nextSlot.fetch_add(1, std::memory_order_relaxed) % numSlots;
#ifdef SHM_DEBUG
            // Nested-IPC deadlock detector (debug only). The README's
            // "Nested IPC & Recursion" section warns that an inner call
            // started while all slots are held by outer calls will spin
            // forever waiting for a slot that will never be released.
            // We count full sweeps with zero progress; after the
            // threshold we emit a one-shot diagnostic so the caller can
            // see the deadlock instead of a silent hang. We do NOT
            // abort — production code without SHM_DEBUG keeps the old
            // spin-forever semantics, so behavior is identical when
            // compiled out.
            int fullSweeps = 0;
            bool diagnosticEmitted = false;
            constexpr int kDeadlockSweepThreshold = 10000; // ~10k full sweeps
#endif

            while (true) {
                Slot& s = slots[idx];
                uint32_t current = s.header->state.load(std::memory_order_acquire);
                bool canClaim = (current == SLOT_FREE);
                if (!canClaim) {
                     if (current == SLOT_RESP_READY || current == SLOT_REQ_READY || current == SLOT_GUEST_BUSY) {
                          if (!s.activeWait.load(std::memory_order_acquire)) {
                              canClaim = true;
                          }
                     }
                }

                if (canClaim) {
                    // §3.6.1: bump generation before the claiming CAS.
                    s.header->gen.fetch_add(1, std::memory_order_acq_rel);
                    if (s.header->state.compare_exchange_strong(current, SLOT_BUSY, std::memory_order_acquire)) {
                        s.header->lease.store(Platform::MonotonicNanos(), std::memory_order_release);
                        slot = &s;
                        cachedSlotIdx = idx;
                        resultIdx = (int32_t)idx;
                        break;
                    }
                }
                idx++;
                if (idx >= numSlots) {
                    idx = 0;
#ifdef SHM_DEBUG
                    if (++fullSweeps == kDeadlockSweepThreshold && !diagnosticEmitted) {
                        SHM_LOG_WARN(
                            "DirectHost::AcquireSlot: ", kDeadlockSweepThreshold,
                            " full sweeps with no free slot — potential "
                            "nested-IPC deadlock. Per README 'Nested IPC & "
                            "Recursion', numHostSlots must be >= "
                            "N_threads * (Depth + 1).");
                        diagnosticEmitted = true;
                    }
#endif
                    // v0.7.2: auto-reclaim. After a full sweep with no
                    // free slot, scan every non-FREE slot once and try
                    // to reclaim any whose lease is stale by
                    // `autoReclaimTimeoutNs`. Zero (default) disables
                    // this entirely. CAS guard inside
                    // TryReclaimAbandonedSlot keeps the operation safe
                    // against concurrent live heartbeats.
                    const uint64_t reclaimThresh =
                        autoReclaimTimeoutNs.load(std::memory_order_relaxed);
                    if (reclaimThresh > 0) {
                        for (uint32_t i = 0; i < numSlots; ++i) {
                            if (TryReclaimAbandonedSlot(
                                    static_cast<int32_t>(i), reclaimThresh)) {
                                SHM_LOG_WARN(
                                    "DirectHost::AcquireSlot: reclaimed "
                                    "abandoned slot ", i,
                                    " (lease older than ", reclaimThresh,
                                    " ns)");
                            }
                        }
                    }
                }
                retries++;
                if (retries > (int)numSlots * 100) {
                    Platform::ThreadYield();
                    retries = 0;
                }
            }
        }
        return resultIdx;
    }

    /**
     * @brief Acquires a specific slot.
     * @param slotIdx The index of the slot to acquire.
     * @param timeoutMs Timeout in milliseconds. Default USE_DEFAULT_TIMEOUT.
     * @return The slot index (same as input), or -1 if failed.
     */
    int32_t AcquireSpecificSlot(int32_t slotIdx, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        if (!*running || slotIdx < 0 || slotIdx >= (int32_t)numSlots) return -1;
        Slot* slot = &slots[slotIdx];

        uint32_t effectiveTimeout = (timeoutMs == USE_DEFAULT_TIMEOUT) ? responseTimeoutMs : timeoutMs;
        auto start = std::chrono::steady_clock::now();
        int retries = 0;

        while(true) {
             uint32_t current = slot->header->state.load(std::memory_order_acquire);
             bool canClaim = (current == SLOT_FREE);
             if (!canClaim && (current == SLOT_RESP_READY || current == SLOT_REQ_READY)) {
                  if (!slot->activeWait.load(std::memory_order_acquire)) {
                      canClaim = true;
                  }
             }

             if (canClaim) {
                 // §3.6.1: bump generation before the claiming CAS.
                 slot->header->gen.fetch_add(1, std::memory_order_acq_rel);
                 if (slot->header->state.compare_exchange_strong(current, SLOT_BUSY, std::memory_order_acquire)) {
                     slot->header->lease.store(Platform::MonotonicNanos(), std::memory_order_release);
                     break;
                 }
             }

             Platform::CpuRelax();
             retries++;
             if (retries > 1000) {
                 if (effectiveTimeout != 0xFFFFFFFF) {
                     auto now = std::chrono::steady_clock::now();
                     auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
                     if (elapsed >= effectiveTimeout) {
                         return -1;
                     }
                 }

                 Platform::ThreadYield();
                 retries = 0;
             }
        }
        return slotIdx;
    }

    /**
     * @brief Enable AcquireSlot auto-reclaim with the given lease-age threshold.
     *
     * When AcquireSlot's slow-path slot scan completes a full sweep
     * without finding a free slot, it walks every non-FREE slot and
     * calls `TryReclaimAbandonedSlot(idx, timeoutNs)`. Zero disables
     * auto-reclaim entirely (the default — opt-in for safety).
     *
     * Typical values: 5× `responseTimeoutMs` converted to ns. Too short
     * and you race a slow-but-live peer (CAS guard catches it, but you
     * burn cycles); too long and you wait forever when a peer truly
     * crashed.
     *
     * Safe to call at any time; thread-safe atomic store.
     *
     * @param timeoutNs Threshold in nanoseconds. 0 = disabled.
     */
    void SetAutoReclaimTimeoutNs(uint64_t timeoutNs) {
        autoReclaimTimeoutNs.store(timeoutNs, std::memory_order_relaxed);
    }

    /**
     * @brief Returns the current auto-reclaim threshold (0 = disabled).
     */
    uint64_t GetAutoReclaimTimeoutNs() const {
        return autoReclaimTimeoutNs.load(std::memory_order_relaxed);
    }

    /**
     * @brief Attempt to reclaim a slot whose lease is older than `maxLeaseAgeNs`.
     *
     * For crash-recovery: if the slot's current owner has crashed, its
     * lease will not refresh and the slot will sit in a non-FREE state
     * forever. This method observes the current state and lease, and if
     * `now - lease > maxLeaseAgeNs` it tries to CAS the state back to
     * SLOT_FREE.
     *
     * Safety / ABA hazard (v0.7.5): a bare `state` CAS is NOT sufficient.
     * Between the lease read and the CAS a peer can legitimately finish the
     * slot (state -> FREE), have it reused, and re-claim it so `state` lands
     * back on the SAME observed value with a FRESH lease. A bare
     * CAS(state, observed, FREE) would then wrongly reclaim a live slot.
     * Worse, the fresh lease is published only AFTER the re-claiming CAS, so
     * even re-reading `lease` before the CAS cannot close the window (the
     * "lease-publication-lag").
     *
     * We close it with the claim-generation handshake (§3.6.1): every claim
     * path bumps `gen` BEFORE its state-claiming CAS. The reclaimer snapshots
     * `gen`, verifies staleness, then wins the reclaim by
     * `compare_exchange(gen, gen+1)`. Because a claim's `gen` bump precedes
     * any `state` change, the reclaim CAS fails whenever a claim has begun —
     * including the lease-lag window. The final `state` CAS (from the
     * observed value) then arbitrates a same-generation reclaim against a
     * SLOT_RESP_READY zombie re-claim.
     *
     * @param slotIdx The slot to inspect.
     * @param maxLeaseAgeNs Maximum tolerated age in nanoseconds. Slots
     *                     whose lease is older than this are candidates
     *                     for reclamation.
     * @return true if the slot was reclaimed. false if (a) the slot was
     *         already FREE, (b) lease was zero (peer never heartbeated),
     *         (c) lease was fresher than the threshold, (d) the lease
     *         changed before the handshake (peer heartbeated/re-claimed),
     *         (e) the generation CAS lost (a claim began), or (f) the final
     *         state CAS lost a race against a concurrent claim.
     */
    bool TryReclaimAbandonedSlot(int32_t slotIdx, uint64_t maxLeaseAgeNs) {
        if (slotIdx < 0 || slotIdx >= (int32_t)(numSlots + numGuestSlots)) {
            return false;
        }
        Slot* slot = &slots[slotIdx];

        // Snapshot the claim generation FIRST. Every claim bumps gen before
        // transitioning state (§3.6.1), so any claim that begins after this
        // load — even one whose lease store has not yet landed — is caught
        // by the generation CAS below.
        uint64_t gen = slot->header->gen.load(std::memory_order_acquire);

        uint32_t state = slot->header->state.load(std::memory_order_acquire);
        if (state == SLOT_FREE) {
            return false;
        }
        uint64_t lease = slot->header->lease.load(std::memory_order_acquire);
        if (lease == 0) {
            // Peer never wrote a lease — either v0.6.x peer or a slot
            // that has somehow skipped the heartbeat. Refuse to reclaim:
            // we cannot tell stale from never-stamped.
            return false;
        }
        uint64_t now = Platform::MonotonicNanos();
        if (now <= lease || (now - lease) <= maxLeaseAgeNs) {
            return false;
        }
        // Cheap common-case rejection: a refreshed lease means a live owner
        // heartbeated/re-claimed and already published its lease.
        if (slot->header->lease.load(std::memory_order_acquire) != lease) {
            return false;
        }
        // Generation handshake: win the exclusive right to reclaim. Fails if
        // any claim bumped gen since our snapshot (including the lease-lag
        // window, where state is re-claimed but the fresh lease is pending).
        if (!slot->header->gen.compare_exchange_strong(
                gen, gen + 1, std::memory_order_acq_rel)) {
            return false;
        }
        // We won the generation. Publish SLOT_FREE via CAS from the observed
        // state (not a blind store): a claim that bumped gen AFTER our
        // winning CAS — e.g. reclaiming a SLOT_RESP_READY zombie — will have
        // advanced state via its own claiming CAS, and this CAS arbitrates
        // the final race. A burned gen tick is harmless (gen only advances).
        return slot->header->state.compare_exchange_strong(
            state, SLOT_FREE, std::memory_order_acq_rel);
    }

    /**
     * @brief Gets the request buffer pointer for an acquired slot.
     * @param slotIdx The slot index.
     * @return Pointer to the buffer, or nullptr if invalid.
     */
    uint8_t* GetReqBuffer(int32_t slotIdx) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return nullptr;
        return slots[slotIdx].reqBuffer;
    }

    /**
     * @brief Gets the max request size for a slot.
     * @param slotIdx The slot index.
     * @return Max size in bytes.
     */
    int32_t GetMaxReqSize(int32_t slotIdx) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return 0;
        return (int32_t)slots[slotIdx].maxReqSize;
    }

    /**
     * @brief Manually signals the Request Event for a slot.
     * Used for custom protocols like RingBuffer.
     */
    void SignalSlot(int32_t slotIdx) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return;
        Platform::SignalEvent(slots[slotIdx].hReqEvent);
    }

    /**
     * @brief Manually waits on the Response Event for a slot.
     * Used for custom protocols like RingBuffer.
     * @param slotIdx The slot index.
     * @param timeoutMs Timeout in milliseconds.
     * @return true if signaled, false if timeout.
     */
    bool WaitForSlotEvent(int32_t slotIdx, uint32_t timeoutMs = 0xFFFFFFFF) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return false;
        return Platform::WaitEvent(slots[slotIdx].hRespEvent, timeoutMs);
    }

    /**
     * @brief Sends a request asynchronously using an acquired slot.
     * Does not wait for response. The slot remains BUSY until WaitForSlot is called.
     * @return Result<void> Success or Error.
     */
    Result<void> SendAcquiredAsync(int32_t slotIdx, int32_t size, MsgType msgType) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return Result<void>::Failure(Error::InvalidArgs);
        Slot* slot = &slots[slotIdx];

        uint32_t absSize = (size < 0) ? (0u - (uint32_t)size) : (uint32_t)size;
        if (absSize > slot->maxReqSize) {
             slot->header->state.store(SLOT_FREE, std::memory_order_release);
             return Result<void>::Failure(Error::BufferTooSmall);
        }

        slot->header->reqSize = size;
        slot->header->msgType = msgType;
        slot->header->msgSeq = slot->msgSeq;
        slot->msgSeq += msgSeqStride;

        // Mark activeWait to prevent theft during async operation
        slot->activeWait.store(true, std::memory_order_relaxed);

        slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
        slot->header->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

        if (slot->header->guestState.load(std::memory_order_seq_cst) == GUEST_STATE_WAITING) {
            Platform::SignalEvent(slot->hReqEvent);
        }

        return Result<void>::Success();
    }

    /**
     * @brief Waits for a specific slot to receive a response.
     * Completes the transaction started by SendAcquiredAsync.
     * @return Result<int> Response size.
     */
    Result<int> WaitForSlot(int32_t slotIdx, std::vector<uint8_t>& outResp, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return Result<int>(Error::InvalidArgs);
        Slot* slot = &slots[slotIdx];

        uint32_t t = (timeoutMs == USE_DEFAULT_TIMEOUT) ? responseTimeoutMs : timeoutMs;

        // The request was already published (SLOT_REQ_READY) and the guest
        // signaled by SendAcquiredAsync; re-affirm the host-side wait flags and
        // run the shared adaptive wait. Do NOT re-publish SLOT_REQ_READY here.
        slot->activeWait.store(true, std::memory_order_relaxed);
        slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);

        bool ready = WaitForRespReady(slot, t);

        slot->activeWait.store(false, std::memory_order_release);

        if (!ready) {
             return Result<int>(Error::Timeout);
        }

        uint32_t expectedSeq = slot->msgSeq - msgSeqStride;
        if (slot->header->msgSeq != expectedSeq) {
             // Release the slot before reporting the violation; otherwise the
             // slot stays in SLOT_RESP_READY indefinitely (the happy-path
             // SLOT_FREE store below is only reached on success).
             slot->header->state.store(SLOT_FREE, std::memory_order_release);
             return Result<int>(Error::ProtocolViolation);
        }

        if (slot->header->msgType == MsgType::SYSTEM_ERROR) {
             slot->header->state.store(SLOT_FREE, std::memory_order_release);
             return Result<int>(Error::InternalError);
        }

        int resultSize = 0;
        int32_t respSize = slot->header->respSize;
        uint32_t absResp = (respSize < 0) ? (0u - (uint32_t)respSize) : (uint32_t)respSize;

        if (absResp > slot->maxRespSize) absResp = slot->maxRespSize;

        outResp.resize(absResp);
        if (absResp > 0) {
            if (respSize >= 0) {
                memcpy(outResp.data(), slot->respBuffer, absResp);
            } else {
                uint32_t offset = slot->maxRespSize - absResp;
                memcpy(outResp.data(), slot->respBuffer + offset, absResp);
            }
        }
        resultSize = (int)absResp;

        slot->header->state.store(SLOT_FREE, std::memory_order_release);
        return Result<int>(resultSize);
    }

    /**
     * @brief Sends a request using an acquired slot (Zero-Copy flow).
     * @return Result<int> Bytes read (response size) on success, or Error on failure.
     */
    Result<int> SendAcquired(int32_t slotIdx, int32_t size, MsgType msgType, std::vector<uint8_t>& outResp, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return Result<int>(Error::InvalidArgs);
        Slot* slot = &slots[slotIdx];

        uint32_t absSize = (size < 0) ? (0u - (uint32_t)size) : (uint32_t)size;
        if (absSize > slot->maxReqSize) {
             slot->header->state.store(SLOT_FREE, std::memory_order_release);
             return Result<int>(Error::BufferTooSmall);
        }

        slot->header->reqSize = size;
        slot->header->msgType = msgType;
        uint32_t currentSeq = slot->msgSeq;
        slot->header->msgSeq = currentSeq;
        slot->msgSeq += msgSeqStride;

        uint32_t t = (timeoutMs == USE_DEFAULT_TIMEOUT) ? responseTimeoutMs : timeoutMs;
        bool ready = WaitResponse(slot, t);

        if (!ready) {
             return Result<int>(Error::Timeout);
        }

        if (slot->header->msgSeq != currentSeq) {
             // Release the slot before reporting the violation; otherwise the
             // slot stays in SLOT_RESP_READY indefinitely (the happy-path
             // SLOT_FREE store below is only reached on success).
             slot->header->state.store(SLOT_FREE, std::memory_order_release);
             return Result<int>(Error::ProtocolViolation);
        }

        if (slot->header->msgType == MsgType::SYSTEM_ERROR) {
             slot->header->state.store(SLOT_FREE, std::memory_order_release);
             return Result<int>(Error::InternalError);
        }

        int resultSize = 0;
        if (ready) {
            int32_t respSize = slot->header->respSize;
            uint32_t absResp = (respSize < 0) ? (0u - (uint32_t)respSize) : (uint32_t)respSize;

            if (absResp > slot->maxRespSize) absResp = slot->maxRespSize;

            outResp.resize(absResp);
            if (absResp > 0) {
                if (respSize >= 0) {
                    memcpy(outResp.data(), slot->respBuffer, absResp);
                } else {
                    uint32_t offset = slot->maxRespSize - absResp;
                    memcpy(outResp.data(), slot->respBuffer + offset, absResp);
                }
            }
            resultSize = (int)absResp;
        }

        slot->header->state.store(SLOT_FREE, std::memory_order_release);

        return Result<int>(resultSize);
    }
};

} // namespace shm
