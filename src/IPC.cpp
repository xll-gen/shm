#include "IPC.h"
#include <iostream>
#ifdef _WIN32
#include <emmintrin.h> // _mm_pause
#else
#include <x86intrin.h> // __builtin_ia32_pause
#endif
#include "../benchmarks/include/ipc_generated.h" // For ipc::GetMessage

namespace shm {

bool IPCClient::Init(const std::string& shmName, uint64_t queueSize) {
    // 1. Create/Open Events
    // Suffixes are added by Platform if needed, but here we add _REQ/_RESP to base name
    std::string reqEvName = shmName + "_REQ";
    std::string respEvName = shmName + "_RESP";

    hReqEvent = Platform::CreateNamedEvent(reqEvName.c_str());
    hRespEvent = Platform::CreateNamedEvent(respEvName.c_str());

    if (!hReqEvent || !hRespEvent) return false;

    // 2. Create SHM
    // Total size = 2 * (Header + Data)
    uint64_t qTotalSize = MPSCQueue::GetRequiredSize(queueSize);
    uint64_t totalShmSize = qTotalSize * 2;

    bool exists = false;
    shmBase = Platform::CreateNamedShm(shmName.c_str(), totalShmSize, hMapFile, exists);

    if (!shmBase) return false;

    // Layout: [ReqQueue] [RespQueue]
    uint8_t* reqBase = (uint8_t*)shmBase;

    // Init if created new OR if existing but uninitialized (capacity 0)
    // This handles the case where Go created the file but C++ initializes the headers.
    QueueHeader* hdr = reinterpret_cast<QueueHeader*>(reqBase);
    if (!exists || hdr->capacity == 0) {
        MPSCQueue::Init(reqBase, queueSize);
        MPSCQueue::Init(reqBase + qTotalSize, queueSize);
    }

    reqQueue = std::make_unique<MPSCQueue>(reqBase, queueSize, hReqEvent);
    respQueue = std::make_unique<MPSCQueue>(reqBase + qTotalSize, queueSize, hRespEvent);

    // 3. Start Reader Thread
    running = true;
    readerThread = std::thread(&IPCClient::ReaderLoop, this);

    return true;
}

void IPCClient::Shutdown() {
    running = false;
    if (readerThread.joinable()) readerThread.join();

    if (shmBase) Platform::CloseShm(hMapFile, shmBase);
    if (hReqEvent) Platform::CloseEvent(hReqEvent);
    if (hRespEvent) Platform::CloseEvent(hRespEvent);
}

void IPCClient::ReaderLoop() {
    std::vector<uint8_t> buffer;
    buffer.reserve(4096);

    while (running) {
        // 1. Try Dequeue
        if (respQueue->Dequeue(buffer)) {
            // Got message
            auto msg = ipc::GetMessage(buffer.data());
            // Need to verify message validity before accessing req_id, technically
            if (!msg) continue;

            uint64_t reqId = msg->req_id();

            std::lock_guard<std::mutex> lock(pendingMutex);
            auto it = pendingRequests.find(reqId);
            if (it != pendingRequests.end()) {
                RequestContext* ctx = it->second;
                ctx->responseData = buffer; // Copy data
                Platform::SignalEvent(ctx->hCompleteEvent);
                pendingRequests.erase(it);
            }
            continue;
        }

        // 2. Hybrid Wait
        // Short Spin
        for (int i = 0; i < 4000; ++i) {
            if (respQueue->header->readPos.load(std::memory_order_relaxed) !=
                respQueue->header->writePos.load(std::memory_order_acquire)) { // Acquire matches MPSCQueue.h logic
                goto NEXT_LOOP; // Data arrived
            }
#ifdef _WIN32
            _mm_pause();
#else
            __builtin_ia32_pause();
#endif
        }

        // Wait Kernel
        Platform::WaitEvent(hRespEvent, 100); // 100ms timeout to check 'running'

    NEXT_LOOP:;
    }
}

bool IPCClient::Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse) {
    // 1. Prepare Context
    RequestContext ctx;
    // Anonymous event for this thread/request
    // On Linux, sem_open requires a name. We might need unnamed semaphores (sem_init) but those are for threads in same process (unless in shm).
    // For simplicity, we use a unique name.
    static std::atomic<uint64_t> uniq{0};
    std::string evName = "ReqEv_" + std::to_string(Platform::GetPid()) + "_" + std::to_string(uniq.fetch_add(1));
    ctx.hCompleteEvent = Platform::CreateNamedEvent(evName.c_str());

    // Parse to find ReqID - we trust the caller has set it in the FlatBuffer
    auto msg = ipc::GetMessage(reqData);
    uint64_t reqId = msg->req_id();

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingRequests[reqId] = &ctx;
    }

    // 2. Enqueue
    if (!reqQueue->Enqueue(reqData, (uint32_t)reqSize)) {
        // Queue Full
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            pendingRequests.erase(reqId);
        }
        Platform::CloseEvent(ctx.hCompleteEvent);
        return false;
    }

    // 3. Wait
    Platform::WaitEvent(ctx.hCompleteEvent);

    // 4. Result
    outResponse = std::move(ctx.responseData);
    Platform::CloseEvent(ctx.hCompleteEvent);
    return true;
}

}
