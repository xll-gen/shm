#pragma once

#include <stdint.h>
#include <atomic>

// Message IDs for control messages
#define MSG_ID_NORMAL 0
#define MSG_ID_HEARTBEAT_REQ 1
#define MSG_ID_HEARTBEAT_RESP 2
#define MSG_ID_SHUTDOWN 3

namespace shm {

// Direct Mode Slot Header
struct SlotHeader {
    uint8_t pre_pad[64];                  // Padding to avoid false sharing with ExchangeHeader
    std::atomic<uint32_t> state;          // 4
    uint32_t reqSize;                     // 4
    uint32_t respSize;                    // 4
    uint32_t msgId;                       // 4
    std::atomic<uint32_t> hostSleeping;   // 4
    std::atomic<uint32_t> guestSleeping;  // 4
    uint8_t padding[40];                  // Padding to make structure 128 bytes (64 + 24 + 40 = 128)
};

// Slot State Constants (PingPong Style)
enum SlotState {
    SLOT_WAIT_REQ = 0,
    SLOT_REQ_READY = 1,
    SLOT_RESP_READY = 2,
    SLOT_DONE = 3
};

// Direct Mode Exchange Header
struct ExchangeHeader {
    uint32_t numSlots;
    uint32_t slotSize;
    uint8_t padding[56]; // Padding to 64 bytes
};

}
