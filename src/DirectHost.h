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

/**
 * @class DirectHost
 * @brief Implements the Host side of the Direct Mode IPC.
 *
 * The DirectHost manages a pool of slots in shared memory. Each slot is intended
 * to be paired with a specific Guest worker thread.
 * It uses a hybrid spin/wait strategy for low latency and utilizes
 * specific memory layout defined in IPCUtils.h.
 */
class DirectHost {
    void* shmBase;
    std::string shmName;
    uint32_t numSlots;
    uint64_t totalShmSize;
    ShmHandle hMapFile;
    bool running;

    /**
     * @brief Internal representation of a Slot.
     */
    struct Slot {
        SlotHeader* header;
        uint8_t* reqBuffer;
        uint8_t* respBuffer;
        EventHandle hReqEvent;  // Signaled by Host (Wake Guest)
        EventHandle hRespEvent; // Signaled by Guest (Wake Host)
        uint32_t maxReqSize;
        uint32_t maxRespSize;
        int spinLimit;
    };

    std::vector<Slot> slots;
    std::atomic<uint32_t> nextSlot{0}; // Round-robin hint

    // Config
    uint32_t slotSize; // Total payload size per slot

public:
    /**
     * @brief Default constructor.
     */
    DirectHost() : shmBase(nullptr), hMapFile(0), running(false) {}

    /**
     * @brief Destructor. Ensures Shutdown is called.
     */
    ~DirectHost() { Shutdown(); }

    /**
     * @brief Initializes the Shared Memory Host.
     *
     * Creates the shared memory region and initializes the ExchangeHeader and SlotHeaders.
     * Also creates the necessary synchronization events for each slot.
     *
     * @param shmName The name of the shared memory region.
     * @param numQueues The number of slots (workers) to allocate.
     * @param dataSize The total size of the data payload per slot (split between Req/Resp). Default 1MB.
     * @return true if initialization succeeded, false otherwise.
     */
    bool Init(const std::string& shmName, uint32_t numQueues, uint32_t dataSize = 1024 * 1024) {
        this->shmName = shmName;
        this->numSlots = numQueues; // Interpret numQueues as numSlots (1:1 workers)
        this->slotSize = dataSize;

        // Split strategy: 50/50
        uint32_t halfSize = slotSize / 2;
        // Align to 64 bytes
        halfSize = (halfSize / 64) * 64;
        if (halfSize < 64) halfSize = 64;

        uint32_t reqOffset = 0;
        uint32_t respOffset = halfSize;

        // Ensure total fits
        if (respOffset + halfSize > slotSize) {
             slotSize = respOffset + halfSize;
        }

        size_t exchangeHeaderSize = sizeof(ExchangeHeader);
        if (exchangeHeaderSize < 64) exchangeHeaderSize = 64;

        size_t slotHeaderSize = sizeof(SlotHeader);
        // Should be 128

        size_t perSlotTotal = slotHeaderSize + slotSize;
        size_t totalSize = exchangeHeaderSize + (perSlotTotal * numSlots);
        this->totalShmSize = totalSize;

        bool exists = false;
        shmBase = Platform::CreateNamedShm(shmName.c_str(), totalSize, hMapFile, exists);
        if (!shmBase) return false;

        // Zero out memory if new (or always, to be safe?)
        memset(shmBase, 0, totalSize);

        // Write ExchangeHeader
        ExchangeHeader* exHeader = (ExchangeHeader*)shmBase;
        exHeader->numSlots = numSlots;
        exHeader->slotSize = slotSize;
        exHeader->reqOffset = reqOffset;
        exHeader->respOffset = respOffset;

        slots.resize(numSlots);
        uint8_t* ptr = (uint8_t*)shmBase + exchangeHeaderSize;

        for (uint32_t i = 0; i < numSlots; ++i) {
            slots[i].header = (SlotHeader*)ptr;
            uint8_t* dataBase = ptr + slotHeaderSize;
            slots[i].reqBuffer = dataBase + reqOffset;
            slots[i].respBuffer = dataBase + respOffset;
            slots[i].maxReqSize = halfSize;
            slots[i].maxRespSize = slotSize - respOffset;
            slots[i].spinLimit = 5000;

            // Events
            std::string reqName = shmName + "_slot_" + std::to_string(i);
            std::string respName = shmName + "_slot_" + std::to_string(i) + "_resp";

            slots[i].hReqEvent = Platform::CreateNamedEvent(reqName.c_str());
            slots[i].hRespEvent = Platform::CreateNamedEvent(respName.c_str());

            // Initialize Header
            slots[i].header->state.store(SLOT_FREE, std::memory_order_relaxed);
            slots[i].header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
            slots[i].header->guestState.store(GUEST_STATE_ACTIVE, std::memory_order_relaxed);

            ptr += perSlotTotal;
        }

        running = true;
        return true;
    }

    /**
     * @brief Shuts down the host, closing all handles and unmapping memory.
     */
    void Shutdown() {
        if (!running) return;

        for (auto& slot : slots) {
             Platform::CloseEvent(slot.hReqEvent);
             Platform::CloseEvent(slot.hRespEvent);
        }

        if (shmBase) Platform::CloseShm(hMapFile, shmBase, totalShmSize);
        running = false;
    }

