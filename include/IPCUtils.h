#pragma once

#include <stdint.h>
#include <atomic>

// Message IDs for control messages
#define MSG_ID_NORMAL 0
#define MSG_ID_HEARTBEAT_REQ 1
#define MSG_ID_HEARTBEAT_RESP 2
#define MSG_ID_SHUTDOWN 3

// Host/Guest Sleeping States
#define HOST_STATE_ACTIVE 0
#define HOST_STATE_WAITING 1

#define GUEST_STATE_ACTIVE 0
#define GUEST_STATE_WAITING 1

namespace shm {

// Direct Mode Slot Header
// Aligned to 128 bytes to prevent false sharing
struct SlotHeader {
    uint8_t pre_pad[64];             // Padding to avoid false sharing with ExchangeHeader or previous slot
    std::atomic<uint32_t> state;     // 4
    uint32_t reqSize;                // 4
    uint32_t respSize;               // 4
    uint32_t msgId;                  // 4
    std::atomic<uint32_t> hostState; // 4 - Host sleeping state
    std::atomic<uint32_t> guestState;// 4 - Guest sleeping state
    uint8_t padding[40];             // 40 - Padding to 128 bytes
};

// Slot State Constants
enum SlotState {
    SLOT_FREE = 0,       // Host is preparing or Idle
    SLOT_REQ_READY = 1,  // Request written, ready for Guest
    SLOT_RESP_READY = 2, // Response written, ready for Host
    SLOT_DONE = 3,       // Transaction Complete (optional usage)
    SLOT_BUSY = 4        // Host is writing request
};

// Direct Mode Exchange Header
// First 64 bytes of Shared Memory
struct ExchangeHeader {
    uint32_t numSlots;
    uint32_t slotSize;
    uint32_t reqOffset;
    uint32_t respOffset;
    uint8_t padding[48]; // Padding to 64 bytes
};

}
