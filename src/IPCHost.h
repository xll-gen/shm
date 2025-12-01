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
    std::thread writerThread; // Dedicated writer thread

    std::atomic<uint64_t> nextReqId{1};

    std::mutex pendingMutex;
    std::unordered_map<uint64_t, RequestContext*> pendingRequests;

    // Outbound Queue for Writer Thread
    std::mutex outboundMutex;
    std::condition_variable outboundCV;
    std::vector<std::vector<uint8_t>> outboundQueue;

public:
    IPCHost() : shmBase(nullptr), hMapFile(0), running(false) {}
    ~IPCHost() { Shutdown(); }

    bool Init(const std::string& shmName, uint64_t queueSize);
    void Shutdown();

    uint64_t GenerateReqId() { return nextReqId.fetch_add(1); }

    // Sends request and waits for response (Blocking)
    // For manual FlatBuffer construction
    bool Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse);

private:
    void ReaderLoop();
    void WriterLoop();
};

}
