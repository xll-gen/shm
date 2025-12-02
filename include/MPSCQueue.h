#pragma once
#include <atomic>
#include <cstdint>
#include <thread>
#include <cstring> // memcpy
#include <vector>
#include "../src/Platform.h"
#include "IPCUtils.h"

#ifdef _WIN32
#include <emmintrin.h> // _mm_pause
#else
#include <x86intrin.h> // __builtin_ia32_pause
#endif

// Lock-Free MPSC Queue (Multi-Producer Single-Consumer)
// Designed for Shared Memory IPC.
//
// Layout matches SPSCQueue for compatibility.
//
// Protocol:
// 1. Reserve: Writer increments writePos using CAS.
// 2. Check: If wrap-around needed, writer marks remaining space as PADDING and tries again from 0.
// 3. Commit: Writer writes data, then writes BLOCK_MAGIC.
// 4. Reader: Reads writePos. If > readPos, calculates offset.
//    Checks BlockHeader magic.
//    If magic is 0 (not written yet), spins/waits.
//    If magic is VALID, processes.
//    If magic is PADDING, skips.

namespace shm {

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
        h->consumerActive = 0;
    }

    // Producer: Enqueue data
    // Returns false if full (Reader too slow) - actually we spin/retry like SPSC
    void Enqueue(const void* data, uint32_t size, uint32_t msgId = 0) {
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

            // Check if wrapping is needed
            if (spaceToEnd < totalSize) {
                // Not enough space at end. We must wrap.

                // If space is too small even for a header, simply skip it.
                if (spaceToEnd < BLOCK_HEADER_SIZE) {
                    // Try to reserve these bytes
                    if (header->writePos.compare_exchange_weak(wPos, wPos + spaceToEnd, std::memory_order_relaxed)) {
                        continue; // Successfully reserved tail, retry from 0 (loop will recalc offset)
                    }
                } else {
                    // Try to reserve [wPos, wPos + spaceToEnd] for PAD
                    if (header->writePos.compare_exchange_weak(wPos, wPos + spaceToEnd, std::memory_order_relaxed)) {
                        // Won the race. Write Padding.
                        BlockHeader* bh = reinterpret_cast<BlockHeader*>(buffer + offset);
                        bh->size = (uint32_t)spaceToEnd - BLOCK_HEADER_SIZE;
                        bh->msgId = 0;
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
                    bh->size = size;
                    bh->msgId = msgId;
                    // Magic must be last.
                    // Initialize magic to 0 first? No, we own this slot now.
                    // But checking magic is done by consumer.
                    // Ideally we ensure it was 0? It should be 0 from previous consumption or init.
                    // Actually consumer clears it.

                    if (size > 0 && data != nullptr) {
                        memcpy(buffer + offset + BLOCK_HEADER_SIZE, data, size);
                    }

                    // Commit (Release)
                    bh->magic.store(BLOCK_MAGIC_DATA, std::memory_order_release);

                    // Ensure Store(Magic) is visible
                    // And checking consumerActive
                    std::atomic_thread_fence(std::memory_order_seq_cst);

                    if (header->consumerActive.load(std::memory_order_relaxed) == 0) {
                        Platform::SignalEvent(hEvent);
                    }

                    return;
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

    // Producer: Enqueue Batch
    void EnqueueBatch(const std::vector<std::vector<uint8_t>>& msgs) {
        if (msgs.empty()) return;

        // MPSC batching is tricky because we can't easily reserve a huge block if we wrap.
        // Simple approach: Enqueue one by one.
        // Optimized approach: Try to reserve N messages at once if they fit.
        // For simplicity and correctness in lock-free: Enqueue one by one.
        // It is still "lock-free", just multiple CAS operations.
        // Since this is MPSC, we compete with other threads.

        for (const auto& data : msgs) {
            Enqueue(data.data(), (uint32_t)data.size(), 0);
        }
    }

    // Consumer: Dequeue data
    // Blocks if empty.
    // Returns msgId.
    // If running is provided and becomes false, returns 0xFFFFFFFF.
    uint32_t Dequeue(std::vector<uint8_t>& outBuffer, std::atomic<bool>* running = nullptr) {
        // Set Active = 1 when running
        header->consumerActive.store(1, std::memory_order_relaxed);
        int spinCount = 0;

        while (true) {
            uint64_t rPos = header->readPos.load(std::memory_order_relaxed);
            uint64_t wPos = header->writePos.load(std::memory_order_acquire);

            if (rPos == wPos) {
                // Empty
                if (spinCount < 4000) {
                    spinCount++;
#ifdef _WIN32
                    _mm_pause();
#else
                    __builtin_ia32_pause();
#endif
                    continue;
                } else {
                    if (running && !running->load(std::memory_order_relaxed)) return 0xFFFFFFFF;

                    header->consumerActive.store(0, std::memory_order_relaxed); // Waiting
                    // Double check
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    if (header->readPos.load(std::memory_order_relaxed) != header->writePos.load(std::memory_order_acquire)) {
                        header->consumerActive.store(1, std::memory_order_relaxed); // Active
                        continue;
                    }

                    Platform::WaitEvent(hEvent, 100);

                    if (running && !running->load(std::memory_order_relaxed)) return 0xFFFFFFFF;

                    header->consumerActive.store(1, std::memory_order_relaxed); // Active
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

            // Spin on Magic
            // In MPSC, writePos is updated BEFORE magic is set.
            // So we might see writePos updated, but magic is still 0 (or old garbage if not cleared properly, but we clear it).
            // We must clear magic after reading.

            int magicSpin = 0;
            uint32_t magic;
            while ((magic = bh->magic.load(std::memory_order_acquire)) == 0) {
#ifdef _WIN32
                 _mm_pause();
#else
                 __builtin_ia32_pause();
#endif
                 if (++magicSpin > 10000) {
                     std::this_thread::yield();
                     if (running && !running->load(std::memory_order_relaxed)) return 0xFFFFFFFF;
                 }
            }

            if (magic == BLOCK_MAGIC_PAD) {
                uint32_t size = bh->size;
                uint64_t nextRPosDiff = size + BLOCK_HEADER_SIZE;

                // Clear magic for next round
                bh->magic.store(0, std::memory_order_relaxed);

                header->readPos.store(rPos + nextRPosDiff, std::memory_order_release);
                continue;
            }

            // DATA
            uint32_t size = bh->size;
            uint32_t msgId = bh->msgId;
            uint32_t alignedSize = (size + 7) & ~7;
            uint64_t nextRPosDiff = alignedSize + BLOCK_HEADER_SIZE;

            outBuffer.resize(size);
            if (size > 0) {
                memcpy(outBuffer.data(), buffer + offset + BLOCK_HEADER_SIZE, size);
            }

            // Clear magic
            bh->magic.store(0, std::memory_order_relaxed);

            header->readPos.store(rPos + nextRPosDiff, std::memory_order_release);
            return msgId;
        }
    }

    // Consumer: Dequeue Batch
    void DequeueBatch(std::vector<std::vector<uint8_t>>& outMsgs, size_t maxCount) {
        if (maxCount == 0) return;

        header->consumerActive.store(1, std::memory_order_relaxed);
        int spinCount = 0;

        while (true) {
            uint64_t rPos = header->readPos.load(std::memory_order_relaxed);
            uint64_t wPos = header->writePos.load(std::memory_order_acquire);

            if (rPos == wPos) {
                if (!outMsgs.empty()) return;

                if (spinCount < 4000) {
                    spinCount++;
#ifdef _WIN32
                    _mm_pause();
#else
                    __builtin_ia32_pause();
#endif
                    continue;
                } else {
                    header->consumerActive.store(0, std::memory_order_relaxed); // Waiting
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    if (header->readPos.load(std::memory_order_relaxed) != header->writePos.load(std::memory_order_acquire)) {
                        header->consumerActive.store(1, std::memory_order_relaxed); // Active
                        continue;
                    }

                    Platform::WaitEvent(hEvent, 100);
                    header->consumerActive.store(1, std::memory_order_relaxed); // Active
                    spinCount = 0;
                    continue;
                }
            }

            uint64_t cap = header->capacity.load(std::memory_order_relaxed);

            while (outMsgs.size() < maxCount && rPos != wPos) {
                uint64_t offset = rPos % cap;
                uint64_t spaceToEnd = cap - offset;

                if (spaceToEnd < BLOCK_HEADER_SIZE) {
                    rPos += spaceToEnd;
                    continue;
                }

                BlockHeader* bh = reinterpret_cast<BlockHeader*>(buffer + offset);

                // Spin on Magic
                int magicSpin = 0;
                uint32_t magic;
                while ((magic = bh->magic.load(std::memory_order_acquire)) == 0) {
#ifdef _WIN32
                     _mm_pause();
#else
                     __builtin_ia32_pause();
#endif
                     if (++magicSpin > 10000) {
                         std::this_thread::yield();
                         // We can't easily exit here without a running flag, so we just spin.
                         // But for batch, we might assume the host is alive.
                     }
                }

                if (magic == BLOCK_MAGIC_PAD) {
                     uint32_t size = bh->size;
                     // Clear Magic
                     bh->magic.store(0, std::memory_order_relaxed);
                     rPos += size + BLOCK_HEADER_SIZE;
                     continue;
                }

                // DATA
                uint32_t size = bh->size;
                uint32_t alignedSize = (size + 7) & ~7;
                uint64_t nextRPosDiff = alignedSize + BLOCK_HEADER_SIZE;

                std::vector<uint8_t> msg(size);
                if (size > 0) {
                    memcpy(msg.data(), buffer + offset + BLOCK_HEADER_SIZE, size);
                }
                outMsgs.push_back(std::move(msg));

                // Clear Magic
                bh->magic.store(0, std::memory_order_relaxed);

                rPos += nextRPosDiff;
            }

            header->readPos.store(rPos, std::memory_order_release);
            return;
        }
    }
};

}
