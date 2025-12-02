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
    };

    std::vector<SlotContext> slots;
    std::atomic<bool> running;

public:
    DirectHost() : shmBase(nullptr), hMapFile(0), running(false) {}
    ~DirectHost() { Shutdown(); }

    bool Init(const std::string& shmName, uint32_t numSlots, uint32_t slotSize) {
        this->shmName = shmName;
        this->numSlots = numSlots;
        this->slotSize = slotSize;

        size_t headerSize = sizeof(ExchangeHeader);
        size_t perSlotSize = sizeof(SlotHeader) + slotSize;
        size_t totalSize = headerSize + (perSlotSize * numSlots);

        bool exists = false;
        shmBase = Platform::CreateNamedShm(shmName.c_str(), totalShmSize, hMapFile, exists);
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

        while (running) {
            uint32_t s = slot.header->state.load(std::memory_order_acquire);
            if (s == SLOT_RESP_READY) break;
            std::atomic_thread_fence(std::memory_order_seq_cst);
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

        while (running) {
            if (slot.header->state.load(std::memory_order_acquire) == SLOT_RESP_READY) break;
            std::atomic_thread_fence(std::memory_order_seq_cst);
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
