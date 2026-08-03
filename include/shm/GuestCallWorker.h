#pragma once
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include "Platform.h"
#include "IPCUtils.h"
#include "SlotAllocator.h"
#include "WaitStrategy.h"

namespace shm {

/**
 * @class GuestCallWorker
 * @brief Owns the Guest-call processing path (R44 refactor — extracted from
 *        DirectHost).
 *
 * Holds the background worker thread and its running flag, the guest-call
 * batch processor, and the handler type. It observes the slot pool through a
 * pointer to the DirectHost-owned SlotAllocator and the lifetime gate through
 * a pointer to DirectHost::running, so there is no duplicated state and no
 * behavioral drift.
 *
 * Pure mechanical, behavior-preserving extraction: the §3.6.1 generation
 * handshake, all memory_orders, and the SYSTEM_ERROR / truncation handling
 * are carried over verbatim.
 */
class GuestCallWorker {
public:
    using GuestCallHandler = std::function<int32_t(const uint8_t*, int32_t, uint8_t*, uint32_t, MsgType)>;

    // Pointers into the DirectHost façade's state. Wired by DirectHost.
    SlotAllocator* alloc = nullptr;
    const bool* running = nullptr;

    // v0.8.6: when true (default), the worker runs an adaptive spin phase over
    // the guest-slot request predicate before parking on the shared request
    // event, and publishes HOST_STATE_WAITING on every guest slot before it
    // parks so the Go sender can elide its request-doorbell SetEvent while the
    // worker is hot (the guest→host analog of the response-side signal gate).
    // Set false via HostConfig::guestWorkerSpin on hosts that cannot spare a
    // briefly-spinning background thread; the sleep-only legacy loop is then
    // used and the sender always signals. The adaptive limit self-throttles to
    // kMinSpin when idle, so the steady-state idle cost is a short spin then a
    // park, not a busy-loop.
    bool spin = true;
    WaitStrategy waitStrategy;

    // v0.8.9: optional CPU affinity mask for the worker thread (0 = no pin).
    // The guest-call path historically ran both endpoints unpinned — the
    // Direct-Exchange sibling co-location (host worker N + Go worker N on one
    // physical core's two SMT LPs) never applied to it, so every guest call
    // paid cross-core cache-line transfers. Pinning the worker to one LP of a
    // free SMT pair (and the Go sender to the other, see Go
    // PinCurrentGoroutine) restores the same locality. Wired from
    // HostConfig::guestWorkerAffinity.
    uint64_t pinMask = 0;

    std::thread guestWorker;
    std::atomic<bool> guestWorkerRunning{false};

    // Defensive: join the worker on destruction so the thread can never outlive
    // its owner even if used outside the DirectHost façade or if a future path
    // destroys the worker without going through Shutdown()->Stop(). A joinable
    // std::thread destroyed without join() calls std::terminate(). The normal
    // lifecycle still joins via DirectHost::Shutdown(); this only backstops it.
    ~GuestCallWorker() { Stop(); }

    // Poll cadence for the two fallback paths that have no event to park on
    // (no guest slots at all, or no shared request event). It is deliberately
    // NOT the caller's timeout: those paths cannot be woken, so the timeout is
    // a POLL INTERVAL there, and honouring a 1 s park would make a request wait
    // up to 1 s and shutdown up to 1 s -- a 10x regression against the 100 ms
    // the pre-extraction loop used. A caller asking for LESS than this still
    // gets what it asked for.
    static constexpr uint32_t kFallbackPollMs = 100;

    /**
     * @brief The shared request event every guest slot signals on, or nullptr.
     *
     * All guest slots share one event (shmName + "_guest_call"), so the first
     * one answers for all. The size conjunct is the confirmed-correct guard from
     * GuestWorkerLoop -- reused, deliberately not re-derived.
     */
    EventHandle GuestRequestEvent() const {
        if (alloc && alloc->numGuestSlots > 0 &&
            alloc->numSlots + alloc->numGuestSlots <= alloc->slots.size()) {
            return alloc->slots[alloc->numSlots].hReqEvent;
        }
        return nullptr;
    }

