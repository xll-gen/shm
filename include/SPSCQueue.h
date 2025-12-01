#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include <cstring>
#include <thread>
#include "../src/Platform.h"

#ifdef _WIN32
#include <emmintrin.h> // _mm_pause
#else
#include <x86intrin.h> // __builtin_ia32_pause
#endif

namespace shm {

static const uint32_t BLOCK_MAGIC_DATA = 0xDA7A0001;
static const uint32_t BLOCK_MAGIC_PAD  = 0xDA7A0002;
static const uint32_t BLOCK_HEADER_SIZE = 8;

struct BlockHeader {
    uint32_t size;
    alignas(4) std::atomic<uint32_t> magic;
};

// Layout must match Go struct
struct QueueHeader {
    alignas(64) std::atomic<uint64_t> writePos;
    alignas(64) std::atomic<uint64_t> readPos;
    std::atomic<uint64_t> capacity;
    uint8_t padding[48];
};

class SPSCQueue {
public:
    QueueHeader* header;
    uint8_t* buffer;
    EventHandle hEvent; // Signaled when data is written (for Consumer)

    SPSCQueue(void* shmBase, uint64_t capacity, EventHandle eventHandle)
        : hEvent(eventHandle) {
        header = reinterpret_cast<QueueHeader*>(shmBase);
        buffer = reinterpret_cast<uint8_t*>(header) + sizeof(QueueHeader);
    }

    static size_t GetRequiredSize(uint64_t capacity) {
        return sizeof(QueueHeader) + capacity;
    }

    static void Init(void* shmBase, uint64_t capacity) {
        QueueHeader* h = new (shmBase) QueueHeader();
        h->writePos = 0;
        h->readPos = 0;
        h->capacity = capacity;
    }

    // Producer: Enqueue data
    // Blocks/Spins if full.
    void Enqueue(const void* data, uint32_t size) {
        uint32_t alignedSize = (size + 7) & ~7;
        uint32_t totalSize = alignedSize + BLOCK_HEADER_SIZE;
        uint64_t cap = header->capacity.load(std::memory_order_relaxed);

        while (true) {
            uint64_t wPos = header->writePos.load(std::memory_order_relaxed);
            uint64_t rPos = header->readPos.load(std::memory_order_acquire);

            uint64_t used = wPos - rPos;
            if (used + totalSize > cap) {
                // Full - Spin/Yield
#ifdef _WIN32
                _mm_pause();
#else
                __builtin_ia32_pause();
#endif
                std::this_thread::yield();
                continue;
            }

            uint64_t offset = wPos % cap;
            uint64_t spaceToEnd = cap - offset;

            // Check wrapping
            if (spaceToEnd < totalSize) {
                if (spaceToEnd < BLOCK_HEADER_SIZE) {
                    // Skip small tail
                    header->writePos.store(wPos + spaceToEnd, std::memory_order_release);
                    continue;
                } else {
                    // Write Padding
                    BlockHeader* bh = reinterpret_cast<BlockHeader*>(buffer + offset);
                    bh->size = (uint32_t)spaceToEnd - BLOCK_HEADER_SIZE;
                    bh->magic.store(BLOCK_MAGIC_PAD, std::memory_order_release);

                    header->writePos.store(wPos + spaceToEnd, std::memory_order_release);
                    continue;
                }
            }

            // Write Data
            BlockHeader* bh = reinterpret_cast<BlockHeader*>(buffer + offset);
            bh->size = size;
            memcpy(buffer + offset + BLOCK_HEADER_SIZE, data, size);

            bh->magic.store(BLOCK_MAGIC_DATA, std::memory_order_release);
            header->writePos.store(wPos + totalSize, std::memory_order_release);

            Platform::SignalEvent(hEvent);
            return;
        }
    }

    // Consumer: Dequeue data
    // Blocks if empty.
    void Dequeue(std::vector<uint8_t>& outBuffer) {
        int spinCount = 0;
        while (true) {
            uint64_t rPos = header->readPos.load(std::memory_order_relaxed);
            uint64_t wPos = header->writePos.load(std::memory_order_acquire);

            if (rPos == wPos) {
                // Empty - Hybrid Wait
                if (spinCount < 4000) {
                    spinCount++;
#ifdef _WIN32
                    _mm_pause();
#else
                    __builtin_ia32_pause();
#endif
                    continue;
                } else {
                    Platform::WaitEvent(hEvent, 100);
                    spinCount = 0;
                    continue;
                }
            }

            uint64_t cap = header->capacity.load(std::memory_order_relaxed);
            uint64_t offset = rPos % cap;
            uint64_t spaceToEnd = cap - offset;

            if (spaceToEnd < BLOCK_HEADER_SIZE) {
                 header->readPos.store(rPos + spaceToEnd, std::memory_order_release);
                 continue;
            }

            BlockHeader* bh = reinterpret_cast<BlockHeader*>(buffer + offset);

            uint32_t magic;
            int magicSpin = 0;
            while ((magic = bh->magic.load(std::memory_order_acquire)) == 0) {
#ifdef _WIN32
                 _mm_pause();
#else
                 __builtin_ia32_pause();
#endif
                 if (++magicSpin > 1000) std::this_thread::yield();
            }

            if (magic == BLOCK_MAGIC_PAD) {
                uint32_t size = bh->size;
                uint64_t nextRPosDiff = size + BLOCK_HEADER_SIZE;

                bh->magic.store(0, std::memory_order_relaxed);
                header->readPos.store(rPos + nextRPosDiff, std::memory_order_release);
                continue;
            }

            // DATA
            uint32_t size = bh->size;
            uint32_t alignedSize = (size + 7) & ~7;
            uint64_t nextRPosDiff = alignedSize + BLOCK_HEADER_SIZE;

            outBuffer.resize(size);
            memcpy(outBuffer.data(), buffer + offset + BLOCK_HEADER_SIZE, size);

            bh->magic.store(0, std::memory_order_relaxed);
            header->readPos.store(rPos + nextRPosDiff, std::memory_order_release);
            return;
        }
    }
};

}
