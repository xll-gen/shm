#pragma once

#include <stdint.h>
#include <atomic>

/**
 * @brief Magic number to identify initialized memory blocks in the queue.
 * Value: 0xAB12CD34
 */
#define BLOCK_MAGIC 0xAB12CD34

/**
 * @brief Magic number indicating a block contains valid data.
 * Value: 0xAB12CD34
 */
#define BLOCK_MAGIC_DATA 0xAB12CD34

/**
 * @brief Magic number indicating a block is padding (to handle ring buffer wrap-around).
 * Value: 0xAB12CD35
 */
#define BLOCK_MAGIC_PAD  0xAB12CD35

/**
 * @brief Message ID for normal user data payloads.
 */
#define MSG_ID_NORMAL 0

/**
 * @brief Message ID for Heartbeat Request (Host -> Guest).
 * Payload is empty.
 */
#define MSG_ID_HEARTBEAT_REQ 1

/**
 * @brief Message ID for Heartbeat Response (Guest -> Host).
 * Payload is empty.
 */
#define MSG_ID_HEARTBEAT_RESP 2

/**
 * @brief Message ID for Shutdown signal (Host -> Guest).
 * Guest should exit its loop upon receiving this.
 */
#define MSG_ID_SHUTDOWN 3

/**
 * @brief Assumed cache line size for alignment purposes (64 bytes).
 */
#define CACHE_LINE_SIZE 64

/**
 * @brief Size of the BlockHeader structure in bytes.
 */
#define BLOCK_HEADER_SIZE 16

/**
 * @brief Host State: Active (Polling).
 */
#define HOST_STATE_ACTIVE 0

/**
 * @brief Host State: Waiting (Sleeping/Blocked).
 */
#define HOST_STATE_WAITING 1

namespace shm {

/**
 * @brief Header structure for each data block in the SPSC Queue.
 *
 * Each message in the ring buffer is preceded by this header.
 * It manages the size, type, and validity (magic) of the block.
 */
struct BlockHeader {
    /** @brief Size of the payload data following this header. */
    uint32_t size;

    /** @brief Message ID identifying the type of message (Normal vs Control). */
    uint32_t msgId;

    /** @brief Magic number used for atomic synchronization and validity checking. */
    std::atomic<uint32_t> magic;

    /** @brief Padding to align the header. */
    uint32_t _pad;
};

/**
 * @brief Shared Memory Header for the SPSC Queue.
 *
 * Located at the beginning of the shared memory region.
 * Contains the write and read positions, capacity, and consumer state.
 * Heavily padded to prevent false sharing between producer and consumer threads.
 */
struct QueueHeader {
    /** @brief Current write position (Producer index). Monotonically increasing. */
    std::atomic<uint64_t> writePos;     // 8 bytes

    /** @brief Padding to separate writePos and readPos (prevent false sharing). */
    uint8_t _pad1[56];

    /** @brief Current read position (Consumer index). Monotonically increasing. */
    std::atomic<uint64_t> readPos;      // 8 bytes

    /** @brief Total capacity of the ring buffer in bytes. */
    std::atomic<uint64_t> capacity;     // 8 bytes

    /**
     * @brief Consumer state flag.
     * 0 = Waiting (Sleeping), 1 = Active (Running/Polling).
     * Used for the "Signal-If-Waiting" optimization.
     */
    std::atomic<uint32_t> consumerActive; // 4 bytes

    /** @brief Padding to align the structure to 128 bytes. */
    uint8_t _pad2[44];
};

/**
 * @brief Slot Header for Direct Mode.
 *
 * Represents a single exchange slot in shared memory.
 * Each worker thread ("Lane") is assigned a specific slot.
 */
struct SlotHeader {
    /** @brief Padding to avoid false sharing with adjacent structures. */
    uint8_t pre_pad[64];

    /**
     * @brief Current state of the slot (SlotState enum).
     * Managed via atomic CAS operations.
     */
    std::atomic<uint32_t> state;

    /** @brief Size of the request payload. */
    uint32_t reqSize;

    /** @brief Size of the response payload. */
    uint32_t respSize;

    /** @brief Message ID for the request. */
    uint32_t msgId;

    /**
     * @brief State of the Host waiting on this slot.
     * HOST_STATE_ACTIVE or HOST_STATE_WAITING.
     */
    std::atomic<uint32_t> hostState;

    /** @brief Padding to make structure 128 bytes total. */
    uint8_t padding[44];
};

/**
 * @brief Enumeration of Slot States for Direct Mode synchronization.
 */
enum SlotState {
    /** @brief Slot is free and ready for a new request. */
    SLOT_FREE = 0,

    /** @brief Guest worker is polling this slot. */
    SLOT_POLLING = 1,

    /** @brief Host has claimed the slot and is writing a request. */
    SLOT_BUSY = 2,

    /** @brief Request is fully written and ready for the Guest. */
    SLOT_REQ_READY = 3,

    /** @brief Response is fully written and ready for the Host. */
    SLOT_RESP_READY = 4,

    /** @brief Host has read the response and is releasing the slot. */
    SLOT_HOST_DONE = 5
};

/**
 * @brief Header for the Direct Mode Exchange Region.
 *
 * (Legacy/Reserved) Describes the layout of the slot array.
 */
struct ExchangeHeader {
    /** @brief Number of slots in the region. */
    uint32_t numSlots;

    /** @brief Size in bytes of each slot's data area. */
    uint32_t slotSize;

    /** @brief Padding to 64 bytes. */
    uint8_t padding[56];
};

}
