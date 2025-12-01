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
static const uint32_t BLOCK_HEADER_SIZE = 16;

struct BlockHeader {
    uint32_t size;
    uint32_t msgId;
    alignas(4) std::atomic<uint32_t> magic;
    uint32_t padding;
};

// Layout must match Go struct
struct QueueHeader {
    alignas(64) std::atomic<uint64_t> writePos;
    alignas(64) std::atomic<uint64_t> readPos;
    std::atomic<uint64_t> capacity;
    // Added for signal optimization: 1 if consumer is waiting/sleeping, 0 otherwise
    std::atomic<uint32_t> consumerWaiting;
    uint8_t padding[44]; // Reduced padding by 4 bytes to keep 128 byte alignment if needed, or 48?
    // originally 56 bytes pad1, then 44 bytes pad2.
    // wait, let's calculate:
    // wPos (8), rPos (8), cap (8), consumerWaiting (4) = 28 bytes.
    // 128 bytes total size.
    // 128 - 28 = 100 bytes padding.
    // The struct has explicit alignment on wPos and rPos though.
    // wPos @ 0.
    // rPos @ 64.
    // So wPos (8) + pad1 (56) = 64.
    // rPos (8) + cap (8) + consumerWaiting (4) + pad2 (?) = 64.
    // 8 + 8 + 4 = 20.
    // 64 - 20 = 44.
    // So padding is 44 bytes. Correct.
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
        h->consumerWaiting = 0;
    }

    // Producer: Enqueue data
    // Blocks/Spins if full.
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

            // Ensure Store(WritePos) is visible before Load(ConsumerWaiting)
            std::atomic_thread_fence(std::memory_order_seq_cst);

            if (header->consumerWaiting.load(std::memory_order_relaxed) == 1) {
                Platform::SignalEvent(hEvent);
            }
            return;
        }
    }

    // Producer: Enqueue Batch
    // Only supports normal messages (msgId=0) implicitly for now, or we need to change signature.
    // Assuming batch is for data payload.
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

                 // Ensure Store(WritePos) is visible before Load(ConsumerWaiting)
                 std::atomic_thread_fence(std::memory_order_seq_cst);

                 if (header->consumerWaiting.load(std::memory_order_relaxed) == 1) {
                     Platform::SignalEvent(hEvent);
                 }
            }
        }
    }

    // Consumer: Dequeue data
    // Blocks if empty.
    // Returns msgId.
    uint32_t Dequeue(std::vector<uint8_t>& outBuffer) {
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
                    header->consumerWaiting.store(1, std::memory_order_relaxed);
                    // Double check
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    if (header->readPos.load(std::memory_order_relaxed) != header->writePos.load(std::memory_order_acquire)) {
                        header->consumerWaiting.store(0, std::memory_order_relaxed);
                        continue;
                    }

                    Platform::WaitEvent(hEvent, 100);
                    header->consumerWaiting.store(0, std::memory_order_relaxed);
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

                // Optional: Clear magic? Not needed for protocol, but maybe for debugging?
                // bh->magic.store(0, std::memory_order_relaxed);

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

    // Consumer: Dequeue Batch
    void DequeueBatch(std::vector<std::vector<uint8_t>>& outMsgs, size_t maxCount) {
        if (maxCount == 0) return;

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
                    header->consumerWaiting.store(1, std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    if (header->readPos.load(std::memory_order_relaxed) != header->writePos.load(std::memory_order_acquire)) {
                        header->consumerWaiting.store(0, std::memory_order_relaxed);
                        continue;
                    }

                    Platform::WaitEvent(hEvent, 100);
                    header->consumerWaiting.store(0, std::memory_order_relaxed);
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
