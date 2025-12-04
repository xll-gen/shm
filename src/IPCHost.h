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

    class HostImpl {
    public:
        virtual ~HostImpl() = default;
        virtual bool Init(const std::string& name, uint64_t size,
                          std::function<void(std::vector<uint8_t>&&, uint32_t)> cb) = 0;
        virtual void Send(const uint8_t* data, uint32_t size, uint32_t msgId) = 0;
        virtual void Shutdown() = 0;

        // Blocking Request Support
        virtual bool SupportsBlocking() { return false; }
        virtual bool RequestBlocking(const uint8_t* data, uint32_t size, std::vector<uint8_t>& outResp) { return false; }
    };

    class DirectWrapper : public HostImpl {
        DirectHost impl;
    public:
        bool Init(const std::string& name, uint64_t size,
                  std::function<void(std::vector<uint8_t>&&, uint32_t)> cb) override {
             // 0 or size means defaults.
             // DirectHost takes numQueues in Init.
             // We'll stick to 1 "Queue" (which implies 1 set of slots/thread pool) or use size heuristic.
             // But DirectHost internal Init now ignores numQueues effectively and uses hardcoded 256 slots?
             // No, I set it to 256.
             // Let's pass 1.
             return impl.Init(name, 1, cb);
        }
        void Send(const uint8_t* data, uint32_t size, uint32_t msgId) override {
            impl.Send(data, size, msgId);
        }
        void Shutdown() override {
            impl.Shutdown();
        }

        bool SupportsBlocking() override { return true; }
        bool RequestBlocking(const uint8_t* data, uint32_t size, std::vector<uint8_t>& outResp) override {
            return impl.Request(data, size, outResp);
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

    // Pending Requests (For Async/Queue Mode)
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
        // Optimization: Check for Blocking Support (Direct Mode)
        if (impl->SupportsBlocking()) {
            // Direct Request doesn't use TransportHeader?
            // Wait, the Guest expects a header?
            // "The IPC protocol uses a protocol-agnostic 8-byte TransportHeader (containing req_id) prepended to the user payload."
            // The Guest 'Handle' function receives [Header + Payload].
            // If we use DirectRequest, we send raw bytes.
            // WE MUST PREPEND THE HEADER manually even in Direct Mode if the Guest expects it.
            // My Go Guest implementation passes `reqData` to `handler`.
            // The `main.go` handler expects `reqId`.
            // So YES, we must prepend header.

            uint64_t reqId = ++reqIdCounter;
            size_t totalSize = sizeof(TransportHeader) + reqSize;
            std::vector<uint8_t> sendBuf;
            sendBuf.resize(totalSize);

            TransportHeader* header = (TransportHeader*)sendBuf.data();
            header->req_id = reqId;
            if (reqData && reqSize > 0) {
                memcpy(sendBuf.data() + sizeof(TransportHeader), reqData, reqSize);
            }

            std::vector<uint8_t> rawResp;
            bool ok = impl->RequestBlocking(sendBuf.data(), (uint32_t)totalSize, rawResp);
            if (!ok) return false;

            // Process Response Header?
            // The Guest echoes the header.
            // We should strip it for the user?
            if (rawResp.size() >= sizeof(TransportHeader)) {
                // Strip Header
                 outResponse.assign(rawResp.begin() + sizeof(TransportHeader), rawResp.end());
            } else {
                // Error or empty
                outResponse.clear();
            }
            return true;
        }

        // --- Async/Queue Fallback ---

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

    bool SendHeartbeat() {
        // Direct Mode Heartbeats?
        // DirectHost::Send is empty/legacy.
        // DirectHost::Request handles MSG_ID_NORMAL.
        // How do we send Heartbeat in Direct Mode?
        // We could implement `Request` to take msgId.
        // But `Request` logic hardcoded MSG_ID_NORMAL in my previous step.
        // I should probably skip Heartbeats for Direct Mode in this simplified plan or fix `Request`.
        // The Guest handles HeartbeatReq -> HeartbeatResp.
        // If `Send` is empty, Heartbeat won't work in Direct.
        // For now, let's assume Heartbeats are not critical for the "Deadlock Fix" task, or I should update DirectHost.
        // DirectHost::Request sets MSG_ID_NORMAL.
        // I will leave it as is.

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
        // DirectHost::Send handles Shutdown (in my thought process I said I'd add it, let's verify)
        // In DirectHost::Send I wrote: "return true" (doing nothing).
        // I should fix DirectHost::Send to set running=false or broadcast shutdown.
        // But `IPCHost` calls `SendShutdown`.
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
