#pragma once
#include <atomic>
#include <cstdint>
#include <thread>
#include <cstring> // memcpy
#include <vector>
#include "../src/Platform.h"

#ifdef _WIN32
#include <emmintrin.h> // _mm_pause
#else
#include <x86intrin.h> // __builtin_ia32_pause
#endif

// Lock-Free MPSC Queue (Multi-Producer Single-Consumer)
// Designed for Shared Memory IPC.
//
// Layout:
// [Header: writePos, readPos]
// [Data Buffer...]
//
// Blocks have a header: { size (32bit), magic (32bit) }
//
// Protocol:
// 1. Reserve: Writer increments writePos.
// 2. Check: If wrap-around needed, writer marks remaining space as PADDING and tries again from 0.
// 3. Commit: Writer writes data, then writes BLOCK_MAGIC.
// 4. Reader: Reads writePos, if > readPos, reads block header.
//    If header magic is valid, processes. If padding, skips.
//    If header magic is 0 (not written yet), spins/waits.

namespace shm {

static const uint32_t BLOCK_MAGIC_DATA = 0xDA7A0001;
static const uint32_t BLOCK_MAGIC_PAD  = 0xDA7A0002;
static const uint32_t BLOCK_HEADER_SIZE = 8; // sizeof(BlockHeader)

struct BlockHeader {
    uint32_t size;  // Payload size (excluding header)
    alignas(4) std::atomic<uint32_t> magic; // 0=Writing, MAGIC=Ready
};

struct QueueHeader {
    alignas(64) std::atomic<uint64_t> writePos;
    alignas(64) std::atomic<uint64_t> readPos;
    std::atomic<uint64_t> capacity;
    uint8_t padding[48];
};

class MPSCQueue {
public:
    QueueHeader* header;
    uint8_t* buffer;
    EventHandle hEvent; // Kernel Event for Consumer

    MPSCQueue(void* shmBase, uint64_t capacity, EventHandle eventHandle)
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
    // Returns false if full (Reader too slow)
    bool Enqueue(const void* data, uint32_t size) {
        // Align total size to 8 bytes to ensure subsequent headers are aligned
        uint32_t alignedSize = (size + 7) & ~7;
        uint32_t totalSize = alignedSize + BLOCK_HEADER_SIZE;
        uint64_t cap = header->capacity.load(std::memory_order_acquire);

        // CAS Loop
        while (true) {
            uint64_t wPos = header->writePos.load(std::memory_order_relaxed);
            uint64_t rPos = header->readPos.load(std::memory_order_acquire); // Acquire to see Reader updates

            uint64_t used = wPos - rPos;
            if (used + totalSize > cap) {
                // Full
                return false;
            }

            uint64_t offset = wPos % cap;
            uint64_t spaceToEnd = cap - offset;

            // Check if wrapping is needed
            if (spaceToEnd < totalSize) {
                // Not enough space at end. We must wrap.

                // If space is too small even for a header, simply skip it.
                if (spaceToEnd < BLOCK_HEADER_SIZE) {
                    if (header->writePos.compare_exchange_weak(wPos, wPos + spaceToEnd, std::memory_order_relaxed)) {
                        continue; // Successfully skipped tail, retry from 0
                    }
                } else {
                    // Try to reserve [wPos, wPos + spaceToEnd] for PAD
                    if (header->writePos.compare_exchange_weak(wPos, wPos + spaceToEnd, std::memory_order_relaxed)) {
                        // Won the race. Write Padding.
                        BlockHeader* bh = reinterpret_cast<BlockHeader*>(buffer + offset);
                        bh->size = (uint32_t)spaceToEnd - BLOCK_HEADER_SIZE;
                        bh->magic.store(BLOCK_MAGIC_PAD, std::memory_order_release);

                        // Loop again to try writing the actual data at the beginning
                        continue;
                    }
                }
            } else {
                // Fits continuously
                if (header->writePos.compare_exchange_weak(wPos, wPos + totalSize, std::memory_order_relaxed)) {
                    // Won the race. Write Data.
                    BlockHeader* bh = reinterpret_cast<BlockHeader*>(buffer + offset);
                    bh->size = size; // Store actual size, not aligned

                    // Copy data
                    memcpy(buffer + offset + BLOCK_HEADER_SIZE, data, size);

                    // Commit (Release)
                    bh->magic.store(BLOCK_MAGIC_DATA, std::memory_order_release);

                    // Signal Consumer (Optimistic: always signal for now, or if queue was empty)
                    Platform::SignalEvent(hEvent);

                    return true;
                }
            }
            // CAS failed, retry
#ifdef _WIN32
             _mm_pause();
#else
             __builtin_ia32_pause();
#endif
        }
    }

    // Consumer: Dequeue data
    // Returns true if data read, false if empty
    // On success, fills outBuffer. outSize updated with actual size.
    bool Dequeue(std::vector<uint8_t>& outBuffer) {
        uint64_t rPos = header->readPos.load(std::memory_order_relaxed);
        uint64_t wPos = header->writePos.load(std::memory_order_acquire);

        if (rPos == wPos) return false; // Empty

        uint64_t cap = header->capacity.load(std::memory_order_acquire);
        uint64_t offset = rPos % cap;
        uint64_t spaceToEnd = cap - offset;

        if (spaceToEnd < BLOCK_HEADER_SIZE) {
            // Small tail skip (Writer skipped this too)
            header->readPos.store(rPos + spaceToEnd, std::memory_order_release);
            return Dequeue(outBuffer);
        }

        BlockHeader* bh = reinterpret_cast<BlockHeader*>(buffer + offset);

        // Spin/Wait for Magic (Writer might have reserved but not finished writing)
        int spin = 0;
        uint32_t magic;
        while ((magic = bh->magic.load(std::memory_order_acquire)) == 0) {
#ifdef _WIN32
             _mm_pause();
#else
             __builtin_ia32_pause();
#endif
            if (++spin > 1000) {
                 Platform::Yield();
            }
        }

        uint32_t size = bh->size;
        // Re-calculate total block size used by Writer
        // Writer used aligned size for reservation
        uint32_t alignedSize = (size + 7) & ~7;
        uint32_t totalBlockSize = alignedSize + BLOCK_HEADER_SIZE;

        uint64_t nextRPosDiff;

        if (magic == BLOCK_MAGIC_PAD) {
            // PAD block fills exactly the rest of the buffer
            nextRPosDiff = size + BLOCK_HEADER_SIZE;

            // Clear magic
            bh->magic.store(0, std::memory_order_relaxed);
            header->readPos.store(rPos + nextRPosDiff, std::memory_order_release);

            // Recurse
            return Dequeue(outBuffer);
        }
        else {
            // DATA block
            nextRPosDiff = totalBlockSize; // Aligned logic

            // Real Data
            if (outBuffer.size() < size) outBuffer.resize(size);
            memcpy(outBuffer.data(), buffer + offset + BLOCK_HEADER_SIZE, size);

            // Clear Magic
            bh->magic.store(0, std::memory_order_relaxed);

            // Advance ReadPos
            header->readPos.store(rPos + nextRPosDiff, std::memory_order_release);
            return true;
        }
    }
};

}
