#pragma once
#include <string>
#include <vector>
#include <future>
#include <map>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <thread>
#include <cstring>
#include "SPSCQueue.h"
#include "Platform.h"
#include "IPCUtils.h"
#include "../benchmarks/include/ipc_generated.h"

namespace shm {

template <typename QueueT>
class IPCHost {
    struct RequestContext {
        std::promise<std::vector<uint8_t>> promise;
    };

    void* shmBase;
    std::string shmName;
    uint64_t queueSize;
    ShmHandle hMapFile;

    std::unique_ptr<QueueT> toGuestQueue;
    std::unique_ptr<QueueT> fromGuestQueue;
    EventHandle hToGuestEvent;
    EventHandle hFromGuestEvent;

    std::thread readerThread;
    std::atomic<bool> running;

    std::mutex pendingMutex;
    std::unordered_map<uint64_t, RequestContext*> pendingRequests;

    // Mutex for direct writing to Queue
    // SPSCQueue is NOT thread-safe for multiple producers, so we need a lock.
    std::mutex sendMutex;

    // For Heartbeat
    std::mutex heartbeatMutex;
    std::unique_ptr<std::promise<void>> heartbeatPromise;

    std::atomic<uint32_t> reqIdCounter{0};

public:
    IPCHost() : shmBase(nullptr), hMapFile(0), running(false) {}
    ~IPCHost() { Shutdown(); }

    uint32_t GenerateReqId() { return ++reqIdCounter; }

    bool Init(const std::string& shmName, uint64_t queueSize) {
        this->shmName = shmName;
        this->queueSize = queueSize;

        size_t qTotalSize = QueueT::GetRequiredSize(queueSize);
        size_t totalShmSize = qTotalSize * 2;

        bool exists = false;
        shmBase = Platform::CreateNamedShm(shmName.c_str(), totalShmSize, hMapFile, exists);
        if (!shmBase) return false;

        memset(shmBase, 0, totalShmSize);

        uint8_t* reqBase = (uint8_t*)shmBase;
        QueueT::Init(reqBase, queueSize);

        uint8_t* respBase = reqBase + qTotalSize;
        QueueT::Init(respBase, queueSize);

        hToGuestEvent = Platform::CreateNamedEvent((shmName + "_event_req").c_str());
        hFromGuestEvent = Platform::CreateNamedEvent((shmName + "_event_resp").c_str());

        toGuestQueue = std::unique_ptr<QueueT>(new QueueT(reqBase, queueSize, hToGuestEvent));
        fromGuestQueue = std::unique_ptr<QueueT>(new QueueT(respBase, queueSize, hFromGuestEvent));

        running = true;
        readerThread = std::thread(&IPCHost::ReaderLoop, this);

        return true;
    }

    void Shutdown() {
        running = false;

        if (readerThread.joinable()) readerThread.join();

        if (shmBase) Platform::CloseShm(hMapFile, shmBase);
        Platform::CloseEvent(hToGuestEvent);
        Platform::CloseEvent(hFromGuestEvent);
    }

    // Call sends a request and waits for response.
    bool Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse) {
        uint64_t reqId = 0;
        auto msg = ipc::GetMessage(reqData);
        reqId = msg->req_id();

        RequestContext ctx;
        auto future = ctx.promise.get_future();

        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            pendingRequests[reqId] = &ctx;
        }

        EnqueueHelper(reqData, (uint32_t)reqSize, MSG_ID_NORMAL);

        future.wait();
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

        EnqueueHelper(nullptr, 0, MSG_ID_HEARTBEAT_REQ);

        if (future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
            return false;
        }
        return true;
    }

    void SendShutdown() {
        EnqueueHelper(nullptr, 0, MSG_ID_SHUTDOWN);
    }

private:
    // Helper to lock only for SPSCQueue
    // Overload for SPSCQueue
    void EnqueueHelper(const uint8_t* data, uint32_t size, uint32_t msgId) {
        EnqueueImpl(data, size, msgId, std::is_same<QueueT, SPSCQueue>());
    }

    void EnqueueImpl(const uint8_t* data, uint32_t size, uint32_t msgId, std::true_type /* is_spsc */) {
        std::lock_guard<std::mutex> lock(sendMutex);
        toGuestQueue->Enqueue(data, size, msgId);
    }

    void EnqueueImpl(const uint8_t* data, uint32_t size, uint32_t msgId, std::false_type /* is_mpsc */) {
        // No lock needed for MPSC
        toGuestQueue->Enqueue(data, size, msgId);
    }

    void ReaderLoop() {
        std::vector<uint8_t> buffer;
        buffer.reserve(1024);

        while (running) {
            uint32_t msgId = fromGuestQueue->Dequeue(buffer, &running);

            if (msgId == 0xFFFFFFFF) break;

            if (msgId == MSG_ID_HEARTBEAT_RESP) {
                std::lock_guard<std::mutex> lock(heartbeatMutex);
                if (heartbeatPromise) {
                    heartbeatPromise->set_value();
                    heartbeatPromise.reset();
                }
                continue;
            }

            if (msgId == MSG_ID_NORMAL) {
                auto msg = ipc::GetMessage(buffer.data());
                if (!msg) continue;

                uint64_t reqId = msg->req_id();

                std::lock_guard<std::mutex> lock(pendingMutex);
                auto it = pendingRequests.find(reqId);
                if (it != pendingRequests.end()) {
                    RequestContext* ctx = it->second;
                    ctx->promise.set_value(std::move(buffer));
                    pendingRequests.erase(it);
                }
            }
        }
    }
};

}
