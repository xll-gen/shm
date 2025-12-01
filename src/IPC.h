#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <thread>
#include "../src/Platform.h"
#include "../include/MPSCQueue.h"

namespace shm {

class IPCClient {
    struct RequestContext {
        EventHandle hCompleteEvent;
        std::vector<uint8_t> responseData;
    };

    struct SharedMemoryLayout {
        // We use 2 queues.
        // 1. ReqQueue: C++ -> Go
        // 2. RespQueue: Go -> C++
        // They are placed sequentially in SHM.
        // QueueHeader (128) + Data ...
    };

    void* shmBase;
    ShmHandle hMapFile;

    std::unique_ptr<MPSCQueue> reqQueue; // To Go
    std::unique_ptr<MPSCQueue> respQueue; // From Go

    EventHandle hReqEvent; // Signaled when we write to ReqQueue
    EventHandle hRespEvent; // Signaled when Go writes to RespQueue

    std::atomic<bool> running;
    std::thread readerThread;

    std::atomic<uint64_t> nextReqId{1};

    std::mutex pendingMutex;
    std::unordered_map<uint64_t, RequestContext*> pendingRequests;

public:
    IPCClient() : shmBase(nullptr), hMapFile(0), running(false) {}
    ~IPCClient() { Shutdown(); }

    bool Init(const std::string& shmName, uint64_t queueSize);
    void Shutdown();

    // Sends request and waits for response (Blocking)
    // For manual FlatBuffer construction
    bool Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse);

private:
    void ReaderLoop();
};

}
