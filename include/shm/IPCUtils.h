#pragma once

#include <stdint.h>
#include <atomic>

// Message Types for control messages

/**
 * @brief Message Type for normal data payload.
 */
#define MSG_TYPE_NORMAL 0

/**
 * @brief Message Type for heartbeat request (keep-alive).
 */
#define MSG_TYPE_HEARTBEAT_REQ 1

/**
 * @brief Message Type for heartbeat response.
 */
#define MSG_TYPE_HEARTBEAT_RESP 2

/**
 * @brief Message Type for shutdown signal.
 * Used to signal the Guest to terminate its worker loop.
 */
#define MSG_TYPE_SHUTDOWN 3

/**
 * @brief Message Type for FlatBuffer payload.
 * Used when sending Zero-Copy FlatBuffers where the data is aligned to the end of the buffer.
 */
#define MSG_TYPE_FLATBUFFER 10

/**
 * @brief Message Type for Guest Call (Guest -> Host).
 */
#define MSG_TYPE_GUEST_CALL 11

/**
 * @brief Start of Application Specific message types.
 * IDs below 128 are reserved for internal protocol use.
 * Users should start their custom message types from this value.
 *
 * Example:
 *   const uint32_t MY_MSG_LOGIN  = MSG_TYPE_APP_START + 0;
 *   const uint32_t MY_MSG_UPDATE = MSG_TYPE_APP_START + 1;
 */
#define MSG_TYPE_APP_START 128

// Host/Guest Sleeping States

/**
 * @brief Indicates the Host is active (spinning or processing).
 */
#define HOST_STATE_ACTIVE 0

/**
 * @brief Indicates the Host is waiting on the Response Event.
 */
#define HOST_STATE_WAITING 1

/**
 * @brief Indicates the Guest is active (spinning or processing).
 */
#define GUEST_STATE_ACTIVE 0

/**
 * @brief Indicates the Guest is waiting on the Request Event.
 */
#define GUEST_STATE_WAITING 1

namespace shm {

// Direct Mode Slot Header
// Aligned to 128 bytes to prevent false sharing
/**
 * @brief Header structure for a single Direct Mode slot.
 *
 * This structure resides in shared memory and coordinates the state
 * of a single request/response transaction.
 * Aligned to 128 bytes to prevent false sharing between slots.
 */
struct SlotHeader {
    /**
     * @brief Padding to ensure cache line alignment and avoid false sharing with ExchangeHeader or previous slot.
     */
    uint8_t pre_pad[64];

    /**
     * @brief Current state of the slot (Free, Busy, ReqReady, RespReady).
     * Accessed via atomic operations.
     */
    std::atomic<uint32_t> state;

    /**
     * @brief State of the Host (Active/Waiting).
     * Used by the Guest to determine whether to signal the Host.
     */
    std::atomic<uint32_t> hostState;

    /**
     * @brief State of the Guest (Active/Waiting).
     * Used by the Host to determine whether to signal the Guest.
     */
    std::atomic<uint32_t> guestState;

    /**
     * @brief Message Sequence Number.
     * Unique identifier for the transaction, echoed back by the receiver.
     */
    uint32_t msgSeq;

    /**
     * @brief Message Type (e.g., MSG_TYPE_NORMAL, MSG_TYPE_SHUTDOWN).
     * Describes the content/command of the message.
     */
    uint32_t msgType;

    /**
     * @brief Size of the request payload in bytes.
     * Positive: Data starts at offset 0.
     * Negative: Data starts at end (size = -reqSize).
     */
    int32_t reqSize;

    /**
     * @brief Size of the response payload in bytes.
     * Positive: Data starts at offset 0.
     * Negative: Data starts at end (size = -respSize).
     */
    int32_t respSize;

    /**
     * @brief Padding to align the struct to 128 bytes total size.
     * 64 (pre_pad) + 4 (state) + 4 (hostState) + 4 (guestState) + 4 (msgId) + 4 (msgType) + 4 (reqSize) + 4 (respSize) = 92 bytes.
     * 128 - 92 = 36 bytes padding.
     */
    uint8_t padding[36];
};

// Slot State Constants
/**
 * @brief Enumeration of possible Slot states.
 */
enum SlotState {
    /** @brief Slot is free. Host can claim it. */
    SLOT_FREE = 0,
    /** @brief Request data is written. Ready for Guest to process. */
    SLOT_REQ_READY = 1,
    /** @brief Response data is written. Ready for Host to read. */
    SLOT_RESP_READY = 2,
    /** @brief Transaction complete (transient state). */
    SLOT_DONE = 3,
    /** @brief Slot is claimed by Host, writing request. */
    SLOT_BUSY = 4
};

// Direct Mode Exchange Header
// First 64 bytes of Shared Memory
/**
 * @brief Header structure located at the beginning of the Shared Memory region.
 *
 * Contains metadata about the shared memory layout, allowing the Guest
 * to map the memory correctly.
 */
struct ExchangeHeader {
    /** @brief Number of slots in the pool (Host -> Guest). */
    uint32_t numSlots;
    /** @brief Number of Guest Call slots (Guest -> Host). */
    uint32_t numGuestSlots;
    /** @brief Total size of each slot in bytes. */
    uint32_t slotSize;
    /** @brief Offset of the Request buffer within a slot. */
    uint32_t reqOffset;
    /** @brief Offset of the Response buffer within a slot. */
    uint32_t respOffset;
    /** @brief Padding to align to 64 bytes. */
    uint8_t padding[44];
};

}