    /**
     * @brief Sends a request to a specific slot.
     *
     * Used when strict 1:1 thread-to-worker affinity is required.
     * Blocks until the slot is free and then until the response is received.
     *
     * @param slotIdx The index of the slot to use.
     * @param data Pointer to the request data.
     * @param size Size of the request data.
     * @param msgId The message ID (e.g., MSG_ID_NORMAL).
     * @param[out] outResp Vector to store the response data.
     * @return int Bytes read (response size), or -1 on error.
     */
    int SendToSlot(uint32_t slotIdx, const uint8_t* data, uint32_t size, uint32_t msgId, std::vector<uint8_t>& outResp) {
        if (!running || slotIdx >= numSlots) return -1;

        Slot* slot = &slots[slotIdx];

        // Claim specific slot
        int retries = 0;
        while(true) {
             uint32_t expected = SLOT_FREE;
             if (slot->header->state.compare_exchange_strong(expected, SLOT_BUSY, std::memory_order_acquire)) {
                 break;
             }
             Platform::CpuRelax();
             retries++;
             if (retries > 1000) {
                 Platform::ThreadYield();
                 retries = 0;
             }
        }

        return ProcessSlot(slot, data, size, msgId, outResp);
    }

    /**
     * @brief Sends a request using any available slot.
     *
     * Attempts to use a thread-local cached slot first, then searches for a free slot.
     *
     * @param data Pointer to the request data.
     * @param size Size of the request data.
     * @param msgId The message ID.
     * @param[out] outResp Vector to store the response data.
     * @return int Bytes read (response size), or -1 on error.
     */
    int Send(const uint8_t* data, uint32_t size, uint32_t msgId, std::vector<uint8_t>& outResp) {
        if (!running) return -1;

        static thread_local uint32_t cachedSlotIdx = 0xFFFFFFFF;
        Slot* slot = nullptr;

        // Fast Path: Try cached slot
        if (cachedSlotIdx < numSlots) {
            Slot& s = slots[cachedSlotIdx];
            uint32_t expected = SLOT_FREE;
            if (s.header->state.compare_exchange_strong(expected, SLOT_BUSY, std::memory_order_acquire)) {
                slot = &s;
            }
        }

        // Slow Path: Search
        if (!slot) {
            int retries = 0;
            uint32_t idx = nextSlot.fetch_add(1, std::memory_order_relaxed) % numSlots;

            while (true) {
                Slot& s = slots[idx];
                uint32_t expected = SLOT_FREE;
                if (s.header->state.compare_exchange_strong(expected, SLOT_BUSY, std::memory_order_acquire)) {
                    slot = &s;
                    cachedSlotIdx = idx; // Update Cache
                    break;
                }
                idx = (idx + 1) % numSlots;
                retries++;
                if (retries > (int)numSlots * 100) {
                    Platform::ThreadYield();
                    retries = 0;
                }
            }
        }

        return ProcessSlot(slot, data, size, msgId, outResp);
    }

private:
    int ProcessSlot(Slot* slot, const uint8_t* data, uint32_t size, uint32_t msgId, std::vector<uint8_t>& outResp) {
        // 2. Write Request
        if (size > slot->maxReqSize) size = slot->maxReqSize;
        if (data && size > 0) {
            memcpy(slot->reqBuffer, data, size);
        }
        slot->header->reqSize = size;
        slot->header->msgId = msgId;

        // Reset Host State
        slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);

        // 3. Signal Ready
        slot->header->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

        // Wake Guest if waiting
        if (slot->header->guestState.load(std::memory_order_seq_cst) == GUEST_STATE_WAITING) {
            Platform::SignalEvent(slot->hReqEvent);
        }

        // 4. Adaptive Wait for Response
        bool ready = false;
        int currentLimit = slot->spinLimit;
        const int MIN_SPIN = 100;
        const int MAX_SPIN = 20000;

        // Spin Phase
        for (int i = 0; i < currentLimit; ++i) {
            if (slot->header->state.load(std::memory_order_acquire) == SLOT_RESP_READY) {
                ready = true;
                break;
            }
            Platform::CpuRelax();
        }

        if (ready) {
            if (currentLimit < MAX_SPIN) currentLimit += 100;
        } else {
            if (currentLimit > MIN_SPIN) currentLimit -= 500;
            if (currentLimit < MIN_SPIN) currentLimit = MIN_SPIN;

            // Sleep Phase
            slot->header->hostState.store(HOST_STATE_WAITING, std::memory_order_seq_cst);

            // Double check
            if (slot->header->state.load(std::memory_order_acquire) == SLOT_RESP_READY) {
                ready = true;
                slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
            } else {
                // Wait
                 while (slot->header->state.load(std::memory_order_acquire) != SLOT_RESP_READY) {
                    Platform::WaitEvent(slot->hRespEvent, 100);
                 }
                 ready = true;
                 slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
            }
        }
        slot->spinLimit = currentLimit;

        // 5. Read Response
        int resultSize = 0;
        if (ready) {
            uint32_t respSize = slot->header->respSize;
            if (respSize > slot->maxRespSize) respSize = slot->maxRespSize;

            outResp.resize(respSize);
            if (respSize > 0) {
                memcpy(outResp.data(), slot->respBuffer, respSize);
            }
            resultSize = (int)respSize;
        }

        // 6. Release Slot
        slot->header->state.store(SLOT_FREE, std::memory_order_release);

        return resultSize;
    }
};

}