    /**
     * @brief Blocks until a guest request looks ready, timeoutMs elapses, or
     *        WakeWaiter() is called. Returns the advisory readiness observed.
     *
     * THIS IS THE ONLY IMPLEMENTATION OF THE SPECIFICATION.md 3.5 DOORBELL GATE.
     * GuestWorkerLoop calls it rather than carrying its own copy: a second copy
     * of a Dekker is how a lost wakeup ships. If you are tempted to inline a
     * variant for a new caller, add a parameter here instead.
     *
     * The gate, and why each memory order is what it is: we publish
     * HOST_STATE_WAITING on every guest slot (seq_cst) so a concurrently-sending
     * guest may elide its doorbell SetEvent, then RECHECK the request predicate
     * once (seq_cst) before parking. The sender does
     * store REQ_READY(seq_cst) -> load hostState(seq_cst); we do
     * store hostState=WAITING(seq_cst) -> load state(seq_cst). The single seq_cst
     * total order forbids both loads from missing, so there is no lost wakeup.
     * On wake we restore ACTIVE so the caller's processing phase runs
     * doorbell-free.
     *
     * The return value is ADVISORY. The caller must still call ProcessGuestCalls,
     * which re-checks each slot under the ownership CAS -- a `true` here only
     * means "do not go back to sleep yet".
     *
     * FOREIGN CALLERS (a host that runs its own loop instead of Start()): this is
     * safe to call from any single thread, and it is the ONLY way such a host gets
     * the doorbell at all. Until it does, hostState on the guest slots stays
     * HOST_STATE_ACTIVE forever, the Go sender's `if hostState == WAITING` gate
     * never fires, and every request arrives with no signal behind it -- so a
     * naive wait in a foreign loop expires on its own timeout every single call.
     * Call this, do not roll your own.
     */
    bool WaitForRequest(uint32_t timeoutMs) {
        if (!alloc || alloc->numGuestSlots == 0) {
            // No guest slots exist, so no request can ever arrive. Sleep rather
            // than returning instantly, so a foreign loop does not turn into the
            // busy-wait this function exists to end -- but at the POLL cadence,
            // not the caller's full park (see kFallbackPollMs).
            if (timeoutMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(
                    timeoutMs < kFallbackPollMs ? timeoutMs : kFallbackPollMs));
            }
            return false;
        }

        const uint32_t base = alloc->numSlots;
        const uint32_t count = alloc->numGuestSlots;
        EventHandle waitEvent = GuestRequestEvent();

        auto anyRequestReady = [&]() -> bool {
            for (uint32_t i = base; i < base + count; ++i) {
                if (alloc->slots[i].header->state.load(std::memory_order_seq_cst) == SLOT_REQ_READY) {
                    return true;
                }
            }
            return false;
        };

        auto setHostState = [&](uint32_t v, std::memory_order order) {
            for (uint32_t i = base; i < base + count; ++i) {
                alloc->slots[i].header->hostState.store(v, order);
            }
        };

        if (!waitEvent) {
            // No shared request event: there is nothing to park on and nothing
            // for a sender to signal, so the WAITING/ACTIVE contract would be a
            // lie. Poll instead.
            std::this_thread::sleep_for(std::chrono::milliseconds(
                (timeoutMs > 0 && timeoutMs < kFallbackPollMs) ? timeoutMs : kFallbackPollMs));
            return anyRequestReady();
        }

        auto sleepAction = [&]() {
            setHostState(HOST_STATE_WAITING, std::memory_order_seq_cst);
            if (anyRequestReady()) {
                setHostState(HOST_STATE_ACTIVE, std::memory_order_relaxed);
                return;
            }
            Platform::WaitEvent(waitEvent, timeoutMs);
            setHostState(HOST_STATE_ACTIVE, std::memory_order_relaxed);
        };

        if (!spin) {
            // Sleep-only: keep the WAITING/ACTIVE contract around the park so the
            // sender's doorbell gate stays correct; only the spin phase is
            // dropped, so the sender always signals a parked waiter and the loss
            // is the spin-catch fast path, never a wakeup.
            sleepAction();
            return anyRequestReady();
        }

        waitStrategy.Wait(anyRequestReady, sleepAction);
        return anyRequestReady();
    }

    /**
     * @brief Releases a thread parked in WaitForRequest(), for shutdown.
     *
     * Signals the shared guest request event. A parked waiter wakes, restores
     * HOST_STATE_ACTIVE and returns; its caller then observes whatever stop flag
     * it owns. Safe to call when nothing is parked (the event is auto-reset) and
     * safe when there are no guest slots (no-op).
     *
     * A foreign-loop host needs this: without it, stopping the loop waits out the
     * full park quantum, and a host whose teardown budget is shorter than that
     * quantum would DETACH a thread that is still executing inside its image.
     */
    void WakeWaiter() {
        if (EventHandle ev = GuestRequestEvent()) {
            Platform::SignalEvent(ev);
        }
    }

    void GuestWorkerLoop(GuestCallHandler handler, int32_t maxBatchSize) {
        if (pinMask != 0) {
            Platform::PinCurrentThreadAffinity(pinMask);
        }
        // The wait/doorbell gate lives in WaitForRequest so there is exactly one
        // copy of it (see that function). The 1 s cap bounds shutdown latency:
        // Stop() flips guestWorkerRunning and signals the event, so the park is
        // normally cut short rather than waited out.
        while (guestWorkerRunning.load(std::memory_order_relaxed)) {
            WaitForRequest(1000);
            ProcessGuestCalls(handler, maxBatchSize);
        }
    }

