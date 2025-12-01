#include "IPCHost.h"
#include <iostream>
#include "../benchmarks/include/ipc_generated.h" // For FlatBuffers decoding

namespace shm {

bool IPCHost::Init(const std::string& shmName, uint64_t queueSize) {
    this->shmName = shmName;
    this->queueSize = queueSize;

    // 1. Create SHM
    // Layout: [QueueHeader ToGuest][Data...][QueueHeader FromGuest][Data...]
    size_t qTotalSize = SPSCQueue::GetRequiredSize(queueSize);
    size_t totalShmSize = qTotalSize * 2;

    bool exists = false;
    shmBase = Platform::CreateNamedShm(shmName.c_str(), totalShmSize, hMapFile, exists);
    if (!shmBase) return false;

    // Zero out if new? Or always zero out?
    // If it already existed, we might be attaching to a dead session or a live one.
    // For Host, we typically own it.
    memset(shmBase, 0, totalShmSize);

    // 2. Init Queues
    uint8_t* reqBase = (uint8_t*)shmBase;
    SPSCQueue::Init(reqBase, queueSize);

    uint8_t* respBase = reqBase + qTotalSize;
    SPSCQueue::Init(respBase, queueSize);

    // Create Events
    hToGuestEvent = Platform::CreateNamedEvent((shmName + "_event_req").c_str());
    hFromGuestEvent = Platform::CreateNamedEvent((shmName + "_event_resp").c_str());

    toGuestQueue = std::make_unique<SPSCQueue>(reqBase, queueSize, hToGuestEvent);
    fromGuestQueue = std::make_unique<SPSCQueue>(respBase, queueSize, hFromGuestEvent);

    // 3. Start Reader Thread
    running = true;
    readerThread = std::thread(&IPCHost::ReaderLoop, this);

    return true;
}

void IPCHost::Shutdown() {
    running = false;

    if (readerThread.joinable()) readerThread.join();

    if (shmBase) Platform::CloseShm(hMapFile, shmBase);
    Platform::CloseEvent(hToGuestEvent);
    Platform::CloseEvent(hFromGuestEvent);
}

void IPCHost::ReaderLoop() {
    std::vector<uint8_t> buffer;
    buffer.reserve(1024);

    while (running) {
        // Dequeue blocks until data is available
        uint32_t msgId = fromGuestQueue->Dequeue(buffer);

        if (msgId == MSG_ID_HEARTBEAT_RESP) {
            std::lock_guard<std::mutex> lock(heartbeatMutex);
            if (heartbeatPromise) {
                heartbeatPromise->set_value();
                heartbeatPromise.reset();
            }
            continue;
        }

        if (msgId == MSG_ID_NORMAL) {
            // Got message
            auto msg = ipc::GetMessage(buffer.data());
            // Verify verifier?
            // flatbuffers::Verifier verifier(buffer.data(), buffer.size());
            // if (!msg->Verify(verifier)) continue;

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

bool IPCHost::Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse) {
    // 1. Create Promise
    uint64_t reqId = 0;
    // We need to parse reqId from FlatBuffer to register it.
    // Assuming the caller constructed a valid FlatBuffer Message.
    auto msg = ipc::GetMessage(reqData);
    reqId = msg->req_id();

    RequestContext ctx;
    auto future = ctx.promise.get_future();

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingRequests[reqId] = &ctx;
    }

    // 2. Enqueue
    {
        std::lock_guard<std::mutex> lock(sendMutex);
        toGuestQueue->Enqueue(reqData, (uint32_t)reqSize, MSG_ID_NORMAL);
    }

    // 3. Wait
    // TODO: Timeout
    future.wait();
    outResponse = future.get();

    return true;
}

bool IPCHost::SendHeartbeat() {
    std::future<void> future;
    {
        std::lock_guard<std::mutex> lock(heartbeatMutex);
        heartbeatPromise = std::make_unique<std::promise<void>>();
        future = heartbeatPromise->get_future();
    }

    {
        std::lock_guard<std::mutex> lock(sendMutex);
        toGuestQueue->Enqueue(nullptr, 0, MSG_ID_HEARTBEAT_REQ);
    }

    // Wait for response with timeout
    if (future.wait_for(std::chrono::seconds(1)) == std::future_status::timeout) {
        return false;
    }
    return true;
}

void IPCHost::SendShutdown() {
    std::lock_guard<std::mutex> lock(sendMutex);
    toGuestQueue->Enqueue(nullptr, 0, MSG_ID_SHUTDOWN);
}

}
