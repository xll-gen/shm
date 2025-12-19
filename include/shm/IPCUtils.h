#pragma once

#include <stdint.h>
#include <atomic>

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

/**
 * @brief Magic Number "XLL!" (0x584C4C21).
 * Used to verify that the shared memory region is a valid xll-gen/shm segment.
 */
static const uint32_t SHM_MAGIC = 0x584C4C21;

/**
 * @brief Protocol Version v0.6.0 (0x00060000).
 * High 16 bits: Major, Low 16 bits: Minor.
 * Breaking changes increment Major version.
 */
static const uint32_t SHM_VERSION = 0x00060000;

/**
 * @brief Message Types for control messages.
 * Strongly typed enum to ensure type safety and match Go implementation.
 */
enum class MsgType : uint32_t {
    /** @brief Normal data payload. */
    NORMAL = 0,
    /** @brief Heartbeat request (keep-alive). */
    HEARTBEAT_REQ = 1,
    /** @brief Heartbeat response. */
    HEARTBEAT_RESP = 2,
    /** @brief Shutdown signal. Signal Guest to terminate worker loop. */
    SHUTDOWN = 3,
    /** @brief FlatBuffer payload (Zero-Copy End-Aligned). */
    FLATBUFFER = 10,
    /** @brief Guest Call (Guest -> Host). */
    GUEST_CALL = 11,
    /** @brief Stream Start (Host -> Guest). used for initializing a stream. */
    STREAM_START = 13,
    /** @brief Stream Chunk (Host -> Guest). used for sending a chunk of a stream. */
    STREAM_CHUNK = 14,
    /** @brief System Error (e.g. buffer overflow, invalid state). */
    SYSTEM_ERROR = 127,
    /** @brief Start of Application Specific message types. */
    APP_START = 128
};

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
     * @brief Message Type (e.g., MsgType::NORMAL, MsgType::SHUTDOWN).
     * Describes the content/command of the message.
     */
    MsgType msgType;

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
     * @brief Reserved for future use.
     * 64 (pre_pad) + 4 (state) + 4 (hostState) + 4 (guestState) + 4 (msgSeq) + 4 (msgType) + 4 (reqSize) + 4 (respSize) = 92 bytes.
     * 128 - 92 = 36 bytes reserved.
     */
    uint8_t reserved[36];
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
    SLOT_BUSY = 4,
    /** @brief Slot is claimed by Guest, writing request. */
    SLOT_GUEST_BUSY = 5
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
    /** @brief Magic Number (SHM_MAGIC). */
    uint32_t magic;
    /** @brief Protocol Version (SHM_VERSION). */
    uint32_t version;
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
    /** @brief Reserved for future use. */
    uint8_t reserved[36];
};

}
