#pragma once
#include <stdint.h>
#include <atomic>

namespace pingpong {

/**
 * @brief Name of the shared memory region for 1KB payload experiment.
 */
constexpr const char* SHM_NAME = "/pingpong_shm_1kb";

/**
 * @brief Size of the shared memory region.
 * 64KB is sufficient for ~60 threads with 1KB packets.
 */
constexpr size_t SHM_SIZE = 64 * 1024;

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
 * @brief Packet structure in shared memory with 1KB payload.
 * Represents the state and data for one worker thread.
 * Aligned to 1088 bytes (multiple of 64).
 */
struct Packet {
    std::atomic<uint32_t> state;          ///< Current state of the transaction
    uint32_t req_id;                      ///< Request ID
    std::atomic<uint32_t> host_sleeping;  ///< Flag: 1 if Host is sleeping
    std::atomic<uint32_t> guest_sleeping; ///< Flag: 1 if Guest is sleeping
    uint8_t data[1024];                   ///< 1024-byte Payload
    uint8_t padding[48];                  ///< Pad to 1088 bytes
};

static_assert(sizeof(Packet) == 1088, "Packet size mismatch");

}
