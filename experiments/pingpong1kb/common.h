#pragma once
#include <stdint.h>
#include <atomic>

namespace pingpong {

constexpr const char* SHM_NAME = "/pingpong_shm_1kb";
constexpr size_t SHM_SIZE = 64 * 1024; // 64KB, enough for ~60 threads

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
    uint8_t data[1024];                   // 1024
    uint8_t padding[48];                  // Pad to 1088 bytes (multiple of 64)
};

static_assert(sizeof(Packet) == 1088, "Packet size mismatch");

}
