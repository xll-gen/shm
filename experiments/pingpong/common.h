#pragma once
#include <stdint.h>
#include <atomic>

namespace pingpong {

constexpr const char* SHM_NAME = "/pingpong_shm";
constexpr size_t SHM_SIZE = 4096; // 4KB is enough

enum State : uint32_t {
    STATE_WAIT_REQ = 0,
    STATE_REQ_READY = 1,
    STATE_RESP_READY = 2,
    STATE_DONE = 3
};

struct Packet {
    std::atomic<uint32_t> state;
    uint32_t req_id;
    int64_t val_a;
    int64_t val_b;
    int64_t sum;
    uint8_t padding[64 - 4 - 4 - 8 - 8 - 8]; // Pad to 64 bytes
};

static_assert(sizeof(Packet) == 64, "Packet size mismatch");

}
