#include "IPCHost.h"
#include <iostream>
#ifdef _WIN32
#include <emmintrin.h> // _mm_pause
#else
#include <x86intrin.h> // __builtin_ia32_pause
#endif
#include "../benchmarks/include/ipc_generated.h" // For ipc::GetMessage

namespace shm {

bool IPCHost::Init(const std::string& shmName, uint64_t queueSize) {
    // 1. Create/Open Events
    // Suffixes are added by Platform if needed, but here we add _REQ/_RESP to base name
    // _REQ -> Host -> Guest (ToGuest)
    // _RESP -> Guest -> Host (FromGuest)
    std::string toGuestEvName = shmName + "_REQ";
    std::string fromGuestEvName = shmName + "_RESP";

    hToGuestEvent = Platform::CreateNamedEvent(toGuestEvName.c_str());
    hFromGuestEvent = Platform::CreateNamedEvent(fromGuestEvName.c_str());

    if (!hToGuestEvent || !hFromGuestEvent) return false;

    // 2. Create SHM
    // Total size = 2 * (Header + Data)
    uint64_t qTotalSize = MPSCQueue::GetRequiredSize(queueSize);
    uint64_t totalShmSize = qTotalSize * 2;

    bool exists = false;
    shmBase = Platform::CreateNamedShm(shmName.c_str(), totalShmSize, hMapFile, exists);

    if (!shmBase) return false;

    // Layout: [ToGuestQueue] [FromGuestQueue]
    uint8_t* reqBase = (uint8_t*)shmBase;

    // Init if created new OR if existing but uninitialized (capacity 0)
    // This handles the case where Go created the file but C++ initializes the headers.
    QueueHeader* hdr = reinterpret_cast<QueueHeader*>(reqBase);
    if (!exists || hdr->capacity == 0) {
        MPSCQueue::Init(reqBase, queueSize);
        MPSCQueue::Init(reqBase + qTotalSize, queueSize);
    }

    toGuestQueue = std::make_unique<MPSCQueue>(reqBase, queueSize, hToGuestEvent);
    fromGuestQueue = std::make_unique<MPSCQueue>(reqBase + qTotalSize, queueSize, hFromGuestEvent);

    // 3. Start Reader Thread
    running = true;
    readerThread = std::thread(&IPCHost::ReaderLoop, this);

    return true;
}

void IPCHost::Shutdown() {
    running = false;
    if (readerThread.joinable()) readerThread.join();

    if (shmBase) Platform::CloseShm(hMapFile, shmBase);
    if (hToGuestEvent) Platform::CloseEvent(hToGuestEvent);
    if (hFromGuestEvent) Platform::CloseEvent(hFromGuestEvent);
}

void IPCHost::ReaderLoop() {
    std::vector<uint8_t> buffer;
    buffer.reserve(4096);

    while (running) {
        // 1. Try Dequeue
        if (fromGuestQueue->Dequeue(buffer)) {
            // Got message
            auto msg = ipc::GetMessage(buffer.data());
            // Need to verify message validity before accessing req_id, technically
            if (!msg) continue;

            uint64_t reqId = msg->req_id();

            std::lock_guard<std::mutex> lock(pendingMutex);
            auto it = pendingRequests.find(reqId);
            if (it != pendingRequests.end()) {
                RequestContext* ctx = it->second;
                ctx->promise.set_value(std::move(buffer));
                pendingRequests.erase(it);
            }
            continue;
        }

        // 2. Hybrid Wait
        // Short Spin
        for (int i = 0; i < 4000; ++i) {
            if (fromGuestQueue->header->readPos.load(std::memory_order_relaxed) !=
                fromGuestQueue->header->writePos.load(std::memory_order_acquire)) { // Acquire matches MPSCQueue.h logic
                goto NEXT_LOOP; // Data arrived
            }
#ifdef _WIN32
            _mm_pause();
#else
            __builtin_ia32_pause();
#endif
        }

        // Wait Kernel
        Platform::WaitEvent(hFromGuestEvent, 100); // 100ms timeout to check 'running'

    NEXT_LOOP:;
    }
}

bool IPCHost::Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse) {
    // 1. Prepare Context
    RequestContext ctx;
    auto future = ctx.promise.get_future();

    // Parse to find ReqID - we trust the caller has set it in the FlatBuffer
    auto msg = ipc::GetMessage(reqData);
    uint64_t reqId = msg->req_id();

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingRequests[reqId] = &ctx;
    }

    // 2. Enqueue
    // Retry loop for Enqueue
    int retries = 0;
    while (!toGuestQueue->Enqueue(reqData, (uint32_t)reqSize)) {
        if (retries++ > 10000) { // Approx 1-10ms depending on pause
            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                pendingRequests.erase(reqId);
            }
            return false;
        }
        std::this_thread::yield();
    }

    // 3. Wait
    // In a real system, we should have a timeout
    outResponse = future.get();
    return true;
}

}
