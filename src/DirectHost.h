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

/**
 * @brief Host implementation for Direct IPC Mode.
 *
 * Manages a set of "Lanes" (Slots), each corresponding to a dedicated worker thread.
 * Ideal for request-response patterns requiring low latency (1:1 thread mapping).
 *
 * Architecture:
 * - Creates 'N' slots in shared memory.
 * - Each slot has a Request Event (Host->Guest) and a Response Event (Guest->Host is implied via State, but Host has no event).
 * - Actually, the `hReqEvent` signals the Guest worker.
 * - The Host spins/yields on `SlotState::SLOT_RESP_READY`.
 */
class DirectHost {
    void* shmBase;
    std::string shmName;
    uint32_t numQueues;
    ShmHandle hMapFile;
    bool running;

    /**
     * @brief Internal Lane structure managing a single worker's slot.
     */
    struct Lane {
        SlotHeader* slots;    ///< Pointer to the slot array in SHM.
        uint32_t slotCount;   ///< Number of slots assigned to this lane (usually 1 or chunk).
        EventHandle hReqEvent;///< Event to signal the Guest worker for this lane.
    };

    std::vector<Lane> lanes;
    std::atomic<uint32_t> nextLane{0};

    // Reader threads (one per lane) to poll for responses
    std::vector<std::thread> readerThreads;

    std::function<void(std::vector<uint8_t>&&, uint32_t)> onMessage;

    // Config
    uint32_t slotDataSize;
    size_t perSlotTotal;

public:
    /**
     * @brief Constructs a DirectHost instance.
     */
    DirectHost() : shmBase(nullptr), hMapFile(0), running(false) {}

    /**
     * @brief Destructor. Ensures clean shutdown.
     */
    ~DirectHost() { Shutdown(); }

    /**
     * @brief Initializes the Direct Host.
     *
     * Creates shared memory and events, and starts reader threads.
     *
     * @param shmName Name of the shared memory region.
     * @param numQueues Number of independent lanes/workers to support.
     * @param msgHandler Callback function for received messages.
     * @return true if initialization succeeded, false otherwise.
     */
    bool Init(const std::string& shmName, uint32_t numQueues,
              std::function<void(std::vector<uint8_t>&&, uint32_t)> msgHandler) {
        this->shmName = shmName;
        this->numQueues = numQueues;
        this->onMessage = msgHandler;

        uint32_t slotsPerLane = 1024; // Actually, code treats this as capacity PER LANE?
        // Wait, the logic below: `laneSize = slotsPerLane * perSlotTotal`.
        // And `lanes[i].slots` points to start of that block.
        // But `SendToLane` iterates `lane.slotCount`.
        // So this supports multiple slots per lane (async pipelining on one thread).

        this->slotDataSize = 1024 * 1024; // 1MB per slot
        this->perSlotTotal = sizeof(SlotHeader) + slotDataSize;

        size_t laneSize = slotsPerLane * perSlotTotal;
        size_t totalSize = laneSize * numQueues;

        bool exists = false;
        shmBase = Platform::CreateNamedShm(shmName.c_str(), totalSize, hMapFile, exists);
        if (!shmBase) return false;

        // Zero out memory if new
        memset(shmBase, 0, totalSize);

        lanes.resize(numQueues);
        uint8_t* ptr = (uint8_t*)shmBase;

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

    /**
     * @brief Shuts down the host, stops threads, and releases resources.
     */
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

    /**
     * @brief Sends data via a free slot in a round-robin lane.
     *
     * @param data Pointer to data.
     * @param size Size of data.
     * @param msgId Message ID.
     * @return true if sent, false if no slots available (after retries).
     */
    bool Send(const uint8_t* data, uint32_t size, uint32_t msgId) {
        // Round robin
        uint32_t laneIdx = nextLane++ % numQueues;
        Lane& lane = lanes[laneIdx];

        return SendToLane(lane, data, size, msgId);
    }

private:
    /**
     * @brief Attempts to find a free slot in the specific lane and write data.
     */
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

    /**
     * @brief Background loop for reading responses from a specific lane.
     */
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
