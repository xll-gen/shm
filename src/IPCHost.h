#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <future>
#include <unordered_map>
#include <atomic>
#include "QueueHost.h"
#include "DirectHost.h"
#include "IPCProtocol.h"

namespace shm {

/**
 * @brief Enumeration of available IPC operating modes.
 */
enum class IPCMode {
    Queue,  ///< SPSC Ring Buffer Mode (Streaming).
    Direct  ///< Slot-based Direct Mode (Low Latency).
};

/**
 * @brief High-level C++ Host Facade.
 *
 * Provides a unified API for interacting with the IPC system, regardless
 * of the underlying mode (Queue vs Direct).
 *
 * Features:
 * - Runtime mode selection.
 * - Asynchronous request/response matching via TransportHeader.
 * - Thread-safe calling mechanism (`Call`).
 * - Heartbeat and Shutdown control messages.
 */
class IPCHost {
    struct RequestContext {
        std::promise<std::vector<uint8_t>> promise;
    };

    /**
     * @brief Abstract internal interface for mode-specific implementations.
     */
    class HostImpl {
    public:
        virtual ~HostImpl() = default;
        virtual bool Init(const std::string& name, uint64_t size,
                          std::function<void(std::vector<uint8_t>&&, uint32_t)> cb) = 0;
        virtual void Send(const uint8_t* data, uint32_t size, uint32_t msgId) = 0;
        virtual void Shutdown() = 0;
    };

    /**
     * @brief Wrapper for DirectHost to adapt to HostImpl interface.
     */
    class DirectWrapper : public HostImpl {
        DirectHost impl;
    public:
        bool Init(const std::string& name, uint64_t size,
                  std::function<void(std::vector<uint8_t>&&, uint32_t)> cb) override {
             // Heuristic mapping of generic 'size' param to 'numQueues'.
             // If size < 256, treat as numQueues. Else default to 4.
             uint32_t numQueues = (size < 256) ? (uint32_t)size : 4;
             return impl.Init(name, numQueues, cb);
        }
        void Send(const uint8_t* data, uint32_t size, uint32_t msgId) override {
            impl.Send(data, size, msgId);
        }
        void Shutdown() override {
            impl.Shutdown();
        }
    };

    /**
     * @brief Wrapper for QueueHost to adapt to HostImpl interface.
     */
    class QueueWrapper : public HostImpl {
        QueueHost<SPSCQueue> impl;
    public:
        bool Init(const std::string& name, uint64_t size,
                  std::function<void(std::vector<uint8_t>&&, uint32_t)> cb) override {
             if (size < 1024) size = 32 * 1024 * 1024; // Default 32MB if param looks like 'numQueues'
             return impl.Init(name, size, cb);
        }
        void Send(const uint8_t* data, uint32_t size, uint32_t msgId) override {
            impl.Send(data, size, msgId);
        }
        void Shutdown() override {
            impl.Shutdown();
        }
    };

    std::unique_ptr<HostImpl> impl;

    // Pending Requests Map Sharding
    static const int SHARD_COUNT = 32;
    struct Shard {
        std::mutex mutex;
        std::unordered_map<uint64_t, RequestContext*> requests;
    };
    Shard shards[SHARD_COUNT];

    std::atomic<uint64_t> reqIdCounter{0};

    // Heartbeat Management
    std::mutex heartbeatMutex;
    std::unique_ptr<std::promise<void>> heartbeatPromise;

public:
    /** @brief Constructor. */
    IPCHost() {}

    /** @brief Destructor. Calls Shutdown. */
    ~IPCHost() { Shutdown(); }

    /**
     * @brief Initializes the IPC Host.
     *
     * @param name Unique name for the IPC channel.
     * @param mode IPCMode::Queue or IPCMode::Direct.
     * @param param Configuration parameter (Queue Size in bytes OR Number of Queues/Lanes).
     * @return true on success.
     */
    bool Init(const std::string& name, IPCMode mode, uint64_t param = 0) {
        auto handler = [this](std::vector<uint8_t>&& data, uint32_t msgId) {
            this->OnMessage(std::move(data), msgId);
        };

        if (mode == IPCMode::Direct) {
            impl = std::unique_ptr<HostImpl>(new DirectWrapper());
        } else {
            impl = std::unique_ptr<HostImpl>(new QueueWrapper());
        }
        return impl->Init(name, param, handler);
    }

