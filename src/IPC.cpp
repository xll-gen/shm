#include "IPC.h"
#include <iostream>
#include <emmintrin.h> // _mm_pause
#include <cstring> // memcpy

namespace shm {

bool IPCClient::Init(const std::string& shmName, uint64_t queueSize) {
    // 1. Create/Open Events
    std::string reqEvName = "Local\\" + shmName + "_REQ";
    std::string respEvName = "Local\\" + shmName + "_RESP";

    hReqEvent = CreateEventA(NULL, FALSE, FALSE, reqEvName.c_str());
    hRespEvent = CreateEventA(NULL, FALSE, FALSE, respEvName.c_str());

    if (!hReqEvent || !hRespEvent) return false;

    // 2. Create SHM
    // Total size = 2 * (Header + Data)
    uint64_t qTotalSize = MPSCQueue::GetRequiredSize(queueSize);
    uint64_t totalShmSize = qTotalSize * 2;

    hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
        (DWORD)(totalShmSize >> 32), (DWORD)totalShmSize, ("Local\\" + shmName).c_str());

    if (!hMapFile) return false;

    bool isFirst = (GetLastError() != ERROR_ALREADY_EXISTS);
    shmBase = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, 0);

    if (!shmBase) return false;

    // Layout: [ReqQueue] [RespQueue]
    uint8_t* reqBase = (uint8_t*)shmBase;
    uint8_t* respBase = reqBase + qTotalSize;

    if (isFirst) {
        MPSCQueue::Init(reqBase, queueSize);
        MPSCQueue::Init(respBase, queueSize);
    }

    reqQueue = std::make_unique<MPSCQueue>(reqBase, queueSize, hReqEvent);
    respQueue = std::make_unique<MPSCQueue>(respBase, queueSize, hRespEvent);

    // 3. Start Reader Thread
    running = true;
    readerThread = std::thread(&IPCClient::ReaderLoop, this);

    return true;
}

void IPCClient::Shutdown() {
    running = false;
    if (readerThread.joinable()) readerThread.join();

    if (shmBase) UnmapViewOfFile(shmBase);
    if (hMapFile) CloseHandle(hMapFile);
    if (hReqEvent) CloseHandle(hReqEvent);
    if (hRespEvent) CloseHandle(hRespEvent);
}

void IPCClient::ReaderLoop() {
    std::vector<uint8_t> buffer;
    buffer.reserve(4096);

    while (running) {
        // 1. Try Dequeue
        if (respQueue->Dequeue(buffer)) {
            // Expected format: [ReqID (8 bytes)] [Payload ...]
            if (buffer.size() < 8) {
                // Invalid message, ignore
                continue;
            }

            uint64_t reqId = 0;
            std::memcpy(&reqId, buffer.data(), 8);

            std::lock_guard<std::mutex> lock(pendingMutex);
            auto it = pendingRequests.find(reqId);
            if (it != pendingRequests.end()) {
                RequestContext* ctx = it->second;
                // Copy data excluding ReqID
                ctx->responseData.assign(buffer.begin() + 8, buffer.end());
                SetEvent(ctx->hCompleteEvent);
                pendingRequests.erase(it);
            }
            continue;
        }

        // 2. Hybrid Wait
        // Short Spin
        for (int i = 0; i < 4000; ++i) {
            if (respQueue->header->readPos.load(std::memory_order_relaxed) !=
                respQueue->header->writePos.load(std::memory_order_acquire)) {
                goto NEXT_LOOP; // Data arrived
            }
            _mm_pause();
        }

        // Wait Kernel
        WaitForSingleObject(hRespEvent, 100); // 100ms timeout to check 'running'

    NEXT_LOOP:;
    }
}

bool IPCClient::Call(const uint8_t* reqData, size_t reqSize, std::vector<uint8_t>& outResponse) {
    // 1. Generate ReqID
    uint64_t reqId = nextReqId.fetch_add(1, std::memory_order_relaxed);

    // 2. Prepare Context
    RequestContext ctx;
    ctx.hCompleteEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingRequests[reqId] = &ctx;
    }

    // 3. Prepare Packet: [ReqID (8)] [Payload (reqSize)]
    std::vector<uint8_t> packet(8 + reqSize);
    std::memcpy(packet.data(), &reqId, 8);
    if (reqSize > 0) {
        std::memcpy(packet.data() + 8, reqData, reqSize);
    }

    // 4. Enqueue
    if (!reqQueue->Enqueue(packet.data(), (uint32_t)packet.size())) {
        // Queue Full
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            pendingRequests.erase(reqId);
        }
        CloseHandle(ctx.hCompleteEvent);
        return false;
    }

    // 5. Wait
    WaitForSingleObject(ctx.hCompleteEvent, INFINITE);

    // 6. Result
    outResponse = std::move(ctx.responseData);
    CloseHandle(ctx.hCompleteEvent);
    return true;
}

}
