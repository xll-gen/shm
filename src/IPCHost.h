#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <thread>
#include <future>
#include "../src/Platform.h"
#include "../include/SPSCQueue.h"

namespace shm {

// Message ID Constants
const uint32_t MSG_ID_NORMAL = 0;
const uint32_t MSG_ID_HEARTBEAT_REQ = 1;
const uint32_t MSG_ID_HEARTBEAT_RESP = 2;
const uint32_t MSG_ID_SHUTDOWN = 3;

class IPCHost {
    struct RequestContext {
        std::promise<std::vector<uint8_t>> promise;
    };

    void* shmBase;
    ShmHandle hMapFile;

    std::unique_ptr<SPSCQueue> toGuestQueue;   // Host -> Guest
    std::unique_ptr<SPSCQueue> fromGuestQueue; // Guest -> Host

    EventHandle hToGuestEvent;   // Signaled when Host writes to toGuestQueue
    EventHandle hFromGuestEvent; // Signaled when Guest writes to fromGuestQueue

    std::atomic<bool> running;
    std::thread readerThread;

    std::atomic<uint64_t> nextReqId{1};

    std::mutex pendingMutex;
    std::unordered_map<uint64_t, RequestContext*> pendingRequests;

    // For Heartbeat
    std::mutex heartbeatMutex;
    std::unique_ptr<std::promise<void>> heartbeatPromise;

    std::mutex sendMutex; // Protects toGuestQueue (Single Producer)

public:
    IPCHost() : shmBase(nullptr), hMapFile(0), running(false) {}
    ~IPCHost() { Shutdown(); }

    bool Init(const std::string& shmName, uint64_t queueSize);
    void Shutdown();

    uint64_t GenerateReqId() { return nextReqId.fetch_add(1); }

    // Sends request and waits for response (Blocking)
    // For manual FlatBuffer construction
    bool Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse);

    // Control Messages
    bool SendHeartbeat();
    void SendShutdown();

private:
    void ReaderLoop();
};

}
