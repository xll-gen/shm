#pragma once
#include <stdint.h>
#include <atomic>

namespace pingpong {

/**
 * @brief Name of the shared memory region.
 */
constexpr const char* SHM_NAME = "/pingpong_opt_shm";

/**
 * @brief Size of the shared memory region.
 * Sufficient for multiple threads/packets.
 */
constexpr size_t SHM_SIZE = 4096 * 4;

/**
 * @brief States for the synchronization state machine.
 */
enum State : uint32_t {
    STATE_WAIT_REQ = 0,   ///< Waiting for request from Host
    STATE_REQ_READY = 1,  ///< Request ready for Guest
    STATE_RESP_READY = 2, ///< Response ready for Host
    STATE_DONE = 3        ///< Benchmark complete
};

/**
 * @brief Packet structure in shared memory.
 * Represents the state and data for one worker thread.
 * Aligned to 128 bytes (double cache line) to prevent false sharing and prefetcher contention.
 */
struct Packet {
    std::atomic<uint32_t> state;          ///< Current state of the transaction
    uint32_t req_id;                      ///< Request ID
    std::atomic<uint32_t> host_sleeping;  ///< Flag: 1 if Host is sleeping
    std::atomic<uint32_t> guest_sleeping; ///< Flag: 1 if Guest is sleeping
    int64_t val_a;                        ///< Payload A
    int64_t val_b;                        ///< Payload B
    int64_t sum;                          ///< Result Sum
    uint8_t padding[88];                  ///< Pad to 128 bytes
};

static_assert(sizeof(Packet) == 128, "Packet size mismatch");

}
