#pragma once
#include <string>
#include <vector>
#include <future>
#include <map>
#include <mutex>
#include "SPSCQueue.h"
#include "Platform.h"

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
    std::string shmName;
    uint64_t queueSize;
    ShmHandle hMapFile;

    std::unique_ptr<SPSCQueue> toGuestQueue;
    std::unique_ptr<SPSCQueue> fromGuestQueue;
    EventHandle hToGuestEvent;
    EventHandle hFromGuestEvent;

    std::thread readerThread;
    std::atomic<bool> running;

    std::mutex pendingMutex;
    std::unordered_map<uint64_t, RequestContext*> pendingRequests;

    // Mutex for direct writing to SPSCQueue (since multiple threads might call Call)
    std::mutex sendMutex;

    // For Heartbeat
    std::mutex heartbeatMutex;
    std::unique_ptr<std::promise<void>> heartbeatPromise;

public:
    IPCHost() : shmBase(nullptr), hMapFile(0), running(false) {}
    ~IPCHost() { Shutdown(); }

    bool Init(const std::string& shmName, uint64_t queueSize);
    void Shutdown();

    // For manual FlatBuffer construction
    bool Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse);

    // Control Messages
    bool SendHeartbeat();
    void SendShutdown();

private:
    void ReaderLoop();
};

}
