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

enum class IPCMode {
    Queue,
    Direct
};

class IPCHost {
    struct RequestContext {
        std::promise<std::vector<uint8_t>> promise;
    };

    // Pimpl-like erasure using virtual interface is cleaner,
    // but to avoid virtual overhead in hot path (Send), we might want templates.
    // However, user asked for runtime "Init(mode)".
    // So we need a virtual base or std::function wrappers.
    // Given the 100ns latency goal, virtual function overhead (few ns) is negligible compared to IPC.

    class HostImpl {
    public:
        virtual ~HostImpl() = default;
        virtual bool Init(const std::string& name, uint64_t size,
                          std::function<void(std::vector<uint8_t>&&, uint32_t)> cb) = 0;
        virtual void Send(const uint8_t* data, uint32_t size, uint32_t msgId) = 0;
        virtual void Shutdown() = 0;
    };

    template <typename T>
    class ImplWrapper : public HostImpl {
        T impl;
    public:
        bool Init(const std::string& name, uint64_t size,
                  std::function<void(std::vector<uint8_t>&&, uint32_t)> cb) override {
            // DirectHost takes 'numQueues' not 'queueSize' in generic sense, but we map it.
            // QueueHost takes 'queueSize'.
            // Simple mapping: DirectHost uses size as 'numQueues' or we need better args.
            // Let's assume size arg is 'queueSize' for Queue, and we default numQueues for Direct?
            // User 'Init' API should probably be flexible.
            // For now, pass size.
            // DirectHost::Init signature: (name, numQueues, handler)
            // QueueHost::Init signature: (name, queueSize, handler)
            // We need specialization.
            return InitImpl(name, size, cb);
        }

        // Specialization via helper
        bool InitImpl(const std::string& name, uint64_t size,
                     std::function<void(std::vector<uint8_t>&&, uint32_t)> cb) {
            return impl.Init(name, size, cb);
        }

        void Send(const uint8_t* data, uint32_t size, uint32_t msgId) override {
            impl.Send(data, size, msgId);
        }
        void Shutdown() override {
            impl.Shutdown();
        }
    };

    // Specialization for DirectHost to map size -> numQueues
    // Wait, we can't specialize inner class method easily.
    // Let's just use a concrete subclass for DirectHostWrapper.

    class DirectWrapper : public HostImpl {
        DirectHost impl;
    public:
        bool Init(const std::string& name, uint64_t size,
                  std::function<void(std::vector<uint8_t>&&, uint32_t)> cb) override {
             // Heuristic: size < 100 -> likely numQueues (e.g. 4, 8 threads).
             // size > 1MB -> likely QueueSize.
             // This is dirty. Better to add 'config' struct.
             // For now, let's assume 'size' param in Init is generic 'capacity'.
             // If Queue, it's bytes. If Direct, it's slots?
             // User Request: "Init(name, mode)".
             // Defaulting Direct to 4 queues if not specified?
             // Let's just hardcode 4 queues for Direct if using generic Init,
             // or allow 'Init(name, mode, param)'.
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

    class QueueWrapper : public HostImpl {
        QueueHost<SPSCQueue> impl;
    public:
        bool Init(const std::string& name, uint64_t size,
                  std::function<void(std::vector<uint8_t>&&, uint32_t)> cb) override {
             if (size < 1024) size = 32 * 1024 * 1024; // Default 32MB
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

    void Shutdown() {
        if (impl) {
            SendShutdown(); // Best effort
            impl->Shutdown();
            impl.reset();
        }
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

        impl->Send(sendBuf.data(), (uint32_t)totalSize, MSG_ID_NORMAL);

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
        impl->Send(nullptr, 0, MSG_ID_HEARTBEAT_REQ);

        if (future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
            return false;
        }
        return true;
    }

    void SendShutdown() {
        impl->Send(nullptr, 0, MSG_ID_SHUTDOWN);
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
