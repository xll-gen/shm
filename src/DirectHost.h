#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <thread>
#include <cstring>
#include <iostream>
#include <functional>
#include <condition_variable>
#include <atomic>
#include <mutex>
#include <chrono>
#include "Platform.h"
#include "IPCUtils.h"

namespace shm {

class DirectHost {
    void* shmBase;
    std::string shmName;
    uint32_t numSlots;
    ShmHandle hMapFile;
    std::atomic<bool> running;

    // Global Events
    EventHandle globalGuestEvent; // Host signals this to wake Guest
    EventHandle globalHostEvent;  // Guest signals this to wake Host

    struct SlotContext {
        SlotHeader* header;
        uint8_t* reqData;
        uint8_t* respData;

        // Synchronization for Blocking Request
        std::mutex mutex;
        std::condition_variable cv;
        bool responseReady;
        std::vector<uint8_t> responseBuffer;

        // Active Flag for Orphan Detection
        std::atomic<bool> requestActive;

        SlotContext() : requestActive(false), responseReady(false) {}
    };

    std::vector<std::unique_ptr<SlotContext>> slots;
    ExchangeHeader* exchangeHeader;

    // Scanner and Heartbeat
    std::thread scannerThread;
    std::thread heartbeatThread;
    std::atomic<uint64_t> lastGuestHeartbeat;
    std::atomic<bool> guestAlive;

    std::function<void(std::vector<uint8_t>&&, uint32_t)> onMessage;

    // Config
    uint32_t slotSize; // Data size for Req AND Resp (each)
    size_t perSlotTotal;

public:
    DirectHost() : shmBase(nullptr), hMapFile(0), running(false), exchangeHeader(nullptr) {}
    ~DirectHost() { Shutdown(); }

    bool Init(const std::string& shmName, uint32_t numQueues,
              std::function<void(std::vector<uint8_t>&&, uint32_t)> msgHandler) {
        this->shmName = shmName;
        this->onMessage = msgHandler;
        this->numSlots = numQueues;
        if (this->numSlots == 0) this->numSlots = 1;

        this->slotSize = 1024 * 1024; // 1MB
        // Layout: Header + Req + Resp
        // Req = 1MB, Resp = 1MB
        this->perSlotTotal = sizeof(SlotHeader) + (this->slotSize * 2);

        size_t headerSize = sizeof(ExchangeHeader);
        if (headerSize < 64) headerSize = 64;

        size_t totalSize = headerSize + (this->perSlotTotal * this->numSlots);

        bool exists = false;
        shmBase = Platform::CreateNamedShm(shmName.c_str(), totalSize, hMapFile, exists);
        if (!shmBase) return false;

        // Zero out memory if new
        if (!exists) {
            memset(shmBase, 0, totalSize);
        }

        // Setup Exchange Header
        exchangeHeader = (ExchangeHeader*)shmBase;
        if (!exists) {
            exchangeHeader->numSlots = this->numSlots;
            exchangeHeader->slotSize = this->slotSize;
            exchangeHeader->hostScannerState.store(SCANNER_STATE_ACTIVE);
            exchangeHeader->guestScannerState.store(SCANNER_STATE_ACTIVE);
            exchangeHeader->guestHeartbeat.store(0);
            exchangeHeader->hostHeartbeat.store(0);
        }

        // Global Events
        std::string gEvtName = shmName + "_event_guest";
        globalGuestEvent = Platform::CreateNamedEvent(gEvtName.c_str());

        std::string hEvtName = shmName + "_event_host";
        globalHostEvent = Platform::CreateNamedEvent(hEvtName.c_str());

        // Initialize Slots
        uint8_t* ptr = (uint8_t*)shmBase + headerSize;
        slots.reserve(this->numSlots);

        for (uint32_t i = 0; i < this->numSlots; ++i) {
            auto ctx = std::make_unique<SlotContext>();
            ctx->header = (SlotHeader*)ptr;
            ctx->reqData = ptr + sizeof(SlotHeader);
            ctx->respData = ctx->reqData + this->slotSize;

            if (!exists) {
                ctx->header->state.store(SLOT_FREE, std::memory_order_relaxed);
            }

            slots.push_back(std::move(ctx));
            ptr += perSlotTotal;
        }

        running = true;
        scannerThread = std::thread(&DirectHost::ScannerLoop, this);
        heartbeatThread = std::thread(&DirectHost::HeartbeatLoop, this);

        return true;
    }

    void Shutdown() {
        if (!running) return;
        running = false;

        // Wake up sleeping requests
        for (auto& slot : slots) {
            slot->cv.notify_all();
        }

        // Wake up scanner if sleeping
        Platform::SignalEvent(globalHostEvent);

        if (scannerThread.joinable()) scannerThread.join();
        if (heartbeatThread.joinable()) heartbeatThread.join();

        if (shmBase) Platform::CloseShm(hMapFile, shmBase);
        Platform::CloseEvent(globalGuestEvent);
        Platform::CloseEvent(globalHostEvent);
    }

