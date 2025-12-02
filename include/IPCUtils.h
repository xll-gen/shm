#pragma once
#include <atomic>
#include <cstdint>

namespace shm {

static const uint32_t BLOCK_MAGIC_DATA = 0xDA7A0001;
static const uint32_t BLOCK_MAGIC_PAD  = 0xDA7A0002;
static const uint32_t BLOCK_HEADER_SIZE = 16;

// Message ID Constants
static const uint32_t MSG_ID_NORMAL = 0;
static const uint32_t MSG_ID_HEARTBEAT_REQ = 1;
static const uint32_t MSG_ID_HEARTBEAT_RESP = 2;
static const uint32_t MSG_ID_SHUTDOWN = 3;

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

// Direct Exchange / Slot based IPC
enum SlotState : uint32_t {
    SLOT_FREE = 0,         // Worker is waiting/sleeping
    SLOT_POLLING = 1,      // Worker is busy-looping checking for work
    SLOT_BUSY = 2,         // Host claimed slot, writing request
    SLOT_REQ_READY = 3,    // Request written, Worker can process
    SLOT_RESP_READY = 4,   // Response written, Host can read
    SLOT_HOST_DONE = 5     // Host finished reading, Worker can reset
};

struct ExchangeHeader {
    uint32_t numSlots;
    uint32_t slotSize;
    uint8_t padding[56]; // Pad to 64 bytes
};

struct SlotHeader {
    alignas(64) std::atomic<uint32_t> state;
    uint32_t reqSize;
    uint32_t respSize;
    uint32_t msgId;
    uint8_t padding[48]; // Pad to 64 bytes
};

}
