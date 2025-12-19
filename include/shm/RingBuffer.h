#pragma once

#include "DirectHost.h"
#include "RingBufferUtils.h"
#include <thread>
#include <chrono>

namespace shm {

class RingBufferSender {
    DirectHost* host;
    int32_t slotIdx;
    uint8_t* basePtr;
    uint32_t capacity;
    RingBufferHeader* header;
    bool initialized;

public:
    RingBufferSender(DirectHost* h, int32_t sIdx) : host(h), slotIdx(sIdx), basePtr(nullptr), capacity(0), header(nullptr), initialized(false) {}

    ~RingBufferSender() {
        if (initialized) {
            // Try to reset slot state if needed, but usually we leave it.
            // Host destructor cleans up.
        }
    }

    /**
     * @brief Initializes the Ring Buffer in the specified slot.
     * Changes MsgType to RING_BUFFER (app specific in this case, say 200).
     */
    Result<void> Init() {
        if (!host) return Result<void>::Failure(Error::InvalidArgs);

        // Acquire the slot first to ensure exclusive access
        // We actually need to "Take Over" the slot.
        // If we use AcquireSpecificSlot or just SendToSlot logic.
        // But we want to set it up once and keep it.

        // For simplicity, we assume the user has conceptually reserved this slot.
        // We will assert control.

        // 1. Get Buffer Pointers
        uint8_t* reqBuf = host->GetReqBuffer(slotIdx);
        int32_t maxReq = host->GetMaxReqSize(slotIdx);
        // Note: Slot is Req + Resp. In RingBuffer mode, we use the WHOLE payload space?
        // Or just Req? Standard slots split Req/Resp.
        // If we want "Two Buffers in Single Slot", we effectively unify them or use them as a continuous space.
        // Let's use the ReqBuffer space for now to be safe with existing offsets.
        // But the user wants "Double Buffering", i.e. more space?
        // Actually, Ring Buffer utilizes the space continuously.

        // Let's assume we use the ReqBuffer space.
        if (maxReq < sizeof(RingBufferHeader) + 1024) {
             return Result<void>::Failure(Error::BufferTooSmall);
        }

        header = reinterpret_cast<RingBufferHeader*>(reqBuf);
        basePtr = reqBuf + sizeof(RingBufferHeader);
        capacity = maxReq - sizeof(RingBufferHeader);

        // Reset Header
        header->writeOffset.store(0);
        header->readOffset.store(0);

        initialized = true;
        return Result<void>::Success();
    }

    /**
     * @brief Writes data to the ring buffer. Blocks if full.
     */
    Result<void> Write(const void* data, uint32_t size) {
        if (!initialized) return Result<void>::Failure(Error::InvalidArgs);

        const uint8_t* src = (const uint8_t*)data;
        uint32_t remaining = size;

        while (remaining > 0) {
            uint64_t w = header->writeOffset.load(std::memory_order_relaxed);
            uint64_t r = header->readOffset.load(std::memory_order_acquire);

            uint64_t usage = w - r;
            if (usage >= capacity) {
                // Full. Wait.
                // In a real impl, we should use `_mm_pause` or `YieldProcessor`.
                // For now, simple yield.
                std::this_thread::yield();
                continue;
            }

            uint32_t available = capacity - (uint32_t)usage;
            uint32_t toWrite = (remaining < available) ? remaining : available;

            // Circular Write
            uint32_t writeIdx = w % capacity;
            uint32_t contiguous = capacity - writeIdx;
            uint32_t chunk = (toWrite < contiguous) ? toWrite : contiguous;

            memcpy(basePtr + writeIdx, src, chunk);
            if (chunk < toWrite) {
                memcpy(basePtr, src + chunk, toWrite - chunk);
            }

            // Commit write
            header->writeOffset.store(w + toWrite, std::memory_order_release);

            // Signal Guest
            // We use the standard SignalEvent from Host.
            // But we need to make sure the Guest is listening.
            // In standard "DirectHost", Guest waits on "Slot N Req".
            // We reuse that event.
            host->SignalSlot(slotIdx);

            src += toWrite;
            remaining -= toWrite;
        }

        return Result<void>::Success();
    }
};

}
