#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <memory>
#include <thread>
#include <cstring>
#include <iostream>
#include <functional>
#include <atomic>
#include <cmath>
#include <chrono>
#include "Platform.h"
#include "IPCUtils.h"
#include "WaitStrategy.h"
#include "Result.h"

namespace shm {

/**
 * @struct HostConfig
 * @brief Configuration parameters for initializing the DirectHost.
 */
struct HostConfig {
    /** @brief Name of the shared memory region (e.g., "MyIPC"). */
    std::string shmName;

    /** @brief Number of slots (workers) to allocate for Host-to-Guest communication.
     * Corresponds to `numSlots` in ExchangeHeader.
     */
    uint32_t numHostSlots;

    /** @brief Desired capacity for request/response payloads in bytes.
     * The actual shared memory slot size will be calculated to accommodate this payload plus headers.
     * Default: 1MB.
     */
    uint32_t payloadSize = 1024 * 1024;

    /** @brief Number of slots to allocate for Guest-to-Host calls. Default: 0. */
    uint32_t numGuestSlots = 0;
};

/**
 * @class DirectHost
 * @brief Implements the Host side of the Direct Mode IPC.
 *
 * The DirectHost manages a pool of slots in shared memory. Each slot is intended
 * to be paired with a specific Guest worker thread.
 * It uses a hybrid spin/wait strategy for low latency and utilizes
 * specific memory layout defined in IPCUtils.h.
 */
class DirectHost {
    void* shmBase;
    std::string shmName;
    uint32_t numSlots;
    uint32_t numGuestSlots;
    uint64_t totalShmSize;
    ShmHandle hMapFile;
    bool running;
    uint32_t responseTimeoutMs;

    /**
     * @brief Stride for Message Sequence generation.
     * Used to ensure global uniqueness of msgSeq without atomic contention.
     * msgSeq = (previous_seq) + msgSeqStride.
     */
    uint32_t msgSeqStride;

    /**
     * @brief Internal representation of a Slot.
     */
    struct Slot {
        SlotHeader* header;
        uint8_t* reqBuffer;
        uint8_t* respBuffer;
        EventHandle hReqEvent;  // Signaled by Host (Wake Guest)
        EventHandle hRespEvent; // Signaled by Guest (Wake Host)
        uint32_t maxReqSize;
        uint32_t maxRespSize;
        WaitStrategy waitStrategy;
        uint32_t msgSeq;
        std::atomic<bool> activeWait;

        Slot() : header(nullptr), reqBuffer(nullptr), respBuffer(nullptr),
                 hReqEvent(nullptr), hRespEvent(nullptr),
                 maxReqSize(0), maxRespSize(0), msgSeq(0), activeWait(false) {}

        Slot(Slot&& other) noexcept : waitStrategy(other.waitStrategy) {
            header = other.header;
            reqBuffer = other.reqBuffer;
            respBuffer = other.respBuffer;
            hReqEvent = other.hReqEvent;
            hRespEvent = other.hRespEvent;
            maxReqSize = other.maxReqSize;
            maxRespSize = other.maxRespSize;
            msgSeq = other.msgSeq;
            activeWait.store(other.activeWait.load());

            other.header = nullptr;
            other.hReqEvent = nullptr;
            other.hRespEvent = nullptr;
        }
    };

    std::vector<Slot> slots;
    std::atomic<uint32_t> nextSlot{0}; // Round-robin hint

    // Config
    uint32_t slotSize; // Total payload size per slot

    /**
     * @brief Internal helper to wait for a response on a specific slot.
     * Contains the adaptive spin/yield/wait logic.
     * @param slot Pointer to the slot to wait on.
     * @param timeoutMs Timeout in milliseconds.
     * @return true if response is ready, false if error/timeout.
     */
    bool WaitResponse(Slot* slot, uint32_t timeoutMs) {
        slot->activeWait.store(true, std::memory_order_relaxed);

        // Reset Host State
        slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);

        // Signal Ready
        slot->header->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

        // Wake Guest if waiting
        if (slot->header->guestState.load(std::memory_order_seq_cst) == GUEST_STATE_WAITING) {
            Platform::SignalEvent(slot->hReqEvent);
        }

        // Condition lambda
        auto checkReady = [&]() -> bool {
            return slot->header->state.load(std::memory_order_acquire) == SLOT_RESP_READY;
        };