    /**
     * @brief Shuts down the host.
     *
     * Sends a shutdown signal to the Guest and cleans up resources.
     */
    void Shutdown() {
        if (impl) {
            SendShutdown(); // Best effort signal
            impl->Shutdown();
            impl.reset();
        }
    }

    /**
     * @brief Sends a request and blocks waiting for a response.
     *
     * Automatically prepends the TransportHeader with a unique ID.
     *
     * @param reqData Pointer to request payload.
     * @param reqSize Size of request payload.
     * @param[out] outResponse Vector to store the received response (header stripped).
     * @return true if successful (response received).
     */
    bool Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse) {
        uint64_t reqId = ++reqIdCounter;

        // Prepare Buffer: [TransportHeader][UserPayload]
        size_t totalSize = sizeof(TransportHeader) + reqSize;

        std::vector<uint8_t> sendBuf;
        sendBuf.resize(totalSize);

        TransportHeader* header = (TransportHeader*)sendBuf.data();
        header->req_id = reqId;

        if (reqData && reqSize > 0) {
            memcpy(sendBuf.data() + sizeof(TransportHeader), reqData, reqSize);
        }

        RequestContext ctx;
        auto future = ctx.promise.get_future();

        // Insert into pending map
        {
            Shard& shard = shards[reqId % SHARD_COUNT];
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.requests[reqId] = &ctx;
        }

        impl->Send(sendBuf.data(), (uint32_t)totalSize, MSG_ID_NORMAL);

        // Wait
        outResponse = future.get();
        return true;
    }

    /**
     * @brief Sends a Heartbeat request and waits for acknowledgment.
     *
     * Useful for checking if the Guest is alive.
     * @return true if acknowledged within 1 second.
     */
    bool SendHeartbeat() {
        std::future<void> future;
        {
            std::lock_guard<std::mutex> lock(heartbeatMutex);
            heartbeatPromise = std::unique_ptr<std::promise<void>>(new std::promise<void>());
            future = heartbeatPromise->get_future();
        }

        // Heartbeat has no payload, just msgId
        impl->Send(nullptr, 0, MSG_ID_HEARTBEAT_REQ);

        if (future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
            return false;
        }
        return true;
    }

    /**
     * @brief Sends a Shutdown signal to the Guest.
     *
     * This is a fire-and-forget message telling the Guest to exit.
     */
    void SendShutdown() {
        impl->Send(nullptr, 0, MSG_ID_SHUTDOWN);
    }

private:
    /**
     * @brief Internal handler for incoming messages from the implementation layer.
     */
    void OnMessage(std::vector<uint8_t>&& data, uint32_t msgId) {
        if (msgId == MSG_ID_HEARTBEAT_RESP) {
            std::lock_guard<std::mutex> lock(heartbeatMutex);
            if (heartbeatPromise) {
                heartbeatPromise->set_value();
                heartbeatPromise.reset();
            }
            return;
        }

        if (msgId == MSG_ID_NORMAL) {
            if (data.size() < sizeof(TransportHeader)) return; // Malformed

            // Extract Header
            TransportHeader* header = (TransportHeader*)data.data();
            uint64_t reqId = header->req_id;

            // Strip Header for user
            std::vector<uint8_t> userPayload;
            userPayload.assign(data.begin() + sizeof(TransportHeader), data.end());

            // Find Promise
            Shard& shard = shards[reqId % SHARD_COUNT];
            std::lock_guard<std::mutex> lock(shard.mutex);
            auto it = shard.requests.find(reqId);
            if (it != shard.requests.end()) {
                it->second->promise.set_value(std::move(userPayload));
                shard.requests.erase(it);
            }
        }
    }
};

}
