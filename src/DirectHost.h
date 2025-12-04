#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <thread>
#include <cstring>
#include <iostream>
#include <functional>
#include "Platform.h"
#include "IPCUtils.h"

namespace shm {

class DirectHost {
    void* shmBase;
    std::string shmName;
    uint32_t numQueues;
    ShmHandle hMapFile;
    bool running;

    struct Lane {
        SlotHeader* slots;
        uint32_t slotCount;
        EventHandle hReqEvent;
    };

    std::vector<Lane> lanes;
    std::atomic<uint32_t> nextLane{0};

    // Reader threads (one per lane)
    std::vector<std::thread> readerThreads;

    std::function<void(std::vector<uint8_t>&&, uint32_t)> onMessage;

    // Config
    uint32_t slotDataSize;
    size_t perSlotTotal;

public:
    DirectHost() : shmBase(nullptr), hMapFile(0), running(false) {}
    ~DirectHost() { Shutdown(); }

    bool Init(const std::string& shmName, uint32_t numQueues,
              std::function<void(std::vector<uint8_t>&&, uint32_t)> msgHandler) {
        this->shmName = shmName;
        this->numQueues = numQueues;
        this->onMessage = msgHandler;

        // Force 1 slot per lane for compatibility with Go DirectGuest
        uint32_t slotsPerLane = 1;
        this->slotDataSize = 1024 * 1024; // 1MB
        this->perSlotTotal = sizeof(SlotHeader) + slotDataSize;

        size_t exchangeHeaderSize = sizeof(ExchangeHeader);
        if (exchangeHeaderSize < 64) exchangeHeaderSize = 64;

        size_t laneSize = slotsPerLane * perSlotTotal;
        size_t totalSize = exchangeHeaderSize + (laneSize * numQueues);

        bool exists = false;
        shmBase = Platform::CreateNamedShm(shmName.c_str(), totalSize, hMapFile, exists);
        if (!shmBase) return false;

        // Zero out memory if new
        memset(shmBase, 0, totalSize);

        // Write ExchangeHeader
        ExchangeHeader* exHeader = (ExchangeHeader*)shmBase;
        exHeader->numSlots = numQueues;
        exHeader->slotSize = slotDataSize;

        lanes.resize(numQueues);
        uint8_t* ptr = (uint8_t*)shmBase + exchangeHeaderSize;

        for (uint32_t i = 0; i < numQueues; ++i) {
            lanes[i].slots = (SlotHeader*)ptr;
            lanes[i].slotCount = slotsPerLane;

            // Event name per lane
            std::string evtName = shmName + "_slot_" + std::to_string(i);
            lanes[i].hReqEvent = Platform::CreateNamedEvent(evtName.c_str());

            // Initialize slots
            uint8_t* slotPtr = ptr;
            for (uint32_t j = 0; j < slotsPerLane; ++j) {
                SlotHeader* header = (SlotHeader*)slotPtr;
                header->state.store(SLOT_FREE, std::memory_order_relaxed);
                slotPtr += perSlotTotal;
            }

            ptr += laneSize;
        }

        running = true;
        // Start reader threads
        for (uint32_t i = 0; i < numQueues; ++i) {
            readerThreads.emplace_back(&DirectHost::ReaderLoop, this, i);
        }

        return true;
    }

    void Shutdown() {
        if (!running) return;
        running = false;
        // Wait for threads
        for (auto& t : readerThreads) {
            if (t.joinable()) t.join();
        }

        if (shmBase) Platform::CloseShm(hMapFile, shmBase);
        for (auto& lane : lanes) {
            Platform::CloseEvent(lane.hReqEvent);
        }
    }

    // Returns true if sent
    bool Send(const uint8_t* data, uint32_t size, uint32_t msgId) {
        // Round robin
        uint32_t laneIdx = nextLane++ % numQueues;
        Lane& lane = lanes[laneIdx];

        return SendToLane(lane, data, size, msgId);
    }

private:
    bool SendToLane(Lane& lane, const uint8_t* data, uint32_t size, uint32_t msgId) {
        // Find a free slot
        for (int retry=0; retry<3; retry++) {
            uint8_t* ptr = (uint8_t*)lane.slots;
            for (uint32_t i = 0; i < lane.slotCount; ++i) {
                SlotHeader* slot = (SlotHeader*)ptr;
                uint32_t expected = SLOT_FREE;
                if (slot->state.compare_exchange_strong(expected, SLOT_BUSY, std::memory_order_acquire)) {
                    // Found slot
                    if (size > slotDataSize) return false;

                    slot->msgId = msgId;
                    slot->reqSize = size;

                    if (data && size > 0) {
                        uint8_t* dataPtr = ptr + sizeof(SlotHeader);
                        memcpy(dataPtr, data, size);
                    }

                    slot->state.store(SLOT_REQ_READY, std::memory_order_release);
                    Platform::SignalEvent(lane.hReqEvent);
                    return true;
                }
                ptr += perSlotTotal;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    void ReaderLoop(uint32_t laneIdx) {
        Lane& lane = lanes[laneIdx];
        while (running) {
            bool worked = false;
            uint8_t* ptr = (uint8_t*)lane.slots;
            for (uint32_t i = 0; i < lane.slotCount; ++i) {
                SlotHeader* slot = (SlotHeader*)ptr;
                if (slot->state.load(std::memory_order_acquire) == SLOT_RESP_READY) {
                    // Process response
                    std::vector<uint8_t> resp;
                    uint32_t respLen = slot->respSize;
                    if (respLen > 0) {
                        if (respLen > slotDataSize) respLen = slotDataSize;
                        uint8_t* dataPtr = ptr + sizeof(SlotHeader);
                        resp.assign(dataPtr, dataPtr + respLen);
                    }

                    uint32_t msgId = slot->msgId;

                    slot->state.store(SLOT_FREE, std::memory_order_release);
                    worked = true;

                    if (onMessage) {
                        onMessage(std::move(resp), msgId);
                    }
                }
                ptr += perSlotTotal;
            }
            if (!worked) {
                std::this_thread::yield();
            }
        }
    }
};

}