        // Sleep/Wait lambda
        auto sleepAction = [&]() {
            slot->header->hostState.store(HOST_STATE_WAITING, std::memory_order_seq_cst);

            // Double check
            if (checkReady()) {
                slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
                return;
            }

            // Wait with timeout
            auto start = std::chrono::steady_clock::now();
            while (!checkReady()) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

                // Note: If timeoutMs is 0xFFFFFFFF, elapsed >= timeoutMs might be false unless elapsed is huge.
                // But generally safe.
                if (timeoutMs != 0xFFFFFFFF && elapsed >= timeoutMs) {
                    break; // Timeout, return and let caller fail
                }

                uint32_t remaining = (timeoutMs == 0xFFFFFFFF) ? 0xFFFFFFFF : (timeoutMs - (uint32_t)elapsed);
                uint32_t waitTime = (remaining > 100) ? 100 : remaining;
                if (timeoutMs == 0xFFFFFFFF) waitTime = 100; // Cap infinite wait chunks

                Platform::WaitEvent(slot->hRespEvent, waitTime);
            }
            slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
        };

        bool ready = slot->waitStrategy.Wait(checkReady, sleepAction);

        slot->activeWait.store(false, std::memory_order_release);
        return ready;
    }

public:
    /**
     * @brief Helper class for managing a Zero-Copy slot.
     *
     * This class acts as a smart wrapper around a slot index. It allows:
     * 1. Direct access to the request buffer (for building FlatBuffers).
     * 2. Sending messages without manually managing the slot index.
     * 3. Automatic release of the slot when the object goes out of scope (RAII).
     * 4. Zero-copy access to the response buffer.
     */
    static const uint32_t USE_DEFAULT_TIMEOUT = 0xFFFFFFFF;

    class ZeroCopySlot {
        DirectHost* host;
        int32_t slotIdx;

    public:
        /**
         * @brief Constructs a ZeroCopySlot.
         * @param h Pointer to the DirectHost.
         * @param idx The index of the acquired slot.
         */
        ZeroCopySlot(DirectHost* h, int32_t idx) : host(h), slotIdx(idx) {}

        /**
         * @brief Destructor. Releases the slot if not already moved.
         */
        ~ZeroCopySlot() {
            if (host && slotIdx >= 0) {
                // Ensure slot is released if user didn't call Send?
                // Actually, Send does NOT release the slot in this design (user reads response after Send).
                // So Destructor MUST release the slot.
                host->slots[slotIdx].header->state.store(SLOT_FREE, std::memory_order_release);
            }
        }

        // Move-only semantics
        ZeroCopySlot(ZeroCopySlot&& other) noexcept : host(other.host), slotIdx(other.slotIdx) {
            other.host = nullptr;
            other.slotIdx = -1;
        }

        ZeroCopySlot& operator=(ZeroCopySlot&& other) noexcept {
             if (this != &other) {
                 // Release current if valid
                 if (host && slotIdx >= 0) {
                     host->slots[slotIdx].header->state.store(SLOT_FREE, std::memory_order_release);
                 }
                 host = other.host;
                 slotIdx = other.slotIdx;
                 other.host = nullptr;
                 other.slotIdx = -1;
             }
             return *this;
        }

        // Disable copying
        ZeroCopySlot(const ZeroCopySlot&) = delete;
        ZeroCopySlot& operator=(const ZeroCopySlot&) = delete;

        /**
         * @brief Checks if the slot is valid.
         * @return true if valid, false otherwise.
         */
        bool IsValid() const { return host && slotIdx >= 0; }

        /**
         * @brief Gets the pointer to the Request Buffer.
         * Use this to write your data (e.g. build a FlatBuffer).
         * @return Pointer to the buffer.
         */
        uint8_t* GetReqBuffer() {
            if (!IsValid()) return nullptr;
            return host->slots[slotIdx].reqBuffer;
        }

        /**
         * @brief Gets the maximum size of the Request Buffer.
         * @return Size in bytes.
         */
        int32_t GetMaxReqSize() {
             if (!IsValid()) return 0;
             return host->slots[slotIdx].maxReqSize;
        }

        /**
         * @brief Sends a request using the zero-copy buffer.
         *
         * Allows sending arbitrary message types with control over alignment.
         *
         * @param size Size of the data. Positive: Start-aligned. Negative: End-aligned.
         * @param msgType The message Type.
         * @param timeoutMs Per-call timeout. Default USE_DEFAULT_TIMEOUT.
         * @return Result<void> Success or Error.
         */
        Result<void> Send(int32_t size, MsgType msgType, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
            if (!IsValid()) return Result<void>::Failure(Error::InvalidArgs);

            Slot* slot = &host->slots[slotIdx];

            // Bounds check
            int32_t absSize = size < 0 ? -size : size;
            if ((uint32_t)absSize > slot->maxReqSize) {
                return Result<void>::Failure(Error::BufferTooSmall);
            }

            slot->header->reqSize = size;
            slot->header->msgType = msgType;
            slot->header->msgSeq = slot->msgSeq;
            slot->msgSeq += host->msgSeqStride;

            uint32_t t = (timeoutMs == USE_DEFAULT_TIMEOUT) ? host->responseTimeoutMs : timeoutMs;
            if (!host->WaitResponse(slot, t)) {
                // Timeout. Invalidate slot to prevent accidental reuse or freeing.
                // Leak the slot to prevent corruption.
                slotIdx = -1;
                return Result<void>::Failure(Error::Timeout);
            }
            // Do NOT release slot here. User might want to read response.
            return Result<void>::Success();
        }

        /**
         * @brief Sends the FlatBuffer request.
         *
         * This method:
         * 1. Sets the message Type to MsgType::FLATBUFFER.
         * 2. Sets the request size to negative (indicating end-aligned Zero-Copy).
         * 3. Signals the Guest and waits for completion.
         *
         * @param size The size of the FlatBuffer data (positive integer).
         *             The method automatically negates it for the protocol.
         * @param timeoutMs Per-call timeout. Default USE_DEFAULT_TIMEOUT.
         * @return Result<void> Success or Error.
         */
        Result<void> SendFlatBuffer(int32_t size, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
            if (!IsValid()) return Result<void>::Failure(Error::InvalidArgs);

            Slot* slot = &host->slots[slotIdx];

            // Bounds check
            int32_t absSize = size;
            if ((uint32_t)absSize > slot->maxReqSize) {
                return Result<void>::Failure(Error::BufferTooSmall);
            }

            // Zero-Copy convention: Negative size
            slot->header->reqSize = -absSize;
            slot->header->msgType = MsgType::FLATBUFFER;
            slot->header->msgSeq = slot->msgSeq;
            slot->msgSeq += host->msgSeqStride;

            uint32_t t = (timeoutMs == USE_DEFAULT_TIMEOUT) ? host->responseTimeoutMs : timeoutMs;
            if (!host->WaitResponse(slot, t)) {
                // Timeout. Invalidate slot to prevent accidental reuse or freeing.
                // Leak the slot to prevent corruption.
                slotIdx = -1;
                return Result<void>::Failure(Error::Timeout);
            }
            // Do NOT release slot here. User might want to read response.
            return Result<void>::Success();
        }

        /**
         * @brief Gets the pointer to the Response Buffer.
         * Call this AFTER SendFlatBuffer() returns.
         *
         * @return Pointer to the response data.
         */
        uint8_t* GetRespBuffer() {
             if (!IsValid()) return nullptr;
             Slot* slot = &host->slots[slotIdx];
             int32_t rSize = slot->header->respSize;

             // If positive, start-aligned. If negative, end-aligned.
             if (rSize >= 0) return slot->respBuffer;

             // End-aligned: Calculate offset
             uint32_t absRSize = (0u - (uint32_t)rSize);
             if (absRSize > slot->maxRespSize) absRSize = slot->maxRespSize;

             uint32_t offset = slot->maxRespSize - absRSize;
             return slot->respBuffer + offset;
        }

        /**
         * @brief Gets the size of the Response data.
         * @return Size in bytes.
         */
        int32_t GetRespSize() {
             if (!IsValid()) return 0;
             Slot* slot = &host->slots[slotIdx];
             int32_t rSize = slot->header->respSize;

             uint32_t absResp = (rSize < 0) ? (0u - (uint32_t)rSize) : (uint32_t)rSize;
             if (absResp > slot->maxRespSize) absResp = slot->maxRespSize;

             return (int32_t)absResp;
        }
    };

    /**
     * @brief Default constructor.
     */
    DirectHost() : shmBase(nullptr), hMapFile(0), running(false), msgSeqStride(0), responseTimeoutMs(10000) {}

    /**
     * @brief Destructor. Ensures Shutdown is called.
     */
    ~DirectHost() { Shutdown(); }

    /**
     * @brief Sets the timeout for waiting for a response.
     * @param ms Timeout in milliseconds. Default 10000.
     */
    void SetTimeout(uint32_t ms) {
        responseTimeoutMs = ms;
    }

    /**
     * @brief Initializes the Shared Memory Host using a configuration object.
     *
     * Creates the shared memory region and initializes the ExchangeHeader and SlotHeaders.
     *
     * @param config The HostConfig object containing initialization parameters.
     * @return Result<void> Success or Error.
     */
    Result<void> Init(const HostConfig& config) {
        this->shmName = config.shmName;
        this->numSlots = config.numHostSlots;
        this->numGuestSlots = config.numGuestSlots;
        this->slotSize = config.payloadSize;

        // Calculate Stride for Global Uniqueness (Interleaved)
        // stride = Total Slots.
        this->msgSeqStride = numSlots + numGuestSlots;

        // Split strategy: 50/50
        uint32_t halfSize = slotSize / 2;
        // Align to 64 bytes
        halfSize = (halfSize / 64) * 64;
        if (halfSize < 64) halfSize = 64;

        uint32_t reqOffset = 0;
        uint32_t respOffset = halfSize;

        // Ensure total fits
        if (respOffset + halfSize > slotSize) {
             slotSize = respOffset + halfSize;
        }

        size_t exchangeHeaderSize = sizeof(ExchangeHeader);
        if (exchangeHeaderSize < 64) exchangeHeaderSize = 64;

        size_t slotHeaderSize = sizeof(SlotHeader);
        // Should be 128

        size_t perSlotTotal = slotHeaderSize + slotSize;
        size_t totalSlots = numSlots + numGuestSlots;
        size_t totalSize = exchangeHeaderSize + (perSlotTotal * totalSlots);
        this->totalShmSize = totalSize;

        bool exists = false;
        shmBase = Platform::CreateNamedShm(shmName.c_str(), totalSize, hMapFile, exists);
        if (!shmBase) return Result<void>::Failure(Error::InternalError);

        // Zero out memory if new
        memset(shmBase, 0, totalSize);

        // Write ExchangeHeader
        ExchangeHeader* exHeader = (ExchangeHeader*)shmBase;
        exHeader->magic = SHM_MAGIC;
        exHeader->version = SHM_VERSION;
        exHeader->numSlots = numSlots;
        exHeader->numGuestSlots = numGuestSlots;
        exHeader->slotSize = slotSize;
        exHeader->reqOffset = reqOffset;
        exHeader->respOffset = respOffset;

        slots.resize(totalSlots);
        uint8_t* ptr = (uint8_t*)shmBase + exchangeHeaderSize;

        for (uint32_t i = 0; i < totalSlots; ++i) {
            slots[i].header = (SlotHeader*)ptr;
            uint8_t* dataBase = ptr + slotHeaderSize;
            slots[i].reqBuffer = dataBase + reqOffset;
            slots[i].respBuffer = dataBase + respOffset;
            slots[i].maxReqSize = halfSize;
            slots[i].maxRespSize = slotSize - respOffset;
            // waitStrategy initialized by default constructor

            // Initial msgSeq = Index + 1
            slots[i].msgSeq = i + 1;

            // Events
            std::string reqName;
            if (i < numSlots) {
                reqName = shmName + "_slot_" + std::to_string(i);
            } else {
                // Shared event for all Guest Calls to allow single-thread draining
                reqName = shmName + "_guest_call";
            }
            std::string respName = shmName + "_slot_" + std::to_string(i) + "_resp";

            slots[i].hReqEvent = Platform::CreateNamedEvent(reqName.c_str());
            slots[i].hRespEvent = Platform::CreateNamedEvent(respName.c_str());

            if (!slots[i].hReqEvent || !slots[i].hRespEvent) {
                running = true; // Enable Shutdown
                Shutdown();
                return Result<void>::Failure(Error::InternalError);
            }

            // Initialize Header
            slots[i].header->state.store(SLOT_FREE, std::memory_order_relaxed);
            slots[i].header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
            slots[i].header->guestState.store(GUEST_STATE_ACTIVE, std::memory_order_relaxed);

            ptr += perSlotTotal;
        }

        running = true;
        return Result<void>::Success();
    }

    /**
     * @brief Deprecated Init method. Use HostConfig variant instead.
     */
    Result<void> Init(const std::string& shmName, uint32_t numHostSlots, uint32_t payloadSize = 1024 * 1024, uint32_t numGuestSlots = 0) {
        HostConfig config;
        config.shmName = shmName;
        config.numHostSlots = numHostSlots;
        config.payloadSize = payloadSize;
        config.numGuestSlots = numGuestSlots;
        return Init(config);
    }

    /**
     * @brief Sends shutdown signal to all guest workers.
     * Blocks until all slots have been signaled.
     */
    void SendShutdown() {
        if (!running) return;

        for (uint32_t i = 0; i < numSlots; ++i) {
            std::vector<uint8_t> dummy;
            // Use short timeout (1000ms) to avoid hanging on stuck slots
            SendToSlot(i, nullptr, 0, MsgType::SHUTDOWN, dummy, 1000);
        }
    }

    /**
     * @brief Shuts down the host, closing all handles and unmapping memory.
     */
    void Shutdown() {
        if (!running) return;

        Stop(); // Stop background worker if active

        for (uint32_t i = 0; i < slots.size(); ++i) {
             Platform::CloseEvent(slots[i].hReqEvent);
             Platform::CloseEvent(slots[i].hRespEvent);

             std::string reqName = shmName + "_slot_" + std::to_string(i);
             std::string respName = shmName + "_slot_" + std::to_string(i) + "_resp";
             Platform::UnlinkNamedEvent(reqName.c_str());
             Platform::UnlinkNamedEvent(respName.c_str());
        }

        if (shmBase) Platform::CloseShm(hMapFile, shmBase, totalShmSize);
        Platform::UnlinkShm(shmName.c_str());
        running = false;
    }

    /**
     * @brief Acquires a free slot for Zero-Copy usage.
     * Blocks until a slot is available using the same adaptive strategy as Send.
     *
     * @return The index of the acquired slot, or -1 if not running.
     */
    int32_t AcquireSlot() {
        if (!running) return -1;

        static thread_local uint32_t cachedSlotIdx = 0xFFFFFFFF;
        Slot* slot = nullptr;
        int32_t resultIdx = -1;

        // Fast Path: Try cached slot
        if (cachedSlotIdx < numSlots) {
            Slot& s = slots[cachedSlotIdx];
            uint32_t current = s.header->state.load(std::memory_order_acquire);
            bool canClaim = (current == SLOT_FREE);
            if (!canClaim) {
                 // Recovery logic:
                 // 1. SLOT_RESP_READY or SLOT_REQ_READY and Host not waiting -> Previous transaction abandoned.
                 // 2. SLOT_GUEST_BUSY and Host not waiting -> Guest crashed or timed out during transaction.
                 if (current == SLOT_RESP_READY || current == SLOT_REQ_READY || current == SLOT_GUEST_BUSY) {
                      if (!s.activeWait.load(std::memory_order_acquire)) {
                          canClaim = true;
                      }
                 }
            }

            if (canClaim) {
                if (s.header->state.compare_exchange_strong(current, SLOT_BUSY, std::memory_order_acquire)) {
                    slot = &s;
                    resultIdx = (int32_t)cachedSlotIdx;
                }
            }
        }

        // Slow Path: Search
        if (!slot) {
            int retries = 0;
            uint32_t idx = nextSlot.fetch_add(1, std::memory_order_relaxed) % numSlots;

            while (true) {
                Slot& s = slots[idx];
                uint32_t current = s.header->state.load(std::memory_order_acquire);
                bool canClaim = (current == SLOT_FREE);
                if (!canClaim) {
                     if (current == SLOT_RESP_READY || current == SLOT_REQ_READY || current == SLOT_GUEST_BUSY) {
                          if (!s.activeWait.load(std::memory_order_acquire)) {
                              canClaim = true;
                          }
                     }
                }

                if (canClaim) {
                    if (s.header->state.compare_exchange_strong(current, SLOT_BUSY, std::memory_order_acquire)) {
                        slot = &s;
                        cachedSlotIdx = idx; // Update Cache
                        resultIdx = (int32_t)idx;
                        break;
                    }
                }
                idx = (idx + 1) % numSlots;
                retries++;
                if (retries > (int)numSlots * 100) {
                    Platform::ThreadYield();
                    retries = 0;
                }
            }
        }
        return resultIdx;
    }

    /**
     * @brief Acquires a ZeroCopySlot wrapper.
     * Use this for convenient Zero-Copy FlatBuffer operations.
     *
     * @return ZeroCopySlot object. Check .IsValid() before use.
     */
    ZeroCopySlot GetZeroCopySlot() {
        int32_t idx = AcquireSlot();
        return ZeroCopySlot(this, idx);
    }

    /**
     * @brief Acquires a specific slot.
     * @param slotIdx The index of the slot to acquire.
     * @param timeoutMs Timeout in milliseconds. Default USE_DEFAULT_TIMEOUT.
     * @return The slot index (same as input), or -1 if failed.
     */
    int32_t AcquireSpecificSlot(int32_t slotIdx, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        if (!running || slotIdx < 0 || slotIdx >= (int32_t)numSlots) return -1;
        Slot* slot = &slots[slotIdx];

        uint32_t effectiveTimeout = (timeoutMs == USE_DEFAULT_TIMEOUT) ? responseTimeoutMs : timeoutMs;
        auto start = std::chrono::steady_clock::now();
        int retries = 0;

        while(true) {
             uint32_t current = slot->header->state.load(std::memory_order_acquire);
             bool canClaim = (current == SLOT_FREE);
             if (!canClaim && (current == SLOT_RESP_READY || current == SLOT_REQ_READY)) {
                  if (!slot->activeWait.load(std::memory_order_acquire)) {
                      canClaim = true;
                  }
             }

             if (canClaim) {
                 if (slot->header->state.compare_exchange_strong(current, SLOT_BUSY, std::memory_order_acquire)) {
                     break;
                 }
             }

             Platform::CpuRelax();
             retries++;
             if (retries > 1000) {
                 if (effectiveTimeout != 0xFFFFFFFF) {
                     auto now = std::chrono::steady_clock::now();
                     auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
                     if (elapsed >= effectiveTimeout) {
                         return -1;
                     }
                 }

                 Platform::ThreadYield();
                 retries = 0;
             }
        }
        return slotIdx;
    }

    /**
     * @brief Gets the request buffer pointer for an acquired slot.
     * @param slotIdx The slot index.
     * @return Pointer to the buffer, or nullptr if invalid.
     */
    uint8_t* GetReqBuffer(int32_t slotIdx) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return nullptr;
        return slots[slotIdx].reqBuffer;
    }

    /**
     * @brief Gets the max request size for a slot.
     * @param slotIdx The slot index.
     * @return Max size in bytes.
     */
    int32_t GetMaxReqSize(int32_t slotIdx) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return 0;
        return (int32_t)slots[slotIdx].maxReqSize;
    }

    /**
     * @brief Sends a request using an acquired slot (Zero-Copy flow).
     *
     * @param slotIdx The index of the acquired slot.
     * @param size Size of the data. Negative means End-Aligned (Zero-Copy).
     * @param msgType The message Type.
     * @param[out] outResp Vector to store the response data.
     * @param timeoutMs Per-call timeout. Default USE_DEFAULT_TIMEOUT.
     * @return Result<int> Bytes read (response size) on success, or Error on failure.
     */
    Result<int> SendAcquired(int32_t slotIdx, int32_t size, MsgType msgType, std::vector<uint8_t>& outResp, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return Result<int>(Error::InvalidArgs);
        Slot* slot = &slots[slotIdx];

        // Bounds Check
        int32_t absSize = size < 0 ? -size : size;
        if ((uint32_t)absSize > slot->maxReqSize) {
             // Release Slot
             slot->header->state.store(SLOT_FREE, std::memory_order_release);
             return Result<int>(Error::BufferTooSmall);
        }

        slot->header->reqSize = size;
        slot->header->msgType = msgType;
        slot->header->msgSeq = slot->msgSeq;
        slot->msgSeq += msgSeqStride;

        // Perform Signal and Wait
        uint32_t t = (timeoutMs == USE_DEFAULT_TIMEOUT) ? responseTimeoutMs : timeoutMs;
        bool ready = WaitResponse(slot, t);

        if (!ready) {
             // Timeout. Do NOT release slot (leak it) to prevent corruption.
             return Result<int>(Error::Timeout);
        }

        // Read Response
        int resultSize = 0;
        if (ready) {
            int32_t respSize = slot->header->respSize;
            int32_t absResp = respSize < 0 ? -respSize : respSize;

            if ((uint32_t)absResp > slot->maxRespSize) absResp = (int32_t)slot->maxRespSize;

            outResp.resize(absResp);
            if (absResp > 0) {
                if (respSize >= 0) {
                    // Start-aligned
                    memcpy(outResp.data(), slot->respBuffer, absResp);
                } else {
                    // End-aligned (Zero-Copy Guest)
                    uint32_t offset = slot->maxRespSize - absResp;
                    memcpy(outResp.data(), slot->respBuffer + offset, absResp);
                }
            }
            resultSize = absResp;
        }

        // Release Slot
        slot->header->state.store(SLOT_FREE, std::memory_order_release);

        return Result<int>(resultSize);
    }

    /**
     * @brief Sends a request to a specific slot.
     * @param slotIdx The index of the slot to use.
     * @param data Pointer to the request data.
     * @param size Size of the request data.
     * @param msgType The message Type.
     * @param[out] outResp Vector to store the response data.
     * @param timeoutMs Per-call timeout. Default USE_DEFAULT_TIMEOUT.
     * @return Result<int> Bytes read (response size) on success, or Error on failure.
     */
    Result<int> SendToSlot(uint32_t slotIdx, const uint8_t* data, int32_t size, MsgType msgType, std::vector<uint8_t>& outResp, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        int32_t idx = AcquireSpecificSlot((int32_t)slotIdx, timeoutMs);
        if (idx < 0) return Result<int>(Error::ResourceExhausted);

        if (size != 0) {
            if (!data) {
                // Invalid Argument: size > 0 but data is null.
                slots[idx].header->state.store(SLOT_FREE, std::memory_order_release);
                return Result<int>(Error::InvalidArgs);
            }

            int32_t max = GetMaxReqSize(idx);
            uint32_t uAbsSize = (size < 0) ? (0u - (uint32_t)size) : (uint32_t)size;

            if (uAbsSize > (uint32_t)max) {
                // Release Slot
                slots[idx].header->state.store(SLOT_FREE, std::memory_order_release);
                return Result<int>(Error::BufferTooSmall);
            }

            if (size >= 0) {
                memcpy(GetReqBuffer(idx), data, uAbsSize);
            } else {
                // End-aligned
                uint32_t offset = (uint32_t)max - uAbsSize;
                memcpy(GetReqBuffer(idx) + offset, data, uAbsSize);
            }
        }
        return SendAcquired(idx, size, msgType, outResp, timeoutMs);
    }

    /**
     * @brief Sends a request using any available slot.
     * @param data Pointer to the request data.
     * @param size Size of the request data.
     * @param msgType The message Type.
     * @param[out] outResp Vector to store the response data.
     * @param timeoutMs Per-call timeout. Default USE_DEFAULT_TIMEOUT.
     * @return Result<int> Bytes read (response size) on success, or Error on failure.
     */
    Result<int> Send(const uint8_t* data, int32_t size, MsgType msgType, std::vector<uint8_t>& outResp, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        int32_t idx = AcquireSlot();
        if (idx < 0) return Result<int>(Error::ResourceExhausted);

        if (size != 0) {
            if (!data) {
                // Invalid Argument: size > 0 but data is null.
                slots[idx].header->state.store(SLOT_FREE, std::memory_order_release);
                return Result<int>(Error::InvalidArgs);
            }

            int32_t max = GetMaxReqSize(idx);
            uint32_t uAbsSize = (size < 0) ? (0u - (uint32_t)size) : (uint32_t)size;

            if (uAbsSize > (uint32_t)max) {
                // Release Slot
                slots[idx].header->state.store(SLOT_FREE, std::memory_order_release);
                return Result<int>(Error::BufferTooSmall);
            }

            if (size >= 0) {
                memcpy(GetReqBuffer(idx), data, uAbsSize);
            } else {
                // End-aligned
                uint32_t offset = (uint32_t)max - uAbsSize;
                memcpy(GetReqBuffer(idx) + offset, data, uAbsSize);
            }
        }
        return SendAcquired(idx, size, msgType, outResp, timeoutMs);
    }


    using GuestCallHandler = std::function<int32_t(const uint8_t*, int32_t, uint8_t*, uint32_t, MsgType)>;

