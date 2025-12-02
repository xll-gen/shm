#pragma once
#include <atomic>
#include <cstdint>

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
    // 1 if consumer is running, 0 if waiting/sleeping.
    std::atomic<uint32_t> consumerActive;
    uint8_t padding[44];
};

}
