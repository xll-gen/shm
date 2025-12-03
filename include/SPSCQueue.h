#pragma once
#include <atomic>
#include <cstdint>
#include <vector>
#include <cstring>
#include <thread>
#include "../src/Platform.h"
#include "IPCUtils.h"

#ifdef _WIN32
#include <emmintrin.h> // _mm_pause
#else
#include <x86intrin.h> // __builtin_ia32_pause
#endif

namespace shm {

/**
 * @brief Single Producer Single Consumer (SPSC) Shared Memory Queue.
 *
 * Implements a lock-free ring buffer in shared memory.
 * Uses atomic head/tail pointers and magic numbers for synchronization.
 * Supports "Signal-If-Waiting" to minimize syscalls when the consumer is active.
 */
class SPSCQueue {
public:
    /** @brief Pointer to the queue header in shared memory. */
    QueueHeader* header;

    /** @brief Pointer to the data buffer area in shared memory. */
    uint8_t* buffer;

    /** @brief Event handle used to signal the consumer when data is written. */
    EventHandle hEvent;

    /**
     * @brief Constructs an SPSCQueue wrapper around an existing shared memory region.
     *
     * @param shmBase Pointer to the base of the shared memory region.
     * @param capacity Capacity of the queue in bytes.
     * @param eventHandle Handle to the synchronization event (Semaphore/Event).
     */
    SPSCQueue(void* shmBase, uint64_t capacity, EventHandle eventHandle)
        : hEvent(eventHandle) {
        header = reinterpret_cast<QueueHeader*>(shmBase);
        buffer = reinterpret_cast<uint8_t*>(header) + sizeof(QueueHeader);
    }

    /**
     * @brief Calculates the total shared memory size required for a given capacity.
     *
     * @param capacity Desired data capacity in bytes.
     * @return Total size including the QueueHeader.
     */
    static size_t GetRequiredSize(uint64_t capacity) {
        return sizeof(QueueHeader) + capacity;
    }

    /**
     * @brief Initializes a new shared memory region as an SPSC Queue.
     *
     * Sets up the QueueHeader with initial values.
     *
     * @param shmBase Pointer to the base of the shared memory region.
     * @param capacity Capacity of the queue in bytes.
     */
    static void Init(void* shmBase, uint64_t capacity) {
        QueueHeader* h = new (shmBase) QueueHeader();
        h->writePos = 0;
        h->readPos = 0;
        h->capacity = capacity;
        h->consumerActive = 0; // Default to Waiting (0)
    }

    /**
     * @brief Enqueues a single message into the queue.
     *
     * Blocks (spins/yields) if the queue is full.
     *
     * @param data Pointer to the data to enqueue.
     * @param size Size of the data in bytes.
     * @param msgId Optional message ID (default 0).
     */
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
                    bh->msgId = 0;
                    bh->magic.store(BLOCK_MAGIC_PAD, std::memory_order_relaxed);

