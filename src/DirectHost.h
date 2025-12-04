#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <thread>
#include <cstring>
#include <iostream>
#include <functional>
#include <atomic>
#include "Platform.h"
#include "IPCUtils.h"

namespace shm {

class DirectHost {
    void* shmBase;
    std::string shmName;
    ShmHandle hMapFile;
    bool running;

    struct SlotContext {
        SlotHeader* header;
        uint8_t* reqBuffer;
        uint8_t* respBuffer;
        EventHandle hReqEvent;  // Guest waits on this
        EventHandle hRespEvent; // Host waits on this
    };

    std::vector<SlotContext> slots;
    uint32_t numSlots;
    uint32_t slotTotalDataSize; // Combined Req+Resp size
    uint32_t halfSize;

    std::vector<std::thread> readerThreads;

    std::function<void(std::vector<uint8_t>&&, uint32_t)> onMessage;

public:
    DirectHost() : shmBase(nullptr), hMapFile(0), running(false) {}
    ~DirectHost() { Shutdown(); }

    bool Init(const std::string& shmName, uint32_t numSlots,
              std::function<void(std::vector<uint8_t>&&, uint32_t)> msgHandler) {
        this->shmName = shmName;
        this->numSlots = numSlots;
        this->onMessage = msgHandler;
        this->running = true;

        // Configuration
        this->slotTotalDataSize = 1024 * 1024; // 1MB total
        this->halfSize = this->slotTotalDataSize / 2;

        uint32_t headerSize = sizeof(ExchangeHeader);
        if (headerSize < 64) headerSize = 64;

        uint32_t slotHeaderSize = sizeof(SlotHeader);

        uint64_t perSlotSize = slotHeaderSize + slotTotalDataSize;
        uint64_t totalShmSize = headerSize + (perSlotSize * numSlots);

        bool exists = false;
        shmBase = Platform::CreateNamedShm(shmName.c_str(), totalShmSize, hMapFile, exists);
        if (!shmBase) return false;

        // Init Exchange Header
        ExchangeHeader* exHeader = (ExchangeHeader*)shmBase;
        exHeader->numSlots = numSlots;
        exHeader->slotSize = slotTotalDataSize; // Guest will divide by 2

        uint8_t* ptr = (uint8_t*)shmBase + headerSize;
        slots.resize(numSlots);

        for (uint32_t i = 0; i < numSlots; ++i) {
            SlotContext& ctx = slots[i];
            ctx.header = (SlotHeader*)ptr;
            ctx.reqBuffer = ptr + slotHeaderSize;
            ctx.respBuffer = ctx.reqBuffer + halfSize;

            // Initialize Header
            ctx.header->state.store(SLOT_WAIT_REQ, std::memory_order_relaxed);
            ctx.header->hostSleeping.store(0, std::memory_order_relaxed);
            ctx.header->guestSleeping.store(0, std::memory_order_relaxed);

            // Events
            std::string reqEvtName = shmName + "_slot_" + std::to_string(i);
            std::string respEvtName = shmName + "_slot_" + std::to_string(i) + "_resp";

            ctx.hReqEvent = Platform::CreateNamedEvent(reqEvtName.c_str());
            ctx.hRespEvent = Platform::CreateNamedEvent(respEvtName.c_str());

            ptr += perSlotSize;
        }

        // Start Reader Threads
        for (uint32_t i = 0; i < numSlots; ++i) {
            readerThreads.emplace_back(&DirectHost::WorkerLoop, this, i);
        }

        return true;
    }

    void Shutdown() {
        if (!running) return;
        running = false;

        // Signal Shutdown to all slots
        for (auto& ctx : slots) {
             ctx.header->state.store(SLOT_DONE, std::memory_order_seq_cst);
             Platform::SignalEvent(ctx.hReqEvent); // Wake guest
             Platform::SignalEvent(ctx.hRespEvent); // Wake self
        }

        for (auto& t : readerThreads) {
            if (t.joinable()) t.join();
        }

        if (shmBase) Platform::CloseShm(hMapFile, shmBase);
        for (auto& ctx : slots) {
            Platform::CloseEvent(ctx.hReqEvent);
            Platform::CloseEvent(ctx.hRespEvent);
        }
    }

    // Round robin send
    bool Send(const uint8_t* data, uint32_t size, uint32_t msgId) {
        static std::atomic<uint32_t> nextSlot{0};
        uint32_t startIdx = nextSlot++ % numSlots;

        // Try to find a free slot (WaitReq) starting from round-robin index
        for (uint32_t i = 0; i < numSlots; ++i) {
            uint32_t idx = (startIdx + i) % numSlots;
            SlotContext& ctx = slots[idx];

            // Optimistic check first
            if (ctx.header->state.load(std::memory_order_acquire) == SLOT_WAIT_REQ) {
                // Prepare Data
                if (size > halfSize) return false; // Too big

                ctx.header->msgId = msgId;
                ctx.header->reqSize = size;
                if (data && size > 0) {
                    memcpy(ctx.reqBuffer, data, size);
                }

                // Transition to REQ_READY
                ctx.header->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

                // Wake Guest if sleeping
                if (ctx.header->guestSleeping.load(std::memory_order_seq_cst) == 1) {
                    Platform::SignalEvent(ctx.hReqEvent);
                }
                return true;
            }
        }
        return false; // All busy
    }

private:
    void WorkerLoop(uint32_t slotIdx) {
        SlotContext& ctx = slots[slotIdx];

        // Adaptive Spin Params
        int spinLimit = 2000;
        const int MIN_SPIN = 1;
        const int MAX_SPIN = 2000;

        while (running) {
            // Wait for Response (SLOT_RESP_READY)
            bool ready = false;

            // 1. Spin
            for (int i = 0; i < spinLimit; ++i) {
                if (ctx.header->state.load(std::memory_order_acquire) == SLOT_RESP_READY) {
                    ready = true;
                    break;
                }
                Platform::ThreadYield();
            }

            if (ready) {
                if (spinLimit < MAX_SPIN) spinLimit += 100;
            } else {
                if (spinLimit > MIN_SPIN) spinLimit -= 500;

                // 2. Sleep
                ctx.header->hostSleeping.store(1, std::memory_order_seq_cst);

                // Double check
                if (ctx.header->state.load(std::memory_order_seq_cst) == SLOT_RESP_READY) {
                    ctx.header->hostSleeping.store(0, std::memory_order_relaxed);
                    ready = true;
                } else {
                    Platform::WaitEvent(ctx.hRespEvent);
                    ctx.header->hostSleeping.store(0, std::memory_order_relaxed);
                }
            }

            // Check shutdown
            if (!running) break;

            // Re-check state
            uint32_t s = ctx.header->state.load(std::memory_order_acquire);
            if (s == SLOT_RESP_READY) {
                // Process Response
                std::vector<uint8_t> resp;
                uint32_t rSize = ctx.header->respSize;
                if (rSize > halfSize) rSize = halfSize;

                if (rSize > 0) {
                    resp.assign(ctx.respBuffer, ctx.respBuffer + rSize);
                }

                uint32_t msgId = ctx.header->msgId;

                // Reset to WAIT_REQ so we can send again
                ctx.header->state.store(SLOT_WAIT_REQ, std::memory_order_seq_cst);

                // Callback
                if (onMessage) {
                    onMessage(std::move(resp), msgId);
                }
            }
        }
    }
};

}
