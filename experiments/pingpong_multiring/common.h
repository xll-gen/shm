#pragma once
#include <stdint.h>
#include <atomic>

namespace pingpong_multiring {

constexpr const char* SHM_NAME = "/pingpong_multiring_shm";
constexpr size_t QUEUE_CAPACITY = 64;
constexpr size_t QUEUE_MASK = QUEUE_CAPACITY - 1;
constexpr int MAX_THREADS = 16; // Support up to 16 threads

struct Message {
    uint32_t id;
    int64_t val_a;
    int64_t val_b;
    int64_t sum;
};

// 128-byte cache line alignment
struct alignas(128) CacheLineInt {
    std::atomic<uint64_t> val;
};

// Size: 128 + 128 + (64 * 32) = 256 + 2048 = 2304 bytes.
// 2304 % 128 == 0. Safe for array packing without false sharing between indices.
struct RingBuffer {
    alignas(128) std::atomic<uint64_t> head;
    alignas(128) std::atomic<uint64_t> tail;
    alignas(128) Message data[QUEUE_CAPACITY];
};

struct SharedMemory {
    RingBuffer to_guest[MAX_THREADS];
    RingBuffer to_host[MAX_THREADS];
};

}