                    header->writePos.store(wPos + spaceToEnd, std::memory_order_release);
                    continue;
                }
            }

            // Write Data
            BlockHeader* bh = reinterpret_cast<BlockHeader*>(buffer + offset);
            bh->size = size;
            bh->msgId = msgId;
            bh->magic.store(BLOCK_MAGIC_DATA, std::memory_order_relaxed);
            if (size > 0 && data != nullptr) {
                memcpy(buffer + offset + BLOCK_HEADER_SIZE, data, size);
            }

            header->writePos.store(wPos + totalSize, std::memory_order_release);

            // Ensure Store(WritePos) is visible before Load(ConsumerActive)
            std::atomic_thread_fence(std::memory_order_seq_cst);

            if (header->consumerActive.load(std::memory_order_relaxed) == 0) {
                Platform::SignalEvent(hEvent);
            }
            return;
        }
    }

    /**
     * @brief Enqueues a batch of messages into the queue.
     *
     * Efficiently writes multiple messages, reducing synchronization overhead.
     *
     * @param msgs Vector of byte vectors to enqueue.
     */
    void EnqueueBatch(const std::vector<std::vector<uint8_t>>& msgs) {
        if (msgs.empty()) return;

        uint64_t cap = header->capacity.load(std::memory_order_relaxed);

        size_t currentIdx = 0;
        while (currentIdx < msgs.size()) {
            uint64_t wPos = header->writePos.load(std::memory_order_relaxed);
            uint64_t rPos = header->readPos.load(std::memory_order_acquire);

            uint64_t tempWPos = wPos;
            bool writtenAny = false;

            for (; currentIdx < msgs.size(); ++currentIdx) {
                const auto& data = msgs[currentIdx];
                uint32_t size = (uint32_t)data.size();
                uint32_t alignedSize = (size + 7) & ~7;
                uint32_t totalSize = alignedSize + BLOCK_HEADER_SIZE;

                // Check capacity
                if ((tempWPos - rPos) + totalSize > cap) {
                    if (!writtenAny) {
                        // Full - Spin/Yield
#ifdef _WIN32
                        _mm_pause();
#else
                        __builtin_ia32_pause();
#endif
                        std::this_thread::yield();
                        // Refresh rPos
                        rPos = header->readPos.load(std::memory_order_acquire);
                        currentIdx--;
                        break;
                    } else {
                        break;
                    }
                }

                uint64_t offset = tempWPos % cap;
                uint64_t spaceToEnd = cap - offset;

                // Check wrapping
                if (spaceToEnd < totalSize) {
                    if (spaceToEnd < BLOCK_HEADER_SIZE) {
                         tempWPos += spaceToEnd;
                         currentIdx--;
                         continue;
                    } else {
                        // Padding
                        BlockHeader* bh = reinterpret_cast<BlockHeader*>(buffer + offset);
                        bh->size = (uint32_t)spaceToEnd - BLOCK_HEADER_SIZE;
                        bh->msgId = 0;
                        bh->magic.store(BLOCK_MAGIC_PAD, std::memory_order_relaxed);

                        tempWPos += spaceToEnd;
                        currentIdx--;
                        continue;
                    }
                }

                // Write Data
                BlockHeader* bh = reinterpret_cast<BlockHeader*>(buffer + offset);
                bh->size = size;
                bh->msgId = 0; // Batch implies normal data
                bh->magic.store(BLOCK_MAGIC_DATA, std::memory_order_relaxed);
                memcpy(buffer + offset + BLOCK_HEADER_SIZE, data.data(), size);

                tempWPos += totalSize;
                writtenAny = true;
            }

            if (tempWPos != wPos) {
                 header->writePos.store(tempWPos, std::memory_order_release);

                 // Ensure Store(WritePos) is visible before Load(ConsumerActive)
                 std::atomic_thread_fence(std::memory_order_seq_cst);

                 if (header->consumerActive.load(std::memory_order_relaxed) == 0) {
                     Platform::SignalEvent(hEvent);
                 }
            }
        }
    }

    /**
     * @brief Dequeues a single message from the queue.
     *
     * Blocks if the queue is empty. Uses an adaptive strategy (spin -> yield -> sleep).
     *
     * @param outBuffer Vector to be populated with the message data. Resized automatically.
     * @param running Optional atomic boolean pointer. If provided and becomes false, the wait is aborted.
     * @return The Message ID of the dequeued item, or 0xFFFFFFFF if aborted/shutdown.
     */
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
            // No spin on magic. Just read.
            uint32_t magic = bh->magic.load(std::memory_order_relaxed);

            if (magic == BLOCK_MAGIC_PAD) {
                uint32_t size = bh->size;
                uint64_t nextRPosDiff = size + BLOCK_HEADER_SIZE;

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

            header->readPos.store(rPos + nextRPosDiff, std::memory_order_release);
            return msgId;
        }
    }

    /**
     * @brief Dequeues a batch of messages.
     *
     * Reads up to `maxCount` messages into `outMsgs`.
     *
     * @param outMsgs Vector of vectors to be populated with messages.
     * @param maxCount Maximum number of messages to dequeue.
     */
    void DequeueBatch(std::vector<std::vector<uint8_t>>& outMsgs, size_t maxCount) {
        if (maxCount == 0) return;

        // Set Active = 1 when running
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
                uint32_t magic = bh->magic.load(std::memory_order_relaxed);

                if (magic == BLOCK_MAGIC_PAD) {
                     uint32_t size = bh->size;
                     rPos += size + BLOCK_HEADER_SIZE;
                     continue;
                }

                // DATA
                uint32_t size = bh->size;
                // uint32_t msgId = bh->msgId; // Ignored for batch
                uint32_t alignedSize = (size + 7) & ~7;
                uint64_t nextRPosDiff = alignedSize + BLOCK_HEADER_SIZE;

                std::vector<uint8_t> msg(size);
                if (size > 0) {
                    memcpy(msg.data(), buffer + offset + BLOCK_HEADER_SIZE, size);
                }
                outMsgs.push_back(std::move(msg));

                rPos += nextRPosDiff;
            }

            header->readPos.store(rPos, std::memory_order_release);
            return;
        }
    }
};

}
