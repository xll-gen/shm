#pragma once
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstring>
#include <chrono>
#include <iostream>
#include "IPCUtils.h"
#include "Platform.h"
#include "../benchmarks/include/ipc_generated.h"

namespace shm {

class DirectHost {
    void* shmBase;
    std::string shmName;
    uint32_t numSlots;
    uint32_t slotSize;
    ShmHandle hMapFile;

    struct SlotContext {
        SlotHeader* header;
        uint8_t* data;
        EventHandle hEvent;
        EventHandle hRespEvent;
    };

    std::vector<SlotContext> slots;
    std::atomic<bool> running;

    std::atomic<int32_t> targetPollers;
    std::atomic<int32_t> activePollers;

public:
    DirectHost() : shmBase(nullptr), hMapFile(0), running(false) {
        activePollers = 0;
        int cpus = std::thread::hardware_concurrency();
        if (cpus < 1) cpus = 1;
        int target = cpus / 4;
        if (target < 1) target = 1;
        targetPollers = target;
    }
    ~DirectHost() { Shutdown(); }

    bool Init(const std::string& shmName, uint32_t numSlots, uint32_t slotSize) {
        this->shmName = shmName;
        this->numSlots = numSlots;
        this->slotSize = slotSize;

        size_t headerSize = sizeof(ExchangeHeader);
        size_t perSlotSize = sizeof(SlotHeader) + slotSize;
        size_t totalSize = headerSize + (perSlotSize * numSlots);

        bool exists = false;
        shmBase = Platform::CreateNamedShm(shmName.c_str(), totalSize, hMapFile, exists);
        if (!shmBase) return false;

        memset(shmBase, 0, totalSize);

        ExchangeHeader* exHead = (ExchangeHeader*)shmBase;
        exHead->numSlots = numSlots;
        exHead->slotSize = slotSize;

        uint8_t* ptr = (uint8_t*)shmBase + headerSize;

        for (uint32_t i = 0; i < numSlots; i++) {
            SlotContext ctx;
            ctx.header = (SlotHeader*)ptr;
            ctx.data = ptr + sizeof(SlotHeader);

            std::string evName = shmName + "_slot_" + std::to_string(i);
            ctx.hEvent = Platform::CreateNamedEvent(evName.c_str());

            std::string respEvName = shmName + "_slot_" + std::to_string(i) + "_resp";
            ctx.hRespEvent = Platform::CreateNamedEvent(respEvName.c_str());

            slots.push_back(ctx);
            ptr += perSlotSize;
        }

        running = true;
        return true;
    }

    void Shutdown() {
        running = false;
        if (shmBase) Platform::CloseShm(hMapFile, shmBase);
        for (auto& s : slots) {
            Platform::CloseEvent(s.hEvent);
            Platform::CloseEvent(s.hRespEvent);
        }
        slots.clear();
    }

    bool Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse) {
        return CallWithId(reqData, reqSize, MSG_ID_NORMAL, outResponse);
    }

    bool CallWithId(const uint8_t* reqData, size_t reqSize, uint32_t msgId, std::vector<uint8_t>& outResponse) {
        int slotIdx = -1;
        bool wasSleeping = false;

        while (running) {
            // Pass 1: Look for POLLING (Priority)
            for (int i = 0; i < (int)numSlots; i++) {
                uint32_t expected = SLOT_POLLING;
                if (slots[i].header->state.compare_exchange_strong(expected, SLOT_BUSY)) {
                     slotIdx = i; wasSleeping = false; goto Found;
                }
            }
            // Pass 2: Look for FREE (Worker waiting)
            for (int i = 0; i < (int)numSlots; i++) {
                uint32_t expected = SLOT_FREE;
                if (slots[i].header->state.compare_exchange_strong(expected, SLOT_BUSY)) {
                     slotIdx = i; wasSleeping = true; goto Found;
                }
            }
            std::this_thread::yield();
        }
        return false;

    Found:
        SlotContext& slot = slots[slotIdx];
        if (reqSize > slotSize) {
             slot.header->state.store(wasSleeping ? SLOT_FREE : SLOT_POLLING);
             return false;
        }

        if (reqData && reqSize > 0) memcpy(slot.data, reqData, reqSize);
        slot.header->reqSize = (uint32_t)reqSize;
        slot.header->msgId = msgId;

        slot.header->state.store(SLOT_REQ_READY, std::memory_order_release);

        if (wasSleeping) {
            Platform::SignalEvent(slot.hEvent);
        }

        if (msgId == MSG_ID_SHUTDOWN) {
             return true;
        }

        // Wait for Response (Adaptive)
        bool success = false;
        bool canSpin = false;
        int currentActive = activePollers.load(std::memory_order_relaxed);
        int currentTarget = targetPollers.load(std::memory_order_relaxed);

        if (currentActive < currentTarget) {
            activePollers++;
            canSpin = true;
        }

        if (canSpin) {
            int spins = 0;
            const int SpinLimit = 100000;
            while (running && spins < SpinLimit) {
                uint32_t s = slot.header->state.load(std::memory_order_acquire);
                if (s == SLOT_RESP_READY) {
                    success = true;
                    break;
                }
                spins++;
                 if (spins % 100 == 0) std::atomic_thread_fence(std::memory_order_seq_cst); // or pause
            }
            activePollers--;

            if (!success) {
                // Spin failed (timeout) -> Decrease target
                int t = targetPollers.load();
                if (t > 1) targetPollers.store(t - 1);
            }
        }

        if (!success) {
            // Must wait
            slot.header->hostState.store(HOST_STATE_WAITING, std::memory_order_release);

            // Double check to avoid race
             if (slot.header->state.load(std::memory_order_acquire) == SLOT_RESP_READY) {
                 slot.header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
                 success = true;
             } else {
                 Platform::WaitEvent(slot.hRespEvent);
                 slot.header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);

                 // We were forced to wait -> Increase target
                 int t = targetPollers.load();
                 // Cap at some reasonable limit (e.g. numSlots or arbitrary high)
                 if (t < (int)numSlots) targetPollers.store(t + 1);
             }
        }

