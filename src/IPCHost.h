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
#include <functional> // hash
#include "SPSCQueue.h"
#include "MPSCQueue.h"
#include "Platform.h"
#include "../benchmarks/include/ipc_generated.h"

namespace shm {

// Message ID Constants
const uint32_t MSG_ID_NORMAL = 0;
const uint32_t MSG_ID_HEARTBEAT_REQ = 1;
const uint32_t MSG_ID_HEARTBEAT_RESP = 2;
const uint32_t MSG_ID_SHUTDOWN = 3;

template <typename QueueT>
class IPCHost {
    struct RequestContext {
        std::promise<std::vector<uint8_t>> promise;
    };

    struct Lane {
        std::unique_ptr<QueueT> toGuestQueue;
        std::unique_ptr<QueueT> fromGuestQueue;
        EventHandle hToGuestEvent;
        EventHandle hFromGuestEvent;
        std::thread readerThread;

        // Per-lane pending requests to avoid global lock
        std::mutex pendingMutex;
        std::unordered_map<uint64_t, RequestContext*> pendingRequests;

        // Mutex for direct writing to Queue
        // Only needed if multiple threads map to the same lane and QueueT is SPSC.
        std::mutex sendMutex;
    };

    void* shmBase;
    std::string shmName;
    uint64_t queueSize;
    ShmHandle hMapFile;

    std::vector<std::unique_ptr<Lane>> lanes;
    std::atomic<bool> running;

    // For Heartbeat - we only use Lane 0
    std::mutex heartbeatMutex;
    std::unique_ptr<std::promise<void>> heartbeatPromise;

    std::atomic<uint32_t> reqIdCounter{0};

public:
    IPCHost() : shmBase(nullptr), hMapFile(0), running(false) {}
    ~IPCHost() { Shutdown(); }

    uint32_t GenerateReqId() { return ++reqIdCounter; }

    bool Init(const std::string& shmName, uint64_t queueSize, uint32_t numQueues = 1) {
        if (numQueues == 0) numQueues = 1;
        this->shmName = shmName;
        this->queueSize = queueSize;

        size_t qRequiredSize = QueueT::GetRequiredSize(queueSize);
        // Each lane has 2 queues (req + resp)
        size_t totalShmSize = numQueues * qRequiredSize * 2;

        bool exists = false;
        shmBase = Platform::CreateNamedShm(shmName.c_str(), totalShmSize, hMapFile, exists);
        if (!shmBase) return false;

        if (!exists) {
            memset(shmBase, 0, totalShmSize);
        }

        uint8_t* ptr = (uint8_t*)shmBase;
        running = true;

        for (uint32_t i = 0; i < numQueues; ++i) {
            auto lane = std::unique_ptr<Lane>(new Lane());

            // Initialize Request Queue
            uint8_t* reqBase = ptr;
            if (!exists) QueueT::Init(reqBase, queueSize);
            ptr += qRequiredSize;

            // Initialize Response Queue
            uint8_t* respBase = ptr;
            if (!exists) QueueT::Init(respBase, queueSize);
            ptr += qRequiredSize;

            std::string suffix = "_" + std::to_string(i);
            lane->hToGuestEvent = Platform::CreateNamedEvent((shmName + "_req" + suffix).c_str());
            lane->hFromGuestEvent = Platform::CreateNamedEvent((shmName + "_resp" + suffix).c_str());

            lane->toGuestQueue = std::unique_ptr<QueueT>(new QueueT(reqBase, queueSize, lane->hToGuestEvent));
            lane->fromGuestQueue = std::unique_ptr<QueueT>(new QueueT(respBase, queueSize, lane->hFromGuestEvent));

            lane->readerThread = std::thread(&IPCHost::ReaderLoop, this, i);

            lanes.push_back(std::move(lane));
        }

        return true;
    }

    void Shutdown() {
        bool expected = true;
        if (!running.compare_exchange_strong(expected, false)) return;

        // Wake up readers if stuck?
        // We rely on host.SendShutdown() called before this to signal guest.
        // Guest shutdown should trigger response or closure?
        // Actually, our Dequeue returns if 'running' flag is false, but we need to signal events maybe?
        // But Dequeue checks running in loop.

        for (auto& lane : lanes) {
            if (lane->readerThread.joinable()) lane->readerThread.join();
            Platform::CloseEvent(lane->hToGuestEvent);
            Platform::CloseEvent(lane->hFromGuestEvent);
        }
        lanes.clear();

        if (shmBase) Platform::CloseShm(hMapFile, shmBase);
        shmBase = nullptr;
    }

    // Call sends a request and waits for response.
    bool Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse) {
        uint64_t reqId = 0;
        auto msg = ipc::GetMessage(reqData);
        reqId = msg->req_id();

        // Pick Lane based on Thread ID to reduce contention
        size_t laneIdx = std::hash<std::thread::id>{}(std::this_thread::get_id()) % lanes.size();
        Lane& lane = *lanes[laneIdx];

        RequestContext ctx;
        auto future = ctx.promise.get_future();

        {
            std::lock_guard<std::mutex> lock(lane.pendingMutex);
            lane.pendingRequests[reqId] = &ctx;
        }

        // Send
        EnqueueHelper(lane, reqData, (uint32_t)reqSize, MSG_ID_NORMAL);

        future.wait();
        outResponse = future.get();

        return true;
    }

    bool SendHeartbeat() {
        // Use Lane 0
        if (lanes.empty()) return false;
        Lane& lane = *lanes[0];

        std::future<void> future;
        {
            std::lock_guard<std::mutex> lock(heartbeatMutex);
            heartbeatPromise = std::unique_ptr<std::promise<void>>(new std::promise<void>());
            future = heartbeatPromise->get_future();
        }

        EnqueueHelper(lane, nullptr, 0, MSG_ID_HEARTBEAT_REQ);

        if (future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
            return false;
        }
        return true;
    }

    void SendShutdown() {
        // Broadcast to all lanes to ensure all guest workers exit
        for (auto& lane : lanes) {
             EnqueueHelper(*lane, nullptr, 0, MSG_ID_SHUTDOWN);
        }
    }

private:
    // Helper to lock only for SPSCQueue
    void EnqueueHelper(Lane& lane, const uint8_t* data, uint32_t size, uint32_t msgId) {
        EnqueueImpl(lane, data, size, msgId, std::is_same<QueueT, SPSCQueue>());
    }

    void EnqueueImpl(Lane& lane, const uint8_t* data, uint32_t size, uint32_t msgId, std::true_type /* is_spsc */) {
        std::lock_guard<std::mutex> lock(lane.sendMutex);
        lane.toGuestQueue->Enqueue(data, size, msgId);
    }

    void EnqueueImpl(Lane& lane, const uint8_t* data, uint32_t size, uint32_t msgId, std::false_type /* is_mpsc */) {
        // No lock needed for MPSC
        lane.toGuestQueue->Enqueue(data, size, msgId);
    }

    void ReaderLoop(int laneIdx) {
        Lane& lane = *lanes[laneIdx];
        std::vector<uint8_t> buffer;
        buffer.reserve(1024);

        while (running) {
            uint32_t msgId = lane.fromGuestQueue->Dequeue(buffer, &running);

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

                std::lock_guard<std::mutex> lock(lane.pendingMutex);
                auto it = lane.pendingRequests.find(reqId);
                if (it != lane.pendingRequests.end()) {
                    RequestContext* ctx = it->second;
                    ctx->promise.set_value(std::move(buffer));
                    lane.pendingRequests.erase(it);
                }
            }
        }
    }
};

}
