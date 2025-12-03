#pragma once

#include <stdint.h>
#include <atomic>

// Magic number to identify initialized memory
#define BLOCK_MAGIC 0xAB12CD34
#define BLOCK_MAGIC_DATA 0xAB12CD34
#define BLOCK_MAGIC_PAD  0xAB12CD35

// Message IDs for control messages
#define MSG_ID_NORMAL 0
#define MSG_ID_HEARTBEAT_REQ 1
#define MSG_ID_HEARTBEAT_RESP 2
#define MSG_ID_SHUTDOWN 3

// Alignment for cache lines (usually 64 bytes)
#define CACHE_LINE_SIZE 64
#define BLOCK_HEADER_SIZE 16

// Host State constants
#define HOST_STATE_ACTIVE 0
#define HOST_STATE_WAITING 1

namespace shm {

// Standard Block Header for Queue Mode
struct BlockHeader {
    uint32_t size;
    uint32_t msgId;
    std::atomic<uint32_t> magic;
    uint32_t _pad;
};

// Queue Header (shared memory layout)
struct QueueHeader {
    std::atomic<uint64_t> writePos;     // 8 bytes
    std::atomic<uint64_t> readPos;      // 8 bytes
    std::atomic<uint64_t> capacity;     // 8 bytes
    std::atomic<uint32_t> consumerActive; // 4 bytes (0=Sleeping, 1=Active)
    uint32_t _pad1;                     // 4 bytes
    uint8_t _pad2[96];                  // Padding to 128 bytes
};

// Direct Mode Slot Header
struct SlotHeader {
    std::atomic<uint32_t> state;
    uint32_t reqSize;
    uint32_t respSize;
    uint32_t msgId;
    std::atomic<uint32_t> hostState; // HOST_STATE_ACTIVE or HOST_STATE_WAITING
    uint8_t padding[44];             // Padding to make structure 64 bytes
};

// Slot State Constants
enum SlotState {
    SLOT_FREE = 0,
    SLOT_POLLING = 1,
    SLOT_BUSY = 2,
    SLOT_REQ_READY = 3,
    SLOT_RESP_READY = 4,
    SLOT_HOST_DONE = 5
};

// Direct Mode Exchange Header
struct ExchangeHeader {
    uint32_t numSlots;
    uint32_t slotSize;
    uint8_t padding[56]; // Padding to 64 bytes
};

}
