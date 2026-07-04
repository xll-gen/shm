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
#include "SlotAllocator.h"
#include "GuestCallWorker.h"

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

    /** @brief v0.8.6: whether the guest-call worker runs an adaptive spin phase
     * and publishes HOST_STATE_WAITING before parking (letting the Go sender
     * elide its request-doorbell syscall while the worker is hot). Default true.
     * Set false on hosts that must not dedicate a briefly-spinning background
     * thread to guest-call latency; the worker then uses the sleep-only loop and
     * the sender always signals. No effect when numGuestSlots == 0. */
    bool guestWorkerSpin = true;
};

/**
 * @class DirectHost
 * @brief Implements the Host side of the Direct Mode IPC.
 *
 * The DirectHost manages a pool of slots in shared memory. Each slot is intended
 * to be paired with a specific Guest worker thread.
 * It uses a hybrid spin/wait strategy for low latency and utilizes
 * specific memory layout defined in IPCUtils.h.
 *
 * R44 refactor: the slot state machine has been extracted into SlotAllocator
 * and the guest-call processing into GuestCallWorker. DirectHost is now a thin
 * façade that owns shared-memory mapping, geometry, and lifetime, and delegates
 * the send/acquire/wait API to its SlotAllocator member and the guest-call API
 * to its GuestCallWorker member. The public API is byte-identical to the
 * pre-split class — behavior, memory orders, and ABI are unchanged.
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

    // Config
    uint32_t slotSize; // Total payload size per slot

    // Slot state machine (acquire / reclaim / wait / acquired-send paths).
    // Owns slots, nextSlot, msgSeqStride, responseTimeoutMs, autoReclaimTimeoutNs.
    SlotAllocator allocator_;

    // Guest-call processing (worker thread + ProcessGuestCalls).
    GuestCallWorker worker_;

    // Shared state to track DirectHost lifetime for ZeroCopySlots
    struct SharedState {};
    std::shared_ptr<SharedState> sharedState;

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
                    host->allocator_.slots[slotIdx].header->state.store(SLOT_FREE, std::memory_order_release);
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
                         host->allocator_.slots[slotIdx].header->state.store(SLOT_FREE, std::memory_order_release);
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
            return host->allocator_.slots[slotIdx].reqBuffer;
        }

        /** @brief Gets the maximum size of the Request Buffer. */
        int32_t GetMaxReqSize() {
             if (!IsValid()) return 0;
             return host->allocator_.slots[slotIdx].maxReqSize;
        }

        /**
         * @brief Sends a request using the zero-copy buffer.
         * @return Result<void> Success or Error.
         */
        Result<void> Send(int32_t size, MsgType msgType, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
            if (!IsValid()) return Result<void>::Failure(Error::InvalidArgs);

            Slot* slot = &host->allocator_.slots[slotIdx];

            uint32_t absSize = (size < 0) ? (0u - (uint32_t)size) : (uint32_t)size;
            if (absSize > slot->maxReqSize) {
                return Result<void>::Failure(Error::BufferTooSmall);
            }

            slot->header->reqSize = size;
            slot->header->msgType = msgType;
            uint32_t currentSeq = slot->msgSeq;
            slot->header->msgSeq = currentSeq;
            slot->msgSeq += host->allocator_.msgSeqStride;

            uint32_t t = (timeoutMs == USE_DEFAULT_TIMEOUT) ? host->allocator_.responseTimeoutMs : timeoutMs;
            if (!host->allocator_.WaitResponse(slot, t)) {
                slotIdx = -1;
                return Result<void>::Failure(Error::Timeout);
            }

            if (slot->header->msgSeq != currentSeq) {
                 // Release the slot back to SLOT_FREE before disowning it; otherwise
                 // the slot stays in SLOT_RESP_READY (or worse, in whatever state the
                 // misbehaving peer published) and can only be reclaimed by the
                 // zombie path. Restoring SLOT_FREE here closes that hole.
                 slot->header->state.store(SLOT_FREE, std::memory_order_release);
                 slotIdx = -1;
                 return Result<void>::Failure(Error::ProtocolViolation);
            }

            if (slot->header->msgType == MsgType::SYSTEM_ERROR) {
                 host->allocator_.slots[slotIdx].header->state.store(SLOT_FREE, std::memory_order_release);
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

            Slot* slot = &host->allocator_.slots[slotIdx];

            int32_t absSize = size;
            if ((uint32_t)absSize > slot->maxReqSize) {
                return Result<void>::Failure(Error::BufferTooSmall);
            }

            slot->header->reqSize = -absSize;
            slot->header->msgType = MsgType::FLATBUFFER;
            uint32_t currentSeq = slot->msgSeq;
            slot->header->msgSeq = currentSeq;
            slot->msgSeq += host->allocator_.msgSeqStride;

            uint32_t t = (timeoutMs == USE_DEFAULT_TIMEOUT) ? host->allocator_.responseTimeoutMs : timeoutMs;
            if (!host->allocator_.WaitResponse(slot, t)) {
                slotIdx = -1;
                return Result<void>::Failure(Error::Timeout);
            }

            if (slot->header->msgSeq != currentSeq) {
                 // See identical comment in Send(): release slot back to SLOT_FREE
                 // so it does not remain stuck in SLOT_RESP_READY when the peer
                 // returns a stale msgSeq.
                 slot->header->state.store(SLOT_FREE, std::memory_order_release);
                 slotIdx = -1;
                 return Result<void>::Failure(Error::ProtocolViolation);
            }

            if (slot->header->msgType == MsgType::SYSTEM_ERROR) {
                 host->allocator_.slots[slotIdx].header->state.store(SLOT_FREE, std::memory_order_release);
                 slotIdx = -1;
                 return Result<void>::Failure(Error::InternalError);
            }

            return Result<void>::Success();
        }

        /** @brief Gets the pointer to the Response Buffer. */
        uint8_t* GetRespBuffer() {
             if (!IsValid()) return nullptr;
             Slot* slot = &host->allocator_.slots[slotIdx];
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
             Slot* slot = &host->allocator_.slots[slotIdx];
             int32_t rSize = slot->header->respSize;

             uint32_t absResp = (rSize < 0) ? (0u - (uint32_t)rSize) : (uint32_t)rSize;
             if (absResp > slot->maxRespSize) absResp = slot->maxRespSize;

             return (int32_t)absResp;
        }
    };

    /**
     * @brief RAII wrapper for a held-slot session (v0.8.5).
     *
     * Acquires a slot once (the ordinary gen-bump claim) and re-arms it on
     * every Send via SlotAllocator::SendHeld, skipping the per-send
     * FREE→re-claim cycle — the fast path for callers that own a dedicated
     * slot and send repeatedly (per-thread benchmark workers, xll-gen's
     * per-call sites once migrated off GetZeroCopySlot). Between sends the
     * slot parks at SLOT_BUSY, which tryClaimSlot's zombie branch never
     * steals; only the opt-in lease-based reclaim can take it, so keep any
     * SetAutoReclaimTimeoutNs threshold above the maximum inter-send gap
     * (SPEC §3.6 held-slot note).
     *
     * After a Send error other than BufferTooSmall/InvalidArgs the wrapper
     * disowns the slot (IsValid() turns false) — re-acquire with
     * AcquireHeldSlot. Destruction releases a still-held slot to SLOT_FREE.
     */
    class HeldSlot {
        DirectHost* host;
        int32_t slotIdx;
        std::weak_ptr<SharedState> weakState;

    public:
        HeldSlot() : host(nullptr), slotIdx(-1) {}

        HeldSlot(DirectHost* h, int32_t idx) : host(h), slotIdx(idx) {
            if (h) weakState = h->sharedState;
        }

        ~HeldSlot() { Release(); }

        HeldSlot(HeldSlot&& other) noexcept
            : host(other.host), slotIdx(other.slotIdx), weakState(std::move(other.weakState)) {
            other.host = nullptr;
            other.slotIdx = -1;
        }

        HeldSlot& operator=(HeldSlot&& other) noexcept {
            if (this != &other) {
                Release();
                host = other.host;
                slotIdx = other.slotIdx;
                weakState = std::move(other.weakState);
                other.host = nullptr;
                other.slotIdx = -1;
            }
            return *this;
        }

        HeldSlot(const HeldSlot&) = delete;
        HeldSlot& operator=(const HeldSlot&) = delete;

        /** @brief Checks if the slot is valid (still held). */
        bool IsValid() const {
            return !weakState.expired() && host && slotIdx >= 0;
        }

        /** @brief The underlying slot index, or -1 when not held. */
        int32_t Index() const { return slotIdx; }

        /** @brief Gets the pointer to the Request Buffer. */
        uint8_t* GetReqBuffer() {
            if (!IsValid()) return nullptr;
            return host->allocator_.slots[slotIdx].reqBuffer;
        }

        /** @brief Gets the maximum size of the Request Buffer. */
        int32_t GetMaxReqSize() {
            if (!IsValid()) return 0;
            return host->allocator_.slots[slotIdx].maxReqSize;
        }

        /**
         * @brief Sends the request already written into GetReqBuffer() and
         *        copies the response out, keeping the slot held on success.
         *
         * On Timeout/ProtocolViolation/InternalError the slot is disowned
         * (see SendHeld's error semantics); BufferTooSmall/InvalidArgs leave
         * it held.
         */
        Result<int> Send(int32_t size, MsgType msgType, std::vector<uint8_t>& outResp, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
            if (!IsValid()) return Result<int>(Error::InvalidArgs);

            auto res = host->allocator_.SendHeld(slotIdx, size, msgType, outResp, timeoutMs);
            if (res.HasError()) {
                switch (res.GetError()) {
                    case Error::Timeout:
                    case Error::ProtocolViolation:
                    case Error::InternalError:
                        // SendHeld freed the slot where safe (violation /
                        // system error) or left it in-flight (timeout); in
                        // both cases this wrapper must not touch it again.
                        slotIdx = -1;
                        break;
                    default:
                        break; // still held
                }
            }
            return res;
        }

        /** @brief Releases a still-held slot back to SLOT_FREE. */
        void Release() {
            if (auto lock = weakState.lock()) {
                if (host && slotIdx >= 0) {
                    host->allocator_.ReleaseHeldSlot(slotIdx);
                }
            }
            slotIdx = -1;
        }
    };

    /**
     * @brief Default constructor.
     */
        DirectHost() : shmBase(nullptr), hMapFile(0), hLockFile(0), running(false) {
            // SlotAllocator defaults: msgSeqStride=0, responseTimeoutMs=10000
            // (matching the pre-split DirectHost member initializers).
            allocator_.running = &running;
            worker_.alloc = &allocator_;
            worker_.running = &running;
            sharedState = std::make_shared<SharedState>();
        }

    /**
     * @brief Destructor. Ensures Shutdown is called.
     */
    ~DirectHost() { Shutdown(); }

    // Non-copyable and non-movable. allocator_/worker_ hold raw back-pointers
    // into this object (allocator_.running=&running, worker_.alloc=&allocator_,
    // worker_.running=&running), wired once in the ctor; a copy or move would
    // leave them dangling into the moved-from object in the hottest IPC path.
    // The std::thread/std::atomic members already delete the implicit copy/move,
    // so this is enforced today — make it explicit so a future change to
    // SlotAllocator/GuestCallWorker movability can never silently restore it.
    DirectHost(const DirectHost&) = delete;
    DirectHost& operator=(const DirectHost&) = delete;
    DirectHost(DirectHost&&) = delete;
    DirectHost& operator=(DirectHost&&) = delete;

    /**
     * @brief Sets the timeout for waiting for a response.
     * @param ms Timeout in milliseconds. Default 10000.
     */
    void SetTimeout(uint32_t ms) {
        allocator_.responseTimeoutMs = ms;
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
        allocator_.numSlots = numSlots;
        allocator_.numGuestSlots = numGuestSlots;
        worker_.spin = config.guestWorkerSpin;

        allocator_.msgSeqStride = numSlots + numGuestSlots;

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
        // Guest responder fast-path permission (v0.8.8): allowed only when
        // auto-reclaim is off (the reclaimer is the sole actor the guest's
        // per-RTT gen/claim/lease dance protects against on a host slot). See
        // IPCUtils.h fastPathAllowed and SPECIFICATION.md §3.4. Reclaim policy
        // is startup-time; if it changes, SetAutoReclaimTimeoutNs republishes.
        exHeader->fastPathAllowed.store(
            allocator_.autoReclaimTimeoutNs.load(std::memory_order_relaxed) == 0 ? 1u : 0u,
            std::memory_order_release);

        allocator_.slots.resize(totalSlots);
        uint8_t* ptr = (uint8_t*)shmBase + exchangeHeaderSize;

        for (uint32_t i = 0; i < totalSlots; ++i) {
            allocator_.slots[i].header = (SlotHeader*)ptr;
            uint8_t* dataBase = ptr + slotHeaderSize;
            allocator_.slots[i].reqBuffer = dataBase + reqOffset;
            allocator_.slots[i].respBuffer = dataBase + respOffset;
            allocator_.slots[i].maxReqSize = halfSize;
            allocator_.slots[i].maxRespSize = slotSize - respOffset;
            // waitStrategy initialized by default constructor

            allocator_.slots[i].msgSeq = i + 1;

            std::string reqName;
            if (i < numSlots) {
                reqName = shmName + "_slot_" + std::to_string(i);
            } else {
                reqName = shmName + "_guest_call";
            }
            std::string respName = shmName + "_slot_" + std::to_string(i) + "_resp";

            allocator_.slots[i].hReqEvent = Platform::CreateNamedEvent(reqName.c_str());
            allocator_.slots[i].hRespEvent = Platform::CreateNamedEvent(respName.c_str());

            if (!allocator_.slots[i].hReqEvent || !allocator_.slots[i].hRespEvent) {
                SHM_LOG_ERROR("Failed to create events for slot ", i);
                running = true; // Enable Shutdown
                Shutdown();
                return Result<void>::Failure(Error::InternalError);
            }

            allocator_.slots[i].header->state.store(SLOT_FREE, std::memory_order_relaxed);
            allocator_.slots[i].header->hostState.store(HOST_STATE_ACTIVE, std::memory_order_relaxed);
            allocator_.slots[i].header->guestState.store(GUEST_STATE_ACTIVE, std::memory_order_relaxed);

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

        for (uint32_t i = 0; i < allocator_.slots.size(); ++i) {
             Platform::CloseEvent(allocator_.slots[i].hReqEvent);
             Platform::CloseEvent(allocator_.slots[i].hRespEvent);

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
        return allocator_.AcquireSlot();
    }

    /**
     * @brief Non-blocking slot acquisition (one sweep, no wait/reclaim).
     * @return A claimed slot index, or -1 if the pool was momentarily full.
     */
    int32_t TryAcquireSlot() {
        return allocator_.TryAcquireSlot();
    }

    /**
     * @brief Acquires any free slot as a held-slot session, non-blocking.
     * @return HeldSlot wrapper; check .IsValid() (false when the pool is full).
     */
    HeldSlot TryAcquireHeldSlot() {
        return HeldSlot(this, TryAcquireSlot());
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
     * @brief Acquires any free slot as a held-slot session (see HeldSlot).
     * @return HeldSlot wrapper. Check .IsValid() before use.
     */
    HeldSlot AcquireHeldSlot() {
        return HeldSlot(this, AcquireSlot());
    }

    /**
     * @brief Acquires a specific slot as a held-slot session (see HeldSlot).
     * @param slotIdx The slot index to acquire.
     * @param timeoutMs Timeout in milliseconds. Default USE_DEFAULT_TIMEOUT.
     * @return HeldSlot wrapper. Check .IsValid() before use.
     */
    HeldSlot AcquireHeldSlot(int32_t slotIdx, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        return HeldSlot(this, AcquireSpecificSlot(slotIdx, timeoutMs));
    }

    /**
     * @brief Acquires a specific slot.
     * @param slotIdx The index of the slot to acquire.
     * @param timeoutMs Timeout in milliseconds. Default USE_DEFAULT_TIMEOUT.
     * @return The slot index (same as input), or -1 if failed.
     */
    int32_t AcquireSpecificSlot(int32_t slotIdx, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        return allocator_.AcquireSpecificSlot(slotIdx, timeoutMs);
    }

    /**
     * @brief Enable AcquireSlot auto-reclaim with the given lease-age threshold.
     *
     * When AcquireSlot's slow-path slot scan completes a full sweep
     * without finding a free slot, it walks every non-FREE slot and
     * calls `TryReclaimAbandonedSlot(idx, timeoutNs)`. Zero disables
     * auto-reclaim entirely (the default — opt-in for safety).
     *
     * Typical values: 5× `responseTimeoutMs` converted to ns. Too short
     * and you race a slow-but-live peer (CAS guard catches it, but you
     * burn cycles); too long and you wait forever when a peer truly
     * crashed.
     *
     * Safe to call at any time; thread-safe atomic store.
     *
     * @param timeoutNs Threshold in nanoseconds. 0 = disabled.
     */
    void SetAutoReclaimTimeoutNs(uint64_t timeoutNs) {
        allocator_.SetAutoReclaimTimeoutNs(timeoutNs);
        // Republish the guest fast-path permission: enabling reclaim (nonzero)
        // MUST revoke the guest's no-claim fast path (0) so its per-RTT
        // gen/claim/lease dance resumes before any reclaim can fire. Reclaim
        // policy is startup-time (set before traffic), so the guest — which
        // reads this once at attach — sees the final value; a runtime flip
        // after the guest has attached is not honored until re-attach (SPEC
        // §3.4 / §3.6 held-slot lease contract).
        if (shmBase) {
            ((ExchangeHeader*)shmBase)->fastPathAllowed.store(
                timeoutNs == 0 ? 1u : 0u, std::memory_order_release);
        }
    }

    /**
     * @brief Returns the current auto-reclaim threshold (0 = disabled).
     */
    uint64_t GetAutoReclaimTimeoutNs() const {
        return allocator_.GetAutoReclaimTimeoutNs();
    }

    /**
     * @brief Attempt to reclaim a slot whose lease is older than `maxLeaseAgeNs`.
     *
     * For crash-recovery: if the slot's current owner has crashed, its
     * lease will not refresh and the slot will sit in a non-FREE state
     * forever. This method observes the current state and lease, and if
     * `now - lease > maxLeaseAgeNs` it tries to CAS the state back to
     * SLOT_FREE.
     *
     * See SlotAllocator::TryReclaimAbandonedSlot for the full ABA-hazard and
     * claim-generation-handshake discussion (§3.6.1).
     *
     * @param slotIdx The slot to inspect.
     * @param maxLeaseAgeNs Maximum tolerated age in nanoseconds. Slots
     *                     whose lease is older than this are candidates
     *                     for reclamation.
     * @return true if the slot was reclaimed.
     */
    bool TryReclaimAbandonedSlot(int32_t slotIdx, uint64_t maxLeaseAgeNs) {
        return allocator_.TryReclaimAbandonedSlot(slotIdx, maxLeaseAgeNs);
    }

    /**
     * @brief Gets the request buffer pointer for an acquired slot.
     * @param slotIdx The slot index.
     * @return Pointer to the buffer, or nullptr if invalid.
     */
    uint8_t* GetReqBuffer(int32_t slotIdx) {
        return allocator_.GetReqBuffer(slotIdx);
    }

    /**
     * @brief Gets the max request size for a slot.
     * @param slotIdx The slot index.
     * @return Max size in bytes.
     */
    int32_t GetMaxReqSize(int32_t slotIdx) {
        return allocator_.GetMaxReqSize(slotIdx);
    }

    /**
     * @brief Manually signals the Request Event for a slot.
     * Used for custom protocols like RingBuffer.
     */
    void SignalSlot(int32_t slotIdx) {
        allocator_.SignalSlot(slotIdx);
    }

    /**
     * @brief Manually waits on the Response Event for a slot.
     * Used for custom protocols like RingBuffer.
     * @param slotIdx The slot index.
     * @param timeoutMs Timeout in milliseconds.
     * @return true if signaled, false if timeout.
     */
    bool WaitForSlotEvent(int32_t slotIdx, uint32_t timeoutMs = 0xFFFFFFFF) {
        return allocator_.WaitForSlotEvent(slotIdx, timeoutMs);
    }

    /**
     * @brief Sends a request asynchronously using an acquired slot.
     * Does not wait for response. The slot remains BUSY until WaitForSlot is called.
     * @return Result<void> Success or Error.
     */
    Result<void> SendAcquiredAsync(int32_t slotIdx, int32_t size, MsgType msgType) {
        return allocator_.SendAcquiredAsync(slotIdx, size, msgType);
    }

    /**
     * @brief Waits for a specific slot to receive a response.
     * Completes the transaction started by SendAcquiredAsync.
     * @return Result<int> Response size.
     */
    Result<int> WaitForSlot(int32_t slotIdx, std::vector<uint8_t>& outResp, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        return allocator_.WaitForSlot(slotIdx, outResp, timeoutMs);
    }

    /**
     * @brief Sends a request using an acquired slot (Zero-Copy flow).
     * @return Result<int> Bytes read (response size) on success, or Error on failure.
     */
    Result<int> SendAcquired(int32_t slotIdx, int32_t size, MsgType msgType, std::vector<uint8_t>& outResp, uint32_t timeoutMs = USE_DEFAULT_TIMEOUT) {
        return allocator_.SendAcquired(slotIdx, size, msgType, outResp, timeoutMs);
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
                allocator_.slots[idx].header->state.store(SLOT_FREE, std::memory_order_release);
                return Result<int>(Error::InvalidArgs);
            }

            int32_t max = GetMaxReqSize(idx);
            uint32_t uAbsSize = (size < 0) ? (0u - (uint32_t)size) : (uint32_t)size;

            if (uAbsSize > (uint32_t)max) {
                allocator_.slots[idx].header->state.store(SLOT_FREE, std::memory_order_release);
                return Result<int>(Error::BufferTooSmall);
            }

            if (size >= 0) {
                Platform::CopySmall(GetReqBuffer(idx), data, uAbsSize);
            } else {
                uint32_t offset = (uint32_t)max - uAbsSize;
                Platform::CopySmall(GetReqBuffer(idx) + offset, data, uAbsSize);
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
                allocator_.slots[idx].header->state.store(SLOT_FREE, std::memory_order_release);
                return Result<int>(Error::InvalidArgs);
            }

            int32_t max = GetMaxReqSize(idx);
            uint32_t uAbsSize = (size < 0) ? (0u - (uint32_t)size) : (uint32_t)size;

            if (uAbsSize > (uint32_t)max) {
                allocator_.slots[idx].header->state.store(SLOT_FREE, std::memory_order_release);
                return Result<int>(Error::BufferTooSmall);
            }

            if (size >= 0) {
                Platform::CopySmall(GetReqBuffer(idx), data, uAbsSize);
            } else {
                uint32_t offset = (uint32_t)max - uAbsSize;
                Platform::CopySmall(GetReqBuffer(idx) + offset, data, uAbsSize);
            }
        }
        return SendAcquired(idx, size, msgType, outResp, timeoutMs);
    }


    using GuestCallHandler = GuestCallWorker::GuestCallHandler;

    /**
     * @brief Starts the background worker for Guest Calls.
     *
     * @param handler The handler to process requests.
     * @param maxBatchSize Max requests to process in one burst (default 100).
     */
    void Start(GuestCallHandler handler, int32_t maxBatchSize = 100) {
        worker_.Start(handler, maxBatchSize);
    }

    /**
     * @brief Stops the background worker.
     */
    void Stop() {
        worker_.Stop();
    }

    /**
     * @brief Processes any pending Guest Calls (Guest -> Host).
     * @return int Number of requests processed.
     */
    template <typename Handler>
    int ProcessGuestCalls(Handler&& handler, int limit = -1) {
        return worker_.ProcessGuestCalls(std::forward<Handler>(handler), limit);
    }
};

}
