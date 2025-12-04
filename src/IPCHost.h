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
    IPCHost() {}
    ~IPCHost() { Shutdown(); }

    bool Init(const std::string& name, uint32_t numQueues) {
        auto handler = [this](std::vector<uint8_t>&& data, uint32_t msgId) {
            this->OnMessage(std::move(data), msgId);
        };
        return impl.Init(name, numQueues, handler);
    }

    void Shutdown() {
        SendShutdown(); // Best effort
        impl.Shutdown();
    }

    // Unified Call: Appends Header, Sends, Waits.
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
        auto future = ctx.promise.get_future();

        // Insert into pending map
        {
            Shard& shard = shards[reqId % SHARD_COUNT];
            std::lock_guard<std::mutex> lock(shard.mutex);
            shard.requests[reqId] = &ctx;
        }

        if (!impl.Send(sendBuf.data(), (uint32_t)totalSize, MSG_ID_NORMAL)) {
             // Failed to send
             Shard& shard = shards[reqId % SHARD_COUNT];
             std::lock_guard<std::mutex> lock(shard.mutex);
             shard.requests.erase(reqId);
             return false;
        }

        // Wait
        outResponse = future.get();
        return true;
    }

    bool SendHeartbeat() {
        std::future<void> future;
        {
            std::lock_guard<std::mutex> lock(heartbeatMutex);
            heartbeatPromise = std::unique_ptr<std::promise<void>>(new std::promise<void>());
            future = heartbeatPromise->get_future();
        }

        // Heartbeat has no payload, just msgId
        impl.Send(nullptr, 0, MSG_ID_HEARTBEAT_REQ);

        if (future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
            return false;
        }
        return true;
    }

    void SendShutdown() {
        impl.Send(nullptr, 0, MSG_ID_SHUTDOWN);
    }

private:
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
