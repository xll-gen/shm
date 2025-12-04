#pragma once

#include <stdint.h>
#include <atomic>

// Message IDs for control messages
#define MSG_ID_NORMAL 0
#define MSG_ID_HEARTBEAT_REQ 1
#define MSG_ID_HEARTBEAT_RESP 2
#define MSG_ID_SHUTDOWN 3

// Host State constants
#define HOST_STATE_ACTIVE 0
#define HOST_STATE_WAITING 1

namespace shm {

// Direct Mode Slot Header
struct SlotHeader {
    uint8_t pre_pad[64];             // Padding to avoid false sharing with ExchangeHeader
    std::atomic<uint32_t> state;
    uint32_t reqSize;
    uint32_t respSize;
    uint32_t msgId;
    std::atomic<uint32_t> hostState; // HOST_STATE_ACTIVE or HOST_STATE_WAITING
    uint8_t padding[44];             // Padding to make structure 128 bytes
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
