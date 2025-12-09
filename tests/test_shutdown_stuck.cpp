#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <vector>
#include "shm/DirectHost.h"
#include "shm/IPCUtils.h"
#include "shm/Platform.h"

using namespace shm;

void GuestWorker(std::string shmName, int slotIdx) {
    std::string reqName = shmName + "_slot_" + std::to_string(slotIdx);
    std::string respName = shmName + "_slot_" + std::to_string(slotIdx) + "_resp";

    EventHandle hReq = Platform::CreateNamedEvent(reqName.c_str());
    EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());

    ShmHandle hShm;
    bool exists;
    void* addr = Platform::CreateNamedShm(shmName.c_str(), 65536, hShm, exists);

    if (!addr) {
        std::cerr << "[Guest] Failed to map SHM" << std::endl;
        return;
    }

    uint8_t* ptr = (uint8_t*)addr + 64;
    SlotHeader* header = (SlotHeader*)ptr;

    bool running = true;
    while(running) {
        // Wait for REQ_READY
        while(header->state.load(std::memory_order_acquire) != SLOT_REQ_READY) {
            Platform::WaitEvent(hReq, 100);
            if (!running) break;
        }

        uint32_t type = header->msgType;
        if (type == MSG_TYPE_SHUTDOWN) {
            header->respSize = 0;
            header->state.store(SLOT_RESP_READY, std::memory_order_release);
            Platform::SignalEvent(hResp);
            running = false;
        } else {
            // Reply
            header->respSize = 0;
            header->state.store(SLOT_RESP_READY, std::memory_order_release);
            Platform::SignalEvent(hResp);

            // Wait for Host to consume (SLOT_FREE)
            // If Host is stuck, we spin here
            int spins = 0;
            while(header->state.load(std::memory_order_acquire) == SLOT_RESP_READY) {
                 Platform::CpuRelax();
                 spins++;
                 if (spins > 1000000) {
                      std::this_thread::sleep_for(std::chrono::milliseconds(1));
                      spins = 0;
                 }
            }
        }
    }

    Platform::CloseEvent(hReq);
    Platform::CloseEvent(hResp);
    Platform::CloseShm(hShm, addr, 65536);
}

int main() {
    std::string shmName = "ShutdownStuckTest";
    Platform::UnlinkShm(shmName.c_str());
    Platform::UnlinkNamedEvent((shmName + "_slot_0").c_str());
    Platform::UnlinkNamedEvent((shmName + "_slot_0_resp").c_str());

    DirectHost host;
    if (!host.Init(shmName, 1, 1024)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    // Start Guest
    std::thread guest(GuestWorker, shmName, 0);

    // 1. Send normal message manually to leave slot in RESP_READY
    int32_t idx = host.AcquireSpecificSlot(0);
    if (idx < 0) {
        std::cerr << "Acquire failed" << std::endl;
        guest.detach();
        return 1;
    }

    uint8_t* reqBuf = host.GetReqBuffer(idx);
    SlotHeader* header = (SlotHeader*)(reqBuf - 128); // Offset 128 backwards

    header->msgType = MSG_TYPE_NORMAL;
    header->reqSize = 0;
    header->state.store(SLOT_REQ_READY, std::memory_order_release);

    std::string reqName = shmName + "_slot_0";
    EventHandle hReq = Platform::CreateNamedEvent(reqName.c_str());
    Platform::SignalEvent(hReq);
    Platform::CloseEvent(hReq);

    // Wait for RESP_READY
    auto start = std::chrono::steady_clock::now();
    while(header->state.load(std::memory_order_acquire) != SLOT_RESP_READY) {
         if (std::chrono::steady_clock::now() - start > std::chrono::seconds(2)) {
             std::cerr << "Guest failed to reply" << std::endl;
             guest.detach();
             return 1;
         }
         std::this_thread::yield();
    }

    std::cout << "Slot is RESP_READY. Calling SendShutdown..." << std::endl;

    // 2. Call SendShutdown.
    start = std::chrono::steady_clock::now();
    host.SendShutdown();
    auto end = std::chrono::steady_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "SendShutdown took " << duration << "ms" << std::endl;

    bool bugDetected = false;
    if (duration >= 900) { // > 900ms implies timeout (since we passed 1000ms)
        std::cout << "BUG DETECTED: SendShutdown timed out!" << std::endl;
        bugDetected = true;
    } else {
        std::cout << "SendShutdown succeeded quickly." << std::endl;
    }

    if (bugDetected) {
        guest.detach();
        return 1;
    }

    if (guest.joinable()) guest.join();
    return 0;
}
