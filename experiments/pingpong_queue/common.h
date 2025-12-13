#pragma once
#include <stdint.h>
#include <atomic>

namespace pingpong_queue {

/**
 * @brief Name of the shared memory region.
 */
constexpr const char* SHM_NAME = "/pingpong_queue_shm";

/**
 * @brief Queue capacity (must be power of 2).
 * Small size for strict PingPong to keep it hot in cache.
 */
constexpr size_t QUEUE_CAPACITY = 64;
constexpr size_t QUEUE_MASK = QUEUE_CAPACITY - 1;

/**
 * @brief Data payload.
 */
struct Message {
    uint32_t id;
    int64_t val_a;
    int64_t val_b;
    int64_t sum;
};

/**
 * @brief Cache-line aligned structure to prevent false sharing.
 */
struct alignas(128) CacheLineInt {
    std::atomic<uint64_t> val;
};

/**
 * @brief SPSC Ring Buffer Layout.
 * Head and Tail are separated by padding to avoid False Sharing.
 */
struct RingBuffer {
    alignas(128) std::atomic<uint64_t> head; // Written by Producer, Read by Consumer
    alignas(128) std::atomic<uint64_t> tail; // Written by Consumer, Read by Producer

    // Data array starts on a new cache line
    alignas(128) Message data[QUEUE_CAPACITY];
};

/**
 * @brief Total Shared Memory Layout.
 * Contains two queues: Host->Guest and Guest->Host.
 */
struct SharedMemory {
    RingBuffer to_guest; // Host produces, Guest consumes
    RingBuffer to_host;  // Guest produces, Host consumes
};

}