        if (!running) return false;

        uint32_t respSize = slot.header->respSize;
        if (respSize > slotSize) respSize = slotSize;
        outResponse.resize(respSize);
        memcpy(outResponse.data(), slot.data, respSize);

        slot.header->state.store(SLOT_HOST_DONE, std::memory_order_release);
        return true;
    }

    void SendShutdown() {
         std::vector<uint8_t> dummy;
         for (int i = 0; i < (int)numSlots; i++) {
             CallSlot(i, nullptr, 0, MSG_ID_SHUTDOWN, dummy);
         }
    }

    bool CallSlot(int slotIdx, const uint8_t* reqData, size_t reqSize, uint32_t msgId, std::vector<uint8_t>& outResponse) {
        SlotContext& slot = slots[slotIdx];
        bool wasSleeping = false;

        while (running) {
             uint32_t s = slot.header->state.load(std::memory_order_relaxed);
             if (s == SLOT_POLLING) {
                 if (slot.header->state.compare_exchange_strong(s, SLOT_BUSY)) {
                     wasSleeping = false;
                     goto Found;
                 }
             } else if (s == SLOT_FREE) {
                 if (slot.header->state.compare_exchange_strong(s, SLOT_BUSY)) {
                     wasSleeping = true;
                     goto Found;
                 }
             }
             std::this_thread::yield();
        }
        return false;

    Found:
        if (reqData && reqSize > 0) memcpy(slot.data, reqData, reqSize);
        slot.header->reqSize = (uint32_t)reqSize;
        slot.header->msgId = msgId;

        slot.header->state.store(SLOT_REQ_READY, std::memory_order_release);
        if (wasSleeping) Platform::SignalEvent(slot.hEvent);

        if (msgId == MSG_ID_SHUTDOWN) return true;

         // Wait for Response (Adaptive) - Duplicated logic for CallSlot?
         // Ideally refactor, but for now inline.
        bool success = false;
        bool canSpin = false;
        int currentActive = activePollers.load(std::memory_order_relaxed);
        int currentTarget = targetPollers.load(std::memory_order_relaxed);

        if (currentActive < currentTarget) {
            activePollers++;
            canSpin = true;
        }

        if (canSpin) {
            int spins = 0;
            const int SpinLimit = 100000;
            while (running && spins < SpinLimit) {
                uint32_t s = slot.header->state.load(std::memory_order_acquire);
                if (s == SLOT_RESP_READY) {
                    success = true;
                    break;
                }
                spins++;
                if (spins % 100 == 0) std::atomic_thread_fence(std::memory_order_seq_cst);
            }
            activePollers--;

            if (!success) {
                int t = targetPollers.load();
                if (t > 1) targetPollers.store(t - 1);
            }
        }

        if (!success) {
            slot.header->hostState.store(HOST_STATE_WAITING, std::memory_order_release);
            if (slot.header->state.load(std::memory_order_acquire) == SLOT_RESP_READY) {
                 slot.header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
                 success = true;
            } else {
                 Platform::WaitEvent(slot.hRespEvent);
                 slot.header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
                 int t = targetPollers.load();
                 if (t < (int)numSlots) targetPollers.store(t + 1);
            }
        }

        if (!running) return false;

        uint32_t respSize = slot.header->respSize;
        if (respSize > slotSize) respSize = slotSize;
        outResponse.resize(respSize);
        memcpy(outResponse.data(), slot.data, respSize);

        slot.header->state.store(SLOT_HOST_DONE, std::memory_order_release);
        return true;
    }
};

}
