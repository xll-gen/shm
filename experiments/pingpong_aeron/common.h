#pragma once
#include <cstdint>

namespace pingpong {

constexpr int STREAM_ID_BASE_PING = 1000;
constexpr int STREAM_ID_BASE_PONG = 2000;
constexpr const char* CHANNEL = "aeron:ipc";

struct Message {
    uint64_t req_id;    // 8 bytes
    int64_t val_a;      // 8 bytes
    int64_t val_b;      // 8 bytes
    int64_t sum;        // 8 bytes
    uint8_t padding[32]; // 32 bytes
};

// 8+8+8+8 + 32 = 64 bytes
static_assert(sizeof(Message) == 64, "Message size must be 64 bytes");

}
