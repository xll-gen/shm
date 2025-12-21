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
#include "Logger.h"

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
    ShmHandle hLockFile;
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

    // Shared state to track DirectHost lifetime for ZeroCopySlots
    struct SharedState {};
    std::shared_ptr<SharedState> sharedState;

    /**
     * @brief Internal helper to wait for a response on a specific slot.
     * Contains the adaptive spin/yield/wait logic.
     * @param slot Pointer to the slot to wait on.
     * @param timeoutMs Timeout in milliseconds.
     * @return true if response is ready, false if error/timeout.
     */
    bool WaitResponse(Slot* slot, uint32_t timeoutMs) {
        slot->activeWait.store(true, std::memory_order_relaxed);

        slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);

        slot->header->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

        if (slot->header->guestState.load(std::memory_order_seq_cst) == GUEST_STATE_WAITING) {
            Platform::SignalEvent(slot->hReqEvent);
        }

        auto checkReady = [&]() -> bool {
            return slot->header->state.load(std::memory_order_acquire) == SLOT_RESP_READY;
        };

        auto sleepAction = [&]() {
            slot->header->hostState.store(HOST_STATE_WAITING, std::memory_order_seq_cst);

            if (checkReady()) {
                slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
                return;
            }

            auto start = std::chrono::steady_clock::now();
            while (!checkReady()) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

                if (timeoutMs != 0xFFFFFFFF && elapsed >= timeoutMs) {
                    SHM_LOG_DEBUG("WaitResponse timeout after ", elapsed, "ms");
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
    /** @brief Helper class for managing a Zero-Copy slot. */
    static const uint32_t USE_DEFAULT_TIMEOUT = 0xFFFFFFFF;

    class ZeroCopySlot {
        DirectHost* host;
        int32_t slotIdx;
        std::weak_ptr<SharedState> weakState;

    public:
        /** @brief Constructs a ZeroCopySlot. */
        ZeroCopySlot(DirectHost* h, int32_t idx) : host(h), slotIdx(idx) {
            if (h) weakState = h->sharedState;
        }

        /** @brief Destructor. Releases the slot if not already moved. */
        ~ZeroCopySlot() {
            if (auto lock = weakState.lock()) {
                if (host && slotIdx >= 0) {
                    host->slots[slotIdx].header->state.store(SLOT_FREE, std::memory_order_release);
                }
            }
        }

        ZeroCopySlot(ZeroCopySlot&& other) noexcept : host(other.host), slotIdx(other.slotIdx), weakState(std::move(other.weakState)) {
            other.host = nullptr;
            other.slotIdx = -1;
        }

        ZeroCopySlot& operator=(ZeroCopySlot&& other) noexcept {
             if (this != &other) {
                 if (auto lock = weakState.lock()) {
                     if (host && slotIdx >= 0) {
                         host->slots[slotIdx].header->state.store(SLOT_FREE, std::memory_order_release);
                     }
                 }
                 host = other.host;
                 slotIdx = other.slotIdx;
                 weakState = std::move(other.weakState);
                 other.host = nullptr;
                 other.slotIdx = -1;
             }
             return *this;
        }

        ZeroCopySlot(const ZeroCopySlot&) = delete;
        ZeroCopySlot& operator=(const ZeroCopySlot&) = delete;

        /** @brief Checks if the slot is valid. */
        bool IsValid() const {
             return !weakState.expired() && host && slotIdx >= 0;
        }

        /** @brief Gets the pointer to the Request Buffer. */
        uint8_t* GetReqBuffer() {
            if (!IsValid()) return nullptr;
            return host->slots[slotIdx].reqBuffer;
        }

        /** @brief Gets the maximum size of the Request Buffer. */
        int32_t GetMaxReqSize() {
             if (!IsValid()) return 0;
             return host->slots[slotIdx].maxReqSize;
        }

        /**
         * @brief Sends a request using the zero-copy buffer.
         * @return Result<void> Success or Error.
         */
        Result<void> Send(int32_t size, MsgType msgType, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
            if (!IsValid()) return Result<void>::Failure(Error::InvalidArgs);

            Slot* slot = &host->slots[slotIdx];

            uint32_t absSize = (size < 0) ? (0u - (uint32_t)size) : (uint32_t)size;
            if (absSize > slot->maxReqSize) {
                return Result<void>::Failure(Error::BufferTooSmall);
            }

            slot->header->reqSize = size;
            slot->header->msgType = msgType;
            uint32_t currentSeq = slot->msgSeq;
            slot->header->msgSeq = currentSeq;
            slot->msgSeq += host->msgSeqStride;

            uint32_t t = (timeoutMs == USE_DEFAULT_TIMEOUT) ? host->responseTimeoutMs : timeoutMs;
            if (!host->WaitResponse(slot, t)) {
                slotIdx = -1;
                return Result<void>::Failure(Error::Timeout);
            }

            if (slot->header->msgSeq != currentSeq) {
                 slotIdx = -1;
                 return Result<void>::Failure(Error::ProtocolViolation);
            }

            if (slot->header->msgType == MsgType::SYSTEM_ERROR) {
                 host->slots[slotIdx].header->state.store(SLOT_FREE, std::memory_order_release);
                 slotIdx = -1;
                 return Result<void>::Failure(Error::InternalError);
            }

            return Result<void>::Success();
        }

        /**
         * @brief Sends the FlatBuffer request.
         * @return Result<void> Success or Error.
         */
        Result<void> SendFlatBuffer(int32_t size, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
            if (!IsValid()) return Result<void>::Failure(Error::InvalidArgs);

            Slot* slot = &host->slots[slotIdx];

            int32_t absSize = size;
            if ((uint32_t)absSize > slot->maxReqSize) {
                return Result<void>::Failure(Error::BufferTooSmall);
            }

            slot->header->reqSize = -absSize;
            slot->header->msgType = MsgType::FLATBUFFER;
            uint32_t currentSeq = slot->msgSeq;
            slot->header->msgSeq = currentSeq;
            slot->msgSeq += host->msgSeqStride;

            uint32_t t = (timeoutMs == USE_DEFAULT_TIMEOUT) ? host->responseTimeoutMs : timeoutMs;
            if (!host->WaitResponse(slot, t)) {
                slotIdx = -1;
                return Result<void>::Failure(Error::Timeout);
            }

            if (slot->header->msgSeq != currentSeq) {
                 slotIdx = -1;
                 return Result<void>::Failure(Error::ProtocolViolation);
            }

            if (slot->header->msgType == MsgType::SYSTEM_ERROR) {
                 host->slots[slotIdx].header->state.store(SLOT_FREE, std::memory_order_release);
                 slotIdx = -1;
                 return Result<void>::Failure(Error::InternalError);
            }

            return Result<void>::Success();
        }

        /** @brief Gets the pointer to the Response Buffer. */
        uint8_t* GetRespBuffer() {
             if (!IsValid()) return nullptr;
             Slot* slot = &host->slots[slotIdx];
             int32_t rSize = slot->header->respSize;

             if (rSize >= 0) return slot->respBuffer;

             uint32_t absRSize = (0u - (uint32_t)rSize);
             if (absRSize > slot->maxRespSize) absRSize = slot->maxRespSize;

             uint32_t offset = slot->maxRespSize - absRSize;
             return slot->respBuffer + offset;
        }

        /** @brief Gets the size of the Response data. */
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
        DirectHost() : shmBase(nullptr), hMapFile(0), hLockFile(0), running(false), msgSeqStride(0), responseTimeoutMs(10000) {
            sharedState = std::make_shared<SharedState>();
        }

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
        // Ensure sharedState is valid for this new session
        if (!sharedState) {
            sharedState = std::make_shared<SharedState>();
        }

        SHM_LOG_INFO("Initializing DirectHost with shmName: ", config.shmName,
                     ", numHostSlots: ", config.numHostSlots,
                     ", numGuestSlots: ", config.numGuestSlots,
                     ", payloadSize: ", config.payloadSize);

        this->shmName = config.shmName;
        this->numSlots = config.numHostSlots;
        this->numGuestSlots = config.numGuestSlots;
        this->slotSize = config.payloadSize;

        this->msgSeqStride = numSlots + numGuestSlots;

        uint32_t halfSize = slotSize / 2;
        halfSize = (halfSize / 64) * 64;
        if (halfSize < 64) halfSize = 64;

        uint32_t reqOffset = 0;
        uint32_t respOffset = halfSize;

        if (respOffset + halfSize > slotSize) {
             slotSize = respOffset + halfSize;
        }

        size_t exchangeHeaderSize = sizeof(ExchangeHeader);
        if (exchangeHeaderSize < 64) exchangeHeaderSize = 64;

        size_t slotHeaderSize = sizeof(SlotHeader);

        size_t perSlotTotal = slotHeaderSize + slotSize;
        size_t totalSlots = numSlots + numGuestSlots;
        size_t totalSize = exchangeHeaderSize + (perSlotTotal * totalSlots);
        this->totalShmSize = totalSize;

        bool exists = false;
        shmBase = Platform::CreateNamedShm(shmName.c_str(), totalSize, hMapFile, exists);
        if (!shmBase) return Result<void>::Failure(Error::InternalError);

        if (!Platform::LockShm(hMapFile, shmName.c_str(), hLockFile)) {
            SHM_LOG_ERROR("Failed to acquire lock on SHM: ", shmName, ". Another Host might be running.");
            Platform::CloseShm(hMapFile, shmBase, totalShmSize);
            shmBase = nullptr;
            return Result<void>::Failure(Error::ResourceExhausted);
        }

        memset(shmBase, 0, totalSize);

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

            slots[i].msgSeq = i + 1;

            std::string reqName;
            if (i < numSlots) {
                reqName = shmName + "_slot_" + std::to_string(i);
            } else {
                reqName = shmName + "_guest_call";
            }
            std::string respName = shmName + "_slot_" + std::to_string(i) + "_resp";

            slots[i].hReqEvent = Platform::CreateNamedEvent(reqName.c_str());
            slots[i].hRespEvent = Platform::CreateNamedEvent(respName.c_str());

            if (!slots[i].hReqEvent || !slots[i].hRespEvent) {
                SHM_LOG_ERROR("Failed to create events for slot ", i);
                running = true; // Enable Shutdown
                Shutdown();
                return Result<void>::Failure(Error::InternalError);
            }

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

    /** @brief Sends shutdown signal to all guest workers. */
    void SendShutdown() {
        if (!running) return;

        for (uint32_t i = 0; i < numSlots; ++i) {
            std::vector<uint8_t> dummy;
            SendToSlot(i, nullptr, 0, MsgType::SHUTDOWN, dummy, 1000);
        }
    }

    /** @brief Shuts down the host, closing all handles and unmapping memory. */
    void Shutdown() {
        if (!running) return;

        // Invalidate all outstanding ZeroCopySlots immediately.
        // This prevents Use-After-Free in ZeroCopySlot destructors if they race with unmapping.
        sharedState.reset();

        SHM_LOG_INFO("Shutting down DirectHost: ", shmName);

        Stop();

        for (uint32_t i = 0; i < slots.size(); ++i) {
             Platform::CloseEvent(slots[i].hReqEvent);
             Platform::CloseEvent(slots[i].hRespEvent);

             std::string reqName = shmName + "_slot_" + std::to_string(i);
             std::string respName = shmName + "_slot_" + std::to_string(i) + "_resp";
             Platform::UnlinkNamedEvent(reqName.c_str());
             Platform::UnlinkNamedEvent(respName.c_str());
        }

        if (shmBase) {
             Platform::UnlockShm(hMapFile, hLockFile);
             Platform::CloseShm(hMapFile, shmBase, totalShmSize);
        }
        Platform::UnlinkShm(shmName.c_str());
        running = false;
    }

    /**
     * @brief Acquires a free slot for Zero-Copy usage.
     * @return The index of the acquired slot, or -1 if not running.
     */
    int32_t AcquireSlot() {
        if (!running) return -1;

        static thread_local uint32_t cachedSlotIdx = 0xFFFFFFFF;
        Slot* slot = nullptr;
        int32_t resultIdx = -1;

        if (cachedSlotIdx < numSlots) {
            Slot& s = slots[cachedSlotIdx];
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
                    resultIdx = (int32_t)cachedSlotIdx;
                }
            }
        }

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
                        cachedSlotIdx = idx;
                        resultIdx = (int32_t)idx;
                        break;
                    }
                }
                idx++;
                if (idx >= numSlots) idx = 0;
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
     * @brief Manually signals the Request Event for a slot.
     * Used for custom protocols like RingBuffer.
     */
    void SignalSlot(int32_t slotIdx) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return;
        Platform::SignalEvent(slots[slotIdx].hReqEvent);
    }

    /**
     * @brief Manually waits on the Response Event for a slot.
     * Used for custom protocols like RingBuffer.
     * @param slotIdx The slot index.
     * @param timeoutMs Timeout in milliseconds.
     * @return true if signaled, false if timeout.
     */
    bool WaitForSlotEvent(int32_t slotIdx, uint32_t timeoutMs = 0xFFFFFFFF) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return false;
        return Platform::WaitEvent(slots[slotIdx].hRespEvent, timeoutMs);
    }

    /**
     * @brief Sends a request asynchronously using an acquired slot.
     * Does not wait for response. The slot remains BUSY until WaitForSlot is called.
     * @return Result<void> Success or Error.
     */
    Result<void> SendAcquiredAsync(int32_t slotIdx, int32_t size, MsgType msgType) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return Result<void>::Failure(Error::InvalidArgs);
        Slot* slot = &slots[slotIdx];

        uint32_t absSize = (size < 0) ? (0u - (uint32_t)size) : (uint32_t)size;
        if (absSize > slot->maxReqSize) {
             slot->header->state.store(SLOT_FREE, std::memory_order_release);
             return Result<void>::Failure(Error::BufferTooSmall);
        }

        slot->header->reqSize = size;
        slot->header->msgType = msgType;
        slot->header->msgSeq = slot->msgSeq;
        slot->msgSeq += msgSeqStride;

        // Mark activeWait to prevent theft during async operation
        slot->activeWait.store(true, std::memory_order_relaxed);

        slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
        slot->header->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

        if (slot->header->guestState.load(std::memory_order_seq_cst) == GUEST_STATE_WAITING) {
            Platform::SignalEvent(slot->hReqEvent);
        }

        return Result<void>::Success();
    }

    /**
     * @brief Waits for a specific slot to receive a response.
     * Completes the transaction started by SendAcquiredAsync.
     * @return Result<int> Response size.
     */
    Result<int> WaitForSlot(int32_t slotIdx, std::vector<uint8_t>& outResp, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return Result<int>(Error::InvalidArgs);
        Slot* slot = &slots[slotIdx];

        auto checkReady = [&]() -> bool {
            return slot->header->state.load(std::memory_order_acquire) == SLOT_RESP_READY;
        };

        uint32_t t = (timeoutMs == USE_DEFAULT_TIMEOUT) ? responseTimeoutMs : timeoutMs;

        // Ensure activeWait is true (it should be from SendAcquiredAsync)
        slot->activeWait.store(true, std::memory_order_relaxed);
        slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);

        auto sleepAction = [&]() {
            slot->header->hostState.store(HOST_STATE_WAITING, std::memory_order_seq_cst);
            if (checkReady()) {
                slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
                return;
            }

            auto start = std::chrono::steady_clock::now();
            while (!checkReady()) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

                if (t != 0xFFFFFFFF && elapsed >= t) {
                    break;
                }

                uint32_t remaining = (t == 0xFFFFFFFF) ? 0xFFFFFFFF : (t - (uint32_t)elapsed);
                uint32_t waitTime = (remaining > 100) ? 100 : remaining;
                if (t == 0xFFFFFFFF) waitTime = 100;

                Platform::WaitEvent(slot->hRespEvent, waitTime);
            }
            slot->header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
        };

        bool ready = slot->waitStrategy.Wait(checkReady, sleepAction);

        slot->activeWait.store(false, std::memory_order_release);

        if (!ready) {
             return Result<int>(Error::Timeout);
        }

        uint32_t expectedSeq = slot->msgSeq - msgSeqStride;
        if (slot->header->msgSeq != expectedSeq) {
             return Result<int>(Error::ProtocolViolation);
        }

        if (slot->header->msgType == MsgType::SYSTEM_ERROR) {
             slot->header->state.store(SLOT_FREE, std::memory_order_release);
             return Result<int>(Error::InternalError);
        }

        int resultSize = 0;
        int32_t respSize = slot->header->respSize;
        uint32_t absResp = (respSize < 0) ? (0u - (uint32_t)respSize) : (uint32_t)respSize;

        if (absResp > slot->maxRespSize) absResp = slot->maxRespSize;

        outResp.resize(absResp);
        if (absResp > 0) {
            if (respSize >= 0) {
                memcpy(outResp.data(), slot->respBuffer, absResp);
            } else {
                uint32_t offset = slot->maxRespSize - absResp;
                memcpy(outResp.data(), slot->respBuffer + offset, absResp);
            }
        }
        resultSize = (int)absResp;

        slot->header->state.store(SLOT_FREE, std::memory_order_release);
        return Result<int>(resultSize);
    }

    /**
     * @brief Sends a request using an acquired slot (Zero-Copy flow).
     * @return Result<int> Bytes read (response size) on success, or Error on failure.
     */
    Result<int> SendAcquired(int32_t slotIdx, int32_t size, MsgType msgType, std::vector<uint8_t>& outResp, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        if (slotIdx < 0 || slotIdx >= (int32_t)numSlots) return Result<int>(Error::InvalidArgs);
        Slot* slot = &slots[slotIdx];

        uint32_t absSize = (size < 0) ? (0u - (uint32_t)size) : (uint32_t)size;
        if (absSize > slot->maxReqSize) {
             slot->header->state.store(SLOT_FREE, std::memory_order_release);
             return Result<int>(Error::BufferTooSmall);
        }

        slot->header->reqSize = size;
        slot->header->msgType = msgType;
        uint32_t currentSeq = slot->msgSeq;
        slot->header->msgSeq = currentSeq;
        slot->msgSeq += msgSeqStride;

        uint32_t t = (timeoutMs == USE_DEFAULT_TIMEOUT) ? responseTimeoutMs : timeoutMs;
        bool ready = WaitResponse(slot, t);

        if (!ready) {
             return Result<int>(Error::Timeout);
        }

        if (slot->header->msgSeq != currentSeq) {
             return Result<int>(Error::ProtocolViolation);
        }

        if (slot->header->msgType == MsgType::SYSTEM_ERROR) {
             slot->header->state.store(SLOT_FREE, std::memory_order_release);
             return Result<int>(Error::InternalError);
        }

        int resultSize = 0;
        if (ready) {
            int32_t respSize = slot->header->respSize;
            uint32_t absResp = (respSize < 0) ? (0u - (uint32_t)respSize) : (uint32_t)respSize;

            if (absResp > slot->maxRespSize) absResp = slot->maxRespSize;

            outResp.resize(absResp);
            if (absResp > 0) {
                if (respSize >= 0) {
                    memcpy(outResp.data(), slot->respBuffer, absResp);
                } else {
                    uint32_t offset = slot->maxRespSize - absResp;
                    memcpy(outResp.data(), slot->respBuffer + offset, absResp);
                }
            }
            resultSize = (int)absResp;
        }

        slot->header->state.store(SLOT_FREE, std::memory_order_release);

        return Result<int>(resultSize);
    }

    /**
     * @brief Sends a request to a specific slot.
     * @return Result<int> Bytes read (response size) on success, or Error on failure.
     */
    Result<int> SendToSlot(uint32_t slotIdx, const uint8_t* data, int32_t size, MsgType msgType, std::vector<uint8_t>& outResp, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        int32_t idx = AcquireSpecificSlot((int32_t)slotIdx, timeoutMs);
        if (idx < 0) return Result<int>(Error::ResourceExhausted);

        if (size != 0) {
            if (!data) {
                slots[idx].header->state.store(SLOT_FREE, std::memory_order_release);
                return Result<int>(Error::InvalidArgs);
            }

            int32_t max = GetMaxReqSize(idx);
            uint32_t uAbsSize = (size < 0) ? (0u - (uint32_t)size) : (uint32_t)size;

            if (uAbsSize > (uint32_t)max) {
                slots[idx].header->state.store(SLOT_FREE, std::memory_order_release);
                return Result<int>(Error::BufferTooSmall);
            }

            if (size >= 0) {
                memcpy(GetReqBuffer(idx), data, uAbsSize);
            } else {
                uint32_t offset = (uint32_t)max - uAbsSize;
                memcpy(GetReqBuffer(idx) + offset, data, uAbsSize);
            }
        }
        return SendAcquired(idx, size, msgType, outResp, timeoutMs);
    }

    /**
     * @brief Sends a request using any available slot.
     * @return Result<int> Bytes read (response size) on success, or Error on failure.
     */
    Result<int> Send(const uint8_t* data, int32_t size, MsgType msgType, std::vector<uint8_t>& outResp, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        int32_t idx = AcquireSlot();
        if (idx < 0) return Result<int>(Error::ResourceExhausted);

        if (size != 0) {
            if (!data) {
                slots[idx].header->state.store(SLOT_FREE, std::memory_order_release);
                return Result<int>(Error::InvalidArgs);
            }

            int32_t max = GetMaxReqSize(idx);
            uint32_t uAbsSize = (size < 0) ? (0u - (uint32_t)size) : (uint32_t)size;

            if (uAbsSize > (uint32_t)max) {
                slots[idx].header->state.store(SLOT_FREE, std::memory_order_release);
                return Result<int>(Error::BufferTooSmall);
            }

            if (size >= 0) {
                memcpy(GetReqBuffer(idx), data, uAbsSize);
            } else {
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
     * @return int Number of requests processed.
     */
    template <typename Handler>
    int ProcessGuestCalls(Handler&& handler, int limit = -1) {
        if (!running) return 0;
        int processed = 0;

        for (uint32_t i = numSlots; i < numSlots + numGuestSlots; ++i) {
            if (limit > 0 && processed >= limit) break;
            Slot* slot = &slots[i];

            uint32_t current = slot->header->state.load(std::memory_order_acquire);
            if (current != SLOT_REQ_READY) continue;

            if (slot->header->state.compare_exchange_strong(current, SLOT_BUSY, std::memory_order_acq_rel)) {
                int32_t reqSize = slot->header->reqSize;
                const uint8_t* reqData = nullptr;
                uint32_t absReqSize = (reqSize < 0) ? (0u - (uint32_t)reqSize) : (uint32_t)reqSize;

                if (absReqSize > slot->maxReqSize) {
                    slot->header->respSize = 0;
                    slot->header->msgType = MsgType::SYSTEM_ERROR;
                    slot->header->state.store(SLOT_RESP_READY, std::memory_order_seq_cst);
                    Platform::SignalEvent(slot->hRespEvent);
                    processed++;
                    continue;
                }

                if (reqSize >= 0) {
                     reqData = slot->reqBuffer;
                } else {
                     uint32_t offset = slot->maxReqSize - absReqSize;
                     reqData = slot->reqBuffer + offset;
                }

                int32_t respSize = handler(reqData, (int32_t)absReqSize, slot->respBuffer, slot->maxRespSize, slot->header->msgType);

                uint32_t absRespSize = (respSize < 0) ? (0u - (uint32_t)respSize) : (uint32_t)respSize;
                if (absRespSize > slot->maxRespSize) {
                    respSize = 0;
                    slot->header->msgType = MsgType::SYSTEM_ERROR;
                }

                slot->header->respSize = respSize;

                slot->header->state.store(SLOT_RESP_READY, std::memory_order_seq_cst);
                Platform::SignalEvent(slot->hRespEvent);

                processed++;
            }
        }
        return processed;
    }
};

}
