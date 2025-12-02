#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <future>
#include <stack>

#include "SPSCQueue.h"
#include "Platform.h"

namespace shm {

// Standard IPC Messages
// msgId=0: Normal (handled by user callback/promise)
// msgId=1: Heartbeat Req
// msgId=2: Heartbeat Resp
// msgId=3: Shutdown

static const uint32_t MSG_ID_NORMAL = 0;
static const uint32_t MSG_ID_HEARTBEAT_REQ = 1;
static const uint32_t MSG_ID_HEARTBEAT_RESP = 2;
static const uint32_t MSG_ID_SHUTDOWN = 3;

struct RequestContext {
    std::promise<std::vector<uint8_t>> promise;
};

class IPCHost {
public:
    IPCHost() : running(false), shmBase(nullptr) {}
    ~IPCHost() { Shutdown(); }

    bool Init(const std::string& shmName, uint64_t queueSize);
    void Shutdown();

    // Call sends a request and waits for a response.
    // The response data is placed in outResponse.
    bool Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse);

    // Helper to recycle buffers to reduce allocations
    std::vector<uint8_t> GetBuffer();
    void ReturnBuffer(std::vector<uint8_t>&& buf);

    // Sends a heartbeat request and waits for a response.
    // Returns true if pong received.
    bool SendHeartbeat();

    // Sends a shutdown signal (fire and forget)
    void SendShutdown();

    // Generates a unique Request ID (simple counter)
    uint64_t GenerateReqId() {
        return ++reqIdCounter;
    }

private:
    std::string shmName;
    uint64_t queueSize;
    void* shmBase;
    ShmHandle hMapFile;

    std::unique_ptr<SPSCQueue> toGuestQueue;
    std::unique_ptr<SPSCQueue> fromGuestQueue;

    EventHandle hToGuestEvent;
    EventHandle hFromGuestEvent;

    std::atomic<bool> running;
    std::thread readerThread;

    std::mutex pendingMutex;
    std::unordered_map<uint64_t, RequestContext*> pendingRequests;

    std::mutex sendMutex;
    std::atomic<uint64_t> reqIdCounter{0};

    // Heartbeat handling
    std::mutex heartbeatMutex;
    std::unique_ptr<std::promise<void>> heartbeatPromise;

    // Buffer Pool
    std::mutex poolMutex;
    std::stack<std::vector<uint8_t>> bufferPool;

    void ReaderLoop();
};

}
