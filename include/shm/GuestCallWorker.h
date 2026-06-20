#pragma once
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include "Platform.h"
#include "IPCUtils.h"
#include "SlotAllocator.h"

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

        while (guestWorkerRunning.load(std::memory_order_relaxed)) {
            // Wait for signal (timeout 1s to check for shutdown)
            if (waitEvent) {
                 Platform::WaitEvent(waitEvent, 1000);
            } else {
                 // Should not happen if Start checks numGuestSlots
                 std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            int processed = ProcessGuestCalls(handler, maxBatchSize);
            (void)processed;
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
                    Platform::SignalEvent(slot->hRespEvent);
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

                slot->header->state.store(SLOT_RESP_READY, std::memory_order_seq_cst);
                Platform::SignalEvent(slot->hRespEvent);

                processed++;
            }
        }
        return processed;
    }
};

} // namespace shm