    /**
     * @brief Starts the background worker for Guest Calls.
     *
     * @param handler The handler to process requests.
     * @param maxBatchSize Max requests to process in one burst (default 100).
     */
    void Start(GuestCallHandler handler, int32_t maxBatchSize = 100) {
        if (alloc->numGuestSlots == 0) return;
        if (guestWorkerRunning.exchange(true)) return;
        guestWorker = std::thread(&GuestCallWorker::GuestWorkerLoop, this, handler, maxBatchSize);
    }

    /**
     * @brief Stops the background worker.
     */
    void Stop() {
        if (guestWorkerRunning.exchange(false)) {
            // Pop the worker out of its park instead of waiting the quantum
            // out. Without this the join below blocks for up to the full
            // WaitForRequest timeout (1 s from GuestWorkerLoop) on every
            // shutdown of an idle host -- the flag flip alone is invisible to
            // a thread blocked in WaitForSingleObject. The event is auto-reset
            // and the flag is already false, so a spurious wake just re-tests
            // the loop condition and exits.
            WakeWaiter();
            if (guestWorker.joinable()) {
                guestWorker.join();
            }
        }
    }

    /**
     * @brief Phase 1 of the two-phase responder publish: converts this worker's
     *        SLOT_BUSY claim on a guest slot into an exclusive publish lock
     *        (SLOT_BUSY -> SLOT_DONE, SPEC §3.5 step 5).
     *
     * Callers MUST NOT write a single header field — not respSize, not msgType —
     * until this returns true. Mirror of the Go responder's lockForPublish
     * (go/direct.go); the only difference is the owned state (SLOT_BUSY here,
     * SLOT_GUEST_BUSY on host slots).
     *
     * The transition is a CAS, never a blind store. Ownership of a claimed guest
     * slot is not eternal: a slow handler can outlive the Guest sender's
     * response timeout, after which the Guest's opt-in lease reclaimer
     * (TryReclaimAbandonedSlot) can free the slot and a fresh sender can claim
     * and republish it under a NEW transaction. A blind store would then stamp
     * RESP_READY over that transaction while respBuffer still holds the old
     * one's bytes; because the responder never rewrites msgSeq, the sender's
     * msgSeq guard validates and silently accepts the wrong value. CAS from the
     * owned SLOT_BUSY makes the publishing side airtight: a recycled slot is at
     * SLOT_FREE / SLOT_GUEST_BUSY / SLOT_REQ_READY when the late publish
     * arrives, so the CAS fails and the response is dropped instead.
     *
     * Why the lock is a *separate* transition (post-v0.8.16). v0.8.14 sealed only the
     * publishing transition and left the respSize/msgType stores that precede it
     * unconditional, so after a reclaim+republish those stores landed on the new
     * owner's transaction: the publish CAS then failed and the response bytes
     * were correctly dropped, but the header was already poisoned and the next
     * ProcessGuestCalls dispatch read the wrong msgType (a silent wrong value
     * the msgSeq guard cannot catch). Proving ownership before the first header
     * write is the only fix.
     *
     * SLOT_DONE (3) is the lock state because it is outside the zombie-steal set
     * {SLOT_REQ_READY, SLOT_RESP_READY, SLOT_GUEST_BUSY} used by
     * SlotAllocator::tryClaimSlot and the Go tryClaimGuestSlot, so a slot parked
     * there cannot be stolen mid-publish, and because no other protocol
     * transition produces or consumes it. The window is two atomic ops wide and
     * is wire/ABI-neutral: the peer only ever waits on SLOT_RESP_READY.
     *
     * @return true if the slot is locked for publishing (header may be written).
     */
    static bool LockForPublish(Slot* slot, uint32_t slotIdx) {
        uint32_t expected = SLOT_BUSY;
        if (!slot->header->state.compare_exchange_strong(
                expected, SLOT_DONE, std::memory_order_acq_rel)) {
            SHM_LOG_WARN("Guest-call response dropped: slot ", slotIdx,
                         " was reclaimed while the handler ran (state=", expected, ")");
            return false;
        }
        return true;
    }