    // Blocking Request
    // Returns true on success, false on error/shutdown
    bool Request(const uint8_t* data, uint32_t size, std::vector<uint8_t>& outResp) {
        // 1. Find Free Slot
        int slotIdx = -1;
        // Scan for free slot with extended retry
        for (int retry = 0; retry < 1000000; ++retry) {
             for (uint32_t i = 0; i < numSlots; ++i) {
                uint32_t expected = SLOT_FREE;
                if (slots[i]->header->state.compare_exchange_strong(expected, SLOT_BUSY, std::memory_order_acquire)) {
                    slotIdx = i;
                    goto found;
                }
            }
            if (retry % 100 == 0) std::this_thread::yield();
        }
        return false; // Timeout finding slot

    found:
        SlotContext* ctx = slots[slotIdx].get();

        // 2. Mark Active (For Orphan Detection)
        ctx->requestActive.store(true, std::memory_order_release);

        // 3. Write Request
        if (size > slotSize) size = slotSize;
        if (data && size > 0) {
            memcpy(ctx->reqData, data, size);
        }
        ctx->header->reqSize = size;
        ctx->header->msgId = MSG_ID_NORMAL;

        // Reset local state
        {
            std::lock_guard<std::mutex> lock(ctx->mutex);
            ctx->responseReady = false;
        }

        // 4. Set Ready
        ctx->header->state.store(SLOT_REQ_READY, std::memory_order_release);

        // 5. Wake Guest Scanner if needed
        if (exchangeHeader->guestScannerState.load(std::memory_order_relaxed) == SCANNER_STATE_SLEEPING) {
            Platform::SignalEvent(globalGuestEvent);
        }

        // 6. Wait for Response
        {
            std::unique_lock<std::mutex> lock(ctx->mutex);
            ctx->cv.wait(lock, [&]{ return ctx->responseReady || !running; });

            // Critical Section: Finalize Slot State
            // We hold the lock here. Scanner also locks before setting responseReady.

            ctx->requestActive.store(false, std::memory_order_release);

            if (ctx->responseReady) {
                // Success: Scanner delivered data.
                outResp = std::move(ctx->responseBuffer);
                ctx->header->state.store(SLOT_HOST_DONE, std::memory_order_release);
                return true;
            }
        }

        // If we are here, it means !running (Shutdown/Abort) AND Scanner didn't deliver yet.
        // We already set requestActive = false.
        // Scanner, if it comes later, will see requestActive == false and clean up.

        return false;
    }

    bool Send(const uint8_t* data, uint32_t size, uint32_t msgId) {
        if (msgId == MSG_ID_SHUTDOWN) {
            return true;
        }
        return false;
    }

    bool IsGuestAlive() {
        return guestAlive.load();
    }

private:
    void HeartbeatLoop() {
        uint64_t last_hb = 0;
        int stale_count = 0;
        const int stale_threshold = 3; // 3 cycles to declare dead

        while (running) {
            exchangeHeader->hostHeartbeat++;
            uint64_t current_hb = exchangeHeader->guestHeartbeat.load(std::memory_order_seq_cst);

            if (current_hb > 0) { // Start checking only after first heartbeat
                if (current_hb == last_hb) {
                    stale_count++;
                } else {
                    stale_count = 0;
                }
            }

            last_hb = current_hb;

            if (stale_count >= stale_threshold) {
                guestAlive.store(false, std::memory_order_release);
            } else {
                guestAlive.store(true, std::memory_order_release);
            }

            // Check every 2 seconds
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    void ScannerLoop() {
        // Initialize Scanner State
        exchangeHeader->hostScannerState.store(SCANNER_STATE_ACTIVE);

        while (running) {
            bool worked = false;

            for (uint32_t i = 0; i < numSlots; ++i) {
                SlotContext* ctx = slots[i].get();
                if (ctx->header->state.load(std::memory_order_acquire) == SLOT_RESP_READY) {

                    // Synchronization with Request Thread
                    std::lock_guard<std::mutex> lock(ctx->mutex);

                    bool active = ctx->requestActive.load(std::memory_order_acquire);

                    if (active) {
                        // Process Response
                        std::vector<uint8_t> resp;
                        uint32_t respLen = ctx->header->respSize;
                        if (respLen > slotSize) respLen = slotSize;

                        if (respLen > 0) {
                            resp.assign(ctx->respData, ctx->respData + respLen);
                        }

                        // Notify Waiting Thread
                        ctx->responseBuffer = std::move(resp);
                        ctx->responseReady = true;
                        ctx->cv.notify_one();
                    } else {
                        // Orphaned Response (Request Aborted/Shutdown)
                        // Clean it up so Guest can proceed.
                        ctx->header->state.store(SLOT_HOST_DONE, std::memory_order_release);
                    }

                    worked = true;
                }
            }

            if (!worked) {
                // Adaptive Wait
                // 1. Spin Phase
                bool foundSpin = false;
                for (int spin = 0; spin < 10000; ++spin) {
                    Platform::CpuRelax();
                    for (uint32_t i = 0; i < numSlots; ++i) {
                        if (slots[i]->header->state.load(std::memory_order_relaxed) == SLOT_RESP_READY) {
                            foundSpin = true;
                            break;
                        }
                    }
                    if (foundSpin) break;
                }
                if (foundSpin) continue;

                // 2. Sleep Phase
                exchangeHeader->hostScannerState.store(SCANNER_STATE_SLEEPING);

                // Double check before sleeping
                bool found = false;
                for (uint32_t i = 0; i < numSlots; ++i) {
                     if (slots[i]->header->state.load(std::memory_order_relaxed) == SLOT_RESP_READY) {
                         found = true;
                         break;
                     }
                }

                if (found) {
                    exchangeHeader->hostScannerState.store(SCANNER_STATE_ACTIVE);
                    continue;
                }

                Platform::WaitEvent(globalHostEvent, 1);
                exchangeHeader->hostScannerState.store(SCANNER_STATE_ACTIVE);
            } else {
                 Platform::CpuRelax();
            }
        }
    }
};

}