private:
    std::thread guestWorker;
    std::atomic<bool> guestWorkerRunning{false};

    void GuestWorkerLoop(GuestCallHandler handler, int32_t maxBatchSize) {
        EventHandle waitEvent = nullptr;
        // Since all guest slots share the same reqEvent (shmName + "_guest_call"),
        // we can wait on the first one.
        if (numGuestSlots > 0 && numSlots + numGuestSlots <= slots.size()) {
             waitEvent = slots[numSlots].hReqEvent;
        }

        while (guestWorkerRunning.load(std::memory_order_relaxed)) {
            // Wait for signal (timeout 1s to check for shutdown)
            if (waitEvent) {
                 Platform::WaitEvent(waitEvent, 1000);
            } else {
                 // Should not happen if Start checks numGuestSlots
                 std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Drain events (Process until empty or limit reached)
            // Note: On Linux semaphores, WaitEvent consumes 1 count.
            // If multiple requests came in, sem value > 0.
            // However, ProcessGuestCalls iterates ALL slots.
            // So one Wakeup might process N requests.
            // Subsequent WaitEvents might return immediately (spurious wakeups), which is fine.
            int processed = ProcessGuestCalls(handler, maxBatchSize);
            (void)processed;
        }
    }

public:
    /**
     * @brief Starts the background worker for Guest Calls.
     *
     * @param handler The handler to process requests.
     * @param maxBatchSize Max requests to process in one burst (default 100).
     */
    void Start(GuestCallHandler handler, int32_t maxBatchSize = 100) {
        if (numGuestSlots == 0) return;
        if (guestWorkerRunning.exchange(true)) return;
        guestWorker = std::thread(&DirectHost::GuestWorkerLoop, this, handler, maxBatchSize);
    }

    /**
     * @brief Stops the background worker.
     */
    void Stop() {
        if (guestWorkerRunning.exchange(false)) {
            if (guestWorker.joinable()) {
                guestWorker.join();
            }
        }
    }

    /**
     * @brief Processes any pending Guest Calls (Guest -> Host).
     * This method is intended to be called in a loop by a background thread.
     *
     * @param handler A function to process the request.
     *                Args: reqData, reqSize, respBuffer, maxRespSize, msgType.
     *                Returns: respSize.
     * @param limit Max number of requests to process per call. -1 for unlimited.
     * @return int Number of requests processed.
     */
    int ProcessGuestCalls(GuestCallHandler handler, int limit = -1) {
        if (!running) return 0;
        int processed = 0;

        for (uint32_t i = numSlots; i < numSlots + numGuestSlots; ++i) {
            if (limit > 0 && processed >= limit) break;
            Slot* slot = &slots[i];

            // Check for REQ_READY (Guest wrote request)
            // Use CAS to claim the slot (transition to BUSY) to prevent race conditions
            // if ProcessGuestCalls is called from multiple threads or recursively.
            uint32_t expected = SLOT_REQ_READY;
            if (slot->header->state.compare_exchange_strong(expected, SLOT_BUSY, std::memory_order_acq_rel)) {
                // Read Request
                int32_t reqSize = slot->header->reqSize;
                const uint8_t* reqData = nullptr;
                int32_t absReqSize = reqSize < 0 ? -reqSize : reqSize;

                // Validate Size
                if ((uint32_t)absReqSize > slot->maxReqSize) {
                    // Invalid size. Clear slot and signal empty response to prevent crash.
                    slot->header->respSize = 0;
                    slot->header->state.store(SLOT_RESP_READY, std::memory_order_seq_cst);
                    Platform::SignalEvent(slot->hRespEvent);
                    processed++;
                    continue;
                }

                if (reqSize >= 0) {
                     reqData = slot->reqBuffer;
                } else {
                     // End-aligned (Zero-Copy Guest)
                     uint32_t offset = slot->maxReqSize - absReqSize;
                     reqData = slot->reqBuffer + offset;
                }

                // Invoke Handler
                int32_t respSize = 0;
                if (handler) {
                    respSize = handler(reqData, absReqSize, slot->respBuffer, slot->maxRespSize, slot->header->msgType);
                }

                // Validate Response Size
                int32_t absRespSize = respSize < 0 ? -respSize : respSize;
                if ((uint32_t)absRespSize > slot->maxRespSize) {
                    // Overflow: Signal error by returning 0 size.
                    // Prevents sending truncated/corrupt data to Guest.
                    respSize = 0;
                }

                // Write Response Metadata
                slot->header->respSize = respSize;

                // State Transition: BUSY -> RESP_READY
                slot->header->state.store(SLOT_RESP_READY, std::memory_order_seq_cst);

                // Signal Guest (Guest waits on RespEvent)
                Platform::SignalEvent(slot->hRespEvent);

                processed++;
            }
        }
        return processed;
    }
};

}
