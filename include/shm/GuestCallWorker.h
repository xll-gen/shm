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

    std::thread guestWorker;
    std::atomic<bool> guestWorkerRunning{false};

    // Defensive: join the worker on destruction so the thread can never outlive
    // its owner even if used outside the DirectHost façade or if a future path
    // destroys the worker without going through Shutdown()->Stop(). A joinable
    // std::thread destroyed without join() calls std::terminate(). The normal
    // lifecycle still joins via DirectHost::Shutdown(); this only backstops it.
    ~GuestCallWorker() { Stop(); }

    void GuestWorkerLoop(GuestCallHandler handler, int32_t maxBatchSize) {
        EventHandle waitEvent = nullptr;
        // Since all guest slots share the same reqEvent (shmName + "_guest_call"),
        // we can wait on the first one.
        if (alloc->numGuestSlots > 0 && alloc->numSlots + alloc->numGuestSlots <= alloc->slots.size()) {
             waitEvent = alloc->slots[alloc->numSlots].hReqEvent;
        }

        const uint32_t base = alloc->numSlots;
        const uint32_t count = alloc->numGuestSlots;

        // Any guest slot with a published request. This is the spin predicate
        // and the pre-park recheck; the seq_cst load pairs with the Go sender's
        // seq_cst REQ_READY store (Dekker, see sleepAction below).
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

        if (!spin || !waitEvent) {
            // Sleep-only loop (also the fallback when there is no shared request
            // event). Still maintains the HOST_STATE_WAITING/ACTIVE contract
            // around the park so the Go sender's doorbell gate stays correct
            // when spin is disabled — the only thing dropped here is the spin
            // phase, so the sender always signals a parked worker and the loss
            // is just the spin-catch fast path, never a wakeup.
            while (guestWorkerRunning.load(std::memory_order_relaxed)) {
                if (waitEvent) {
                    setHostState(HOST_STATE_WAITING, std::memory_order_seq_cst);
                    if (!anyRequestReady()) {
                        Platform::WaitEvent(waitEvent, 1000);
                    }
                    setHostState(HOST_STATE_ACTIVE, std::memory_order_relaxed);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                ProcessGuestCalls(handler, maxBatchSize);
            }
            return;
        }

        // Runs only when the spin window is exhausted. Publish WAITING on every
        // guest slot (seq_cst) so a concurrently-sending guest gates its
        // doorbell on it, then recheck once (Dekker: sender does
        // store REQ_READY(seq_cst) → load hostState(seq_cst); we do
        // store hostState=WAITING(seq_cst) → load state(seq_cst) — the seq_cst
        // total order forbids both loads from missing, so no lost wakeup). Only
        // park if the recheck is still empty. On wake, restore ACTIVE so the
        // spin/process phase runs doorbell-free.
        auto sleepAction = [&]() {
            setHostState(HOST_STATE_WAITING, std::memory_order_seq_cst);
            if (anyRequestReady()) {
                setHostState(HOST_STATE_ACTIVE, std::memory_order_relaxed);
                return;
            }
            // 1s cap bounds shutdown latency (Stop() flips guestWorkerRunning).
            Platform::WaitEvent(waitEvent, 1000);
            setHostState(HOST_STATE_ACTIVE, std::memory_order_relaxed);
        };

        while (guestWorkerRunning.load(std::memory_order_relaxed)) {
            waitStrategy.Wait(anyRequestReady, sleepAction);
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
            if (guestWorker.joinable()) {
                guestWorker.join();
            }
        }
    }

    /**
     * @brief Processes any pending Guest Calls (Guest -> Host).
     * @return int Number of requests processed.
     */
    template <typename Handler>
    int ProcessGuestCalls(Handler&& handler, int limit = -1) {
        if (!*running) return 0;
        int processed = 0;

        const uint32_t numSlots = alloc->numSlots;
        const uint32_t numGuestSlots = alloc->numGuestSlots;
        for (uint32_t i = numSlots; i < numSlots + numGuestSlots; ++i) {
            if (limit > 0 && processed >= limit) break;
            Slot* slot = &alloc->slots[i];

            uint32_t current = slot->header->state.load(std::memory_order_acquire);
            if (current != SLOT_REQ_READY) continue;

            // §3.6.1: bump generation before the claiming CAS.
            slot->header->gen.fetch_add(1, std::memory_order_acq_rel);
            if (slot->header->state.compare_exchange_strong(current, SLOT_BUSY, std::memory_order_acq_rel)) {
                slot->header->lease.store(Platform::MonotonicNanos(), std::memory_order_release);
                int32_t reqSize = slot->header->reqSize;
                const uint8_t* reqData = nullptr;
                uint32_t absReqSize = (reqSize < 0) ? (0u - (uint32_t)reqSize) : (uint32_t)reqSize;

                if (absReqSize > slot->maxReqSize) {
                    slot->header->respSize = 0;
                    slot->header->msgType = MsgType::SYSTEM_ERROR;
                    slot->header->state.store(SLOT_RESP_READY, std::memory_order_seq_cst);
                    if (slot->header->guestState.load(std::memory_order_seq_cst) == GUEST_STATE_WAITING) {
                        Platform::SignalEvent(slot->hRespEvent);
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
                if (absRespSize > slot->maxRespSize) {
                    respSize = 0;
                    slot->header->msgType = MsgType::SYSTEM_ERROR;
                }

                slot->header->respSize = respSize;

                // Signal elision (two-sided Dekker, same pattern as
                // SlotAllocator's REQ_READY publish): the guest caller
                // stores guestState=GUEST_STATE_WAITING with seq_cst and
                // re-checks `state` before parking (go/direct.go), so a
                // seq_cst RESP_READY publish followed by a seq_cst
                // guestState load can never both miss — either the guest
                // sees RESP_READY and skips the park, or we see WAITING and
                // deliver the (kernel-priced) SetEvent. A spinning guest
                // costs no syscall at all.
                slot->header->state.store(SLOT_RESP_READY, std::memory_order_seq_cst);
                if (slot->header->guestState.load(std::memory_order_seq_cst) == GUEST_STATE_WAITING) {
                    Platform::SignalEvent(slot->hRespEvent);
                }

                processed++;
            }
        }
        return processed;
    }
};

} // namespace shm
