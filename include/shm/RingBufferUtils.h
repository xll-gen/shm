#pragma once

#include "IPCUtils.h"
#include <atomic>

namespace shm {

/**
 * @brief Ring Buffer Mode Protocol
 *
 * When a Slot is in RingBuffer Mode (MsgType::RING_BUFFER),
 * the payload area (Request + Response buffers) is treated as a single Circular Buffer.
 *
 * Layout within the Slot Payload (starting at reqOffset):
 * [RingHeader (128 bytes)] [Circular Data Buffer ........... ]
 */

struct RingBufferHeader {
    // We use atomic offsets for Head/Tail.
    // Alignment to 128 bytes to avoid false sharing is critical if they are updated by different threads.
    // However, in Ring Buffer, Head is Producer-Write/Consumer-Read, Tail is Consumer-Write/Producer-Read.
    // So they should be on separate cache lines.

    alignas(128) std::atomic<uint64_t> writeOffset; // Producer (Host) writes, Consumer reads.
    alignas(128) std::atomic<uint64_t> readOffset;  // Consumer (Guest) writes, Producer reads.

    // No padding needed at the end if we just start data after this.
    // But let's keep it clean.
};

// Ensure header size is reasonable. 256 bytes total.
static_assert(sizeof(RingBufferHeader) == 256, "RingBufferHeader size mismatch");

}
