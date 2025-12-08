#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <future>
#include <unordered_map>
#include <atomic>
#include "DirectHost.h"
#include "IPCProtocol.h"

namespace shm {

/**
 * @class IPCHost
 * @brief High-level Facade for the IPC Host.
 *
 * Wraps DirectHost to provide an asynchronous request-response model.
 *
 * @note The current implementation adapts the synchronous DirectHost to the Facade API.
 */
class IPCHost {
    struct RequestContext {
        std::promise<std::vector<uint8_t>> promise;
    };

    DirectHost impl;

    // Pending Requests
    // Sharded map to reduce contention
    static const int SHARD_COUNT = 32;
    struct Shard {
        std::mutex mutex;
        std::unordered_map<uint64_t, RequestContext*> requests;
    };
    Shard shards[SHARD_COUNT];

    std::atomic<uint64_t> reqIdCounter{0};

    // Heartbeat
    std::mutex heartbeatMutex;
    std::unique_ptr<std::promise<void>> heartbeatPromise;

public:
    /**
     * @brief Default constructor.
     */
    IPCHost() {}

    /**
     * @brief Destructor.
     */
    ~IPCHost() { Shutdown(); }

    /**
     * @brief Initializes the IPC Host.
     *
     * Initializes the underlying DirectHost and sets up the shared memory region.
     *
     * @param name The name of the shared memory region.
     * @param numSlots The number of worker slots to allocate.
     * @return true if initialization was successful, false otherwise.
     */
    bool Init(const std::string& name, uint32_t numSlots) {
        // DirectHost Init does not take a handler in this version.
        return impl.Init(name, numSlots);
    }

    /**
     * @brief Shuts down the host.
     */
    void Shutdown() {
        SendShutdown(); // Best effort
        impl.Shutdown();
    }

    /**
     * @brief Sends a request to the Guest and awaits a response.
     *
     * This method is thread-safe and can be called concurrently. It wraps the
     * user payload with a TransportHeader to track the request ID.
     *
     * @param reqData Pointer to the request payload data.
     * @param reqSize Size of the request payload in bytes.
     * @param[out] outResponse Reference to a vector where the response payload will be stored.
     * @return true if the call succeeded and the response ID matched, false otherwise.
     */
    bool Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse) {
        uint64_t reqId = ++reqIdCounter;

        // Prepare Buffer: [TransportHeader][UserPayload]
        // We need a contiguous buffer.
        size_t totalSize = sizeof(TransportHeader) + reqSize;

        // Optimize: Small stack buffer? Or thread_local vector?
        // std::vector allocation is acceptable for now.
        std::vector<uint8_t> sendBuf;
        sendBuf.resize(totalSize);

        TransportHeader* header = (TransportHeader*)sendBuf.data();
        header->req_id = reqId;

        if (reqData && reqSize > 0) {
            memcpy(sendBuf.data() + sizeof(TransportHeader), reqData, reqSize);
        }

        RequestContext ctx;
        // Future/Promise not strictly needed for synchronous DirectHost, but kept for structure.

        // Insert into pending map (metadata tracking)
        {
            Shard& shard = shards[reqId % SHARD_COUNT];
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.requests[reqId] = &ctx;
        }

        std::vector<uint8_t> rawResp;
        if (impl.Send(sendBuf.data(), (uint32_t)totalSize, MSG_TYPE_NORMAL, rawResp) < 0) {
             // Failed to send
             Shard& shard = shards[reqId % SHARD_COUNT];
             std::lock_guard<std::mutex> lock(shard.mutex);
             shard.requests.erase(reqId);
             return false;
        }

        // Remove from map
        {
            Shard& shard = shards[reqId % SHARD_COUNT];
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.requests.erase(reqId);
        }

        // Validate Response
        if (rawResp.size() < sizeof(TransportHeader)) {
            return false;
        }

        TransportHeader* respHeader = (TransportHeader*)rawResp.data();
        if (respHeader->req_id != reqId) {
            // ID mismatch
            return false;
        }

        // Strip Header
        outResponse.assign(rawResp.begin() + sizeof(TransportHeader), rawResp.end());
        return true;
    }

    /**
     * @brief Sends a heartbeat request to the Guest.
     *
     * This is used to verify that the Guest is responsive. The Guest should
     * reply with a MSG_TYPE_HEARTBEAT_RESP.
     *
     * @return true if the heartbeat was sent successfully (slot acquired), false otherwise.
     */
    bool SendHeartbeat() {
        // DirectHost is synchronous, so we just check return value.
        std::vector<uint8_t> dummy;
        int res = impl.Send(nullptr, 0, MSG_TYPE_HEARTBEAT_REQ, dummy);
        return res >= 0;
    }

    /**
     * @brief Sends a shutdown signal to all connected Guest workers.
     *
     * This iterates through all slots and sends a MSG_TYPE_SHUTDOWN command.
     * Guest workers are expected to exit their loops upon receiving this.
     */
    void SendShutdown() {
        std::vector<uint8_t> dummy;
        impl.Send(nullptr, 0, MSG_TYPE_SHUTDOWN, dummy);
    }

private:
    /**
     * @brief Callback for processing received messages (Async mode).
     * @note Not used in synchronous DirectHost mode.
     */
    void OnMessage(std::vector<uint8_t>&& data, uint32_t msgId) {
        // Logic moved to Call() for synchronous mode.
    }
};

}