    /**
     * @brief Phase 2 of the responder publish: SLOT_DONE -> SLOT_RESP_READY.
     *
     * Only the holder of the publish lock can be at SLOT_DONE, so this CAS
     * cannot legitimately fail; a failure means an invariant was violated
     * (someone wrote `state` while we held the lock) and is logged as an error
     * rather than silently swallowed.
     *
     * The CAS stays seq_cst because the §4.2 doorbell Dekker depends on it.
     * Signal elision (two-sided Dekker, same pattern as SlotAllocator's
     * REQ_READY publish): the guest caller stores
     * guestState=GUEST_STATE_WAITING with seq_cst and re-checks `state` before
     * parking (go/direct.go), so a seq_cst RESP_READY publish followed by a
     * seq_cst guestState load can never both miss — either the guest sees
     * RESP_READY and skips the park, or we see WAITING and deliver the
     * (kernel-priced) SetEvent. A spinning guest costs no syscall at all.
     *
     * @return true if the response was published.
     */
    static bool PublishResponse(Slot* slot, uint32_t slotIdx) {
        uint32_t expected = SLOT_DONE;
        if (!slot->header->state.compare_exchange_strong(
                expected, SLOT_RESP_READY, std::memory_order_seq_cst)) {
            SHM_LOG_ERROR("Guest-call publish lock lost on slot ", slotIdx,
                          " between SLOT_DONE and the RESP_READY publish (state=",
                          expected, ") — protocol invariant violated");
            return false;
        }
        if (slot->header->guestState.load(std::memory_order_seq_cst) == GUEST_STATE_WAITING) {
            Platform::SignalEvent(slot->hRespEvent);
        }
        return true;
    }

    /**
     * @brief Processes any pending Guest Calls (Guest -> Host).
     * @return int Number of requests processed.
     */
    template <typename Handler>
    int ProcessGuestCalls(Handler&& handler, int limit = -1) {
        if (!*running) return 0;
        int processed = 0;

        // v0.8.9 no-reclaim fast claim: the gen bump and lease refresh on this
        // claim exist solely for the reclaim machinery — the gen bump fences
        // §3.6.1 ABA for reclaimers (guest-sender transaction cycles are
        // already fenced by the SENDER's own bump on every claim), and the
        // lease heartbeat extends the sender's claim-time stamp for lease-
        // based reclaimers. With host-side auto-reclaim off (the default),
        // skip both. The claiming CAS itself is KEPT: the public
        // ProcessGuestCalls API permits concurrent callers, and the CAS is
        // what arbitrates two processors racing for one request. A Go-side
        // reclaimer (guest's own opt-in) is covered by the existing lease
        // contract — the sender's claim stamp is fresh within any threshold
        // that exceeds the max call duration, which the contract requires.
        const bool fastClaim =
            alloc->autoReclaimTimeoutNs.load(std::memory_order_relaxed) == 0;

        const uint32_t numSlots = alloc->numSlots;
        const uint32_t numGuestSlots = alloc->numGuestSlots;
        for (uint32_t i = numSlots; i < numSlots + numGuestSlots; ++i) {
            if (limit > 0 && processed >= limit) break;
            Slot* slot = &alloc->slots[i];

            uint32_t current = slot->header->state.load(std::memory_order_acquire);
            if (current != SLOT_REQ_READY) continue;

            // §3.6.1: bump generation before the claiming CAS (skipped when
            // fastClaim — see above).
            if (!fastClaim) {
                slot->header->gen.fetch_add(1, std::memory_order_acq_rel);
            }
            if (slot->header->state.compare_exchange_strong(current, SLOT_BUSY, std::memory_order_acq_rel)) {
                if (!fastClaim) {
                    slot->header->lease.store(Platform::MonotonicNanos(), std::memory_order_release);
                }
                int32_t reqSize = slot->header->reqSize;
                const uint8_t* reqData = nullptr;
                uint32_t absReqSize = (reqSize < 0) ? (0u - (uint32_t)reqSize) : (uint32_t)reqSize;

                // Every response-producing branch below follows the same
                // two-phase order (§3.5 responder publish rule): compute the
                // response metadata into LOCALS, take the publish lock, and only
                // then write header fields. A branch whose LockForPublish fails
                // writes nothing — the slot now carries somebody else's
                // transaction.
                if (absReqSize > slot->maxReqSize) {
                    if (LockForPublish(slot, i)) {
                        slot->header->respSize = 0;
                        slot->header->msgType = MsgType::SYSTEM_ERROR;
                        PublishResponse(slot, i);
                    }
                    processed++;
                    continue;
                }

                if (reqSize >= 0) {
                     reqData = slot->reqBuffer;
                } else {
                     uint32_t offset = slot->maxReqSize - absReqSize;
                     reqData = slot->reqBuffer + offset;
                }

                int32_t respSize = handler(reqData, (int32_t)absReqSize, slot->respBuffer, slot->maxRespSize, slot->header->msgType);

                uint32_t absRespSize = (respSize < 0) ? (0u - (uint32_t)respSize) : (uint32_t)respSize;
                const bool respOverflow = absRespSize > slot->maxRespSize;
                if (respOverflow) {
                    respSize = 0;
                }

                if (LockForPublish(slot, i)) {
                    if (respOverflow) {
                        slot->header->msgType = MsgType::SYSTEM_ERROR;
                    }
                    slot->header->respSize = respSize;
                    PublishResponse(slot, i);
                }

                processed++;
            }
        }
        return processed;
    }
};

} // namespace shm
