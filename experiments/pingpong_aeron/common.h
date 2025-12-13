#pragma once
#include <cstdint>

namespace pingpong {

constexpr int STREAM_ID_BASE_PING = 1000;
constexpr int STREAM_ID_BASE_PONG = 2000;
constexpr const char* CHANNEL = "aeron:ipc";

struct Message {
    uint32_t req_id;
    int64_t val_a;
    int64_t val_b;
    int64_t sum;
    uint8_t padding[36]; // Padding to ensure 64-byte payload
};

static_assert(sizeof(Message) == 64, "Message size must be 64 bytes");

}
