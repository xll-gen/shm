#pragma once
#include <stdint.h>
#include <atomic>

namespace pingpong {

constexpr const char* SHM_NAME = "/pingpong_shm";
constexpr size_t SHM_SIZE = 4096 * 4; // Increase size to be safe for multiple threads

enum State : uint32_t {
    STATE_WAIT_REQ = 0,
    STATE_REQ_READY = 1,
    STATE_RESP_READY = 2,
    STATE_DONE = 3
};

struct Packet {
    std::atomic<uint32_t> state;          // 4
    uint32_t req_id;                      // 4
    std::atomic<uint32_t> host_sleeping;  // 4
    std::atomic<uint32_t> guest_sleeping; // 4
    int64_t val_a;                        // 8
    int64_t val_b;                        // 8
    int64_t sum;                          // 8
    uint8_t padding[24];                  // Pad to 64 bytes
};

static_assert(sizeof(Packet) == 64, "Packet size mismatch");

}
