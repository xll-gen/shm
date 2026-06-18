#pragma once

#include <stdint.h>
#include <atomic>
#include <cstddef> // offsetof — per-field ABI offset guards (R25)

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
 * @brief Wire protocol version (0x00070000). High 16 bits: Major, low 16 bits: Minor.
 *
 * Intentionally PINNED across the entire ABI-compatible v0.7.x series — patch
 * releases add fields carved from `reserved` (atomic uint64 `lease` at SlotHeader
 * offset 96 in v0.7.0 for crash-recovery reclamation; atomic uint64 `gen` at offset
 * 104 in v0.7.5 for the reclamation ABA guard) without bumping this constant, since
 * old readers never touched those bytes. Only breaking layout changes increment Major.
 */
static const uint32_t SHM_VERSION = 0x00070000;

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
 *
 * @note The `alignas(64)` specifier matches the x86/x64 cache-line size,
 *       which is the only supported deployment target (see
 *       `AGENTS.md` §"Platform Targets"). Each slot is sized to exactly
 *       128 bytes, so consecutive slots occupy back-to-back cache lines
 *       and false sharing is avoided in practice regardless of whether
 *       a future host happens to use 128-byte lines.
 */
struct alignas(64) SlotHeader {
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
     * @brief Monotonic-ns heartbeat written by the slot's current owner.
     *
     * Whoever last CAS'd `state` to a non-FREE value MUST write
     * `Platform::MonotonicNanos()` to `lease` in the same critical section
     * (immediately after the CAS or just before publishing it). This
     * marks the slot as "actively owned at time T".
     *
     * Lives at offset 96 (4 bytes of compiler-inserted padding after
     * `respSize` to satisfy `std::atomic<uint64_t>`'s 8-byte alignment;
     * Go-side matches with an explicit `uint32` pad).
     *
     * Added in shm v0.7.0 / protocol version 0x00070000. Older readers
     * compiled against v0.6.x ignore this field (it lives inside what
     * used to be `reserved[36]`); they keep the original forever-busy
     * behavior on peer crash.
     *
     * v0.7.0 only writes the lease — no reclamation logic yet. The
     * `TryReclaimAbandonedSlot` API and auto-reclamation hook arrive in
     * v0.7.1 together with a property-based crash-injection test.
     * Until then this field is informational; consumers may read it to
     * detect liveness manually.
     */
    std::atomic<uint64_t> lease;

    /**
     * @brief Claim generation counter (v0.7.5). Offset 104.
     *
     * Monotonic counter advanced by EVERY slot-claiming path immediately
     * BEFORE its state-claiming CAS (see the `claimGen` helper and
     * `SPECIFICATION.md §3.6.1`). It exists to make crash-recovery
     * reclamation airtight against the ABA hazard: `state` alone returns to
     * the same value when a slot is finished, reused, and re-claimed, and
     * `lease` is published only AFTER the claiming CAS, so neither a bare
     * `state` CAS nor a `lease` re-read can distinguish an abandoned slot
     * from a freshly re-claimed live one.
     *
     * `TryReclaimAbandonedSlot` snapshots `gen`, verifies staleness, then
     * reclaims by `compare_exchange(gen, gen+1)`. Because every claim bumps
     * `gen` before transitioning `state`, the reclaim CAS fails whenever a
     * claim has begun — including the lease-publication-lag window.
     *
     * Carved from the former `reserved[24]`; the layout stays 128 bytes and
     * is forward-compatible with v0.7.0–v0.7.3 readers (which never wrote
     * these bytes). A peer that does not bump `gen` degrades to the
     * pre-v0.7.5 reclaim behavior for its slots.
     */
    std::atomic<uint64_t> gen;

    /**
     * @brief Reserved for future use.
     * 64 (pre_pad) + 4*7 (uint32 cluster up to respSize) + 4 (alignment pad)
     * + 8 (lease) + 8 (gen) = 112 bytes. 128 - 112 = 16 bytes reserved.
     */
    uint8_t reserved[16];
};

// ABI safety: SlotHeader must remain exactly 128 bytes and at least 64-byte
// aligned. These asserts are textual guards; layout is frozen.
static_assert(sizeof(SlotHeader) == 128, "SlotHeader must be exactly 128 bytes (ABI)");
static_assert(alignof(SlotHeader) >= 64, "SlotHeader must be at least 64-byte aligned");

// Per-field offset guards (R25). The size assert above catches total-size drift
// but NOT field reordering that preserves 128 bytes (e.g. swapping reqSize/respSize
// or moving the pre-lease alignment pad). These freeze every field offset and are
// mirrored byte-for-byte by the Go side's unsafe.Offsetof guards in go/direct.go
// and by SPECIFICATION.md §2.2.1. Update all three together if the layout ever changes.
static_assert(offsetof(SlotHeader, state)      == 64,  "SlotHeader.state @64");
static_assert(offsetof(SlotHeader, hostState)  == 68,  "SlotHeader.hostState @68");
static_assert(offsetof(SlotHeader, guestState) == 72,  "SlotHeader.guestState @72");
static_assert(offsetof(SlotHeader, msgSeq)     == 76,  "SlotHeader.msgSeq @76");
static_assert(offsetof(SlotHeader, msgType)    == 80,  "SlotHeader.msgType @80");
static_assert(offsetof(SlotHeader, reqSize)    == 84,  "SlotHeader.reqSize @84");
static_assert(offsetof(SlotHeader, respSize)   == 88,  "SlotHeader.respSize @88");
static_assert(offsetof(SlotHeader, lease)      == 96,  "SlotHeader.lease @96 (4B pad after respSize)");
static_assert(offsetof(SlotHeader, gen)        == 104, "SlotHeader.gen @104");
static_assert(offsetof(SlotHeader, reserved)   == 112, "SlotHeader.reserved @112");

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
    /** @brief Transaction complete (transient state).
     *  SLOT_DONE = 3 (reserved; not currently used by the protocol but
     *  retained for future flow extensions). */
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

// ABI safety: ExchangeHeader must remain exactly 64 bytes. Layout is frozen.
static_assert(sizeof(ExchangeHeader) == 64, "ExchangeHeader must be exactly 64 bytes (ABI)");

// Per-field offset guards (R25) — freeze every field offset against the Go mirror
// (go/direct.go) and SPECIFICATION.md §2.1; the size assert alone misses reordering.
static_assert(offsetof(ExchangeHeader, magic)         == 0,  "ExchangeHeader.magic @0");
static_assert(offsetof(ExchangeHeader, version)       == 4,  "ExchangeHeader.version @4");
static_assert(offsetof(ExchangeHeader, numSlots)      == 8,  "ExchangeHeader.numSlots @8");
static_assert(offsetof(ExchangeHeader, numGuestSlots) == 12, "ExchangeHeader.numGuestSlots @12");
static_assert(offsetof(ExchangeHeader, slotSize)      == 16, "ExchangeHeader.slotSize @16");
static_assert(offsetof(ExchangeHeader, reqOffset)     == 20, "ExchangeHeader.reqOffset @20");
static_assert(offsetof(ExchangeHeader, respOffset)    == 24, "ExchangeHeader.respOffset @24");
static_assert(offsetof(ExchangeHeader, reserved)      == 28, "ExchangeHeader.reserved @28");

}
