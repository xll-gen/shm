#include <iostream>
#include <thread>
#include <cstring>
#include <atomic>
#include <vector>
#include <shm/DirectHost.h>
#include <shm/Platform.h>

using namespace shm;

void run_malicious_guest(std::string shmName, int numSlots) {
    // Open SHM
    ShmHandle hMap;
    bool exists;
    // Assume Host created it already.
    // 64 + (128 + 1024)*numSlots
    size_t size = 64 + (128 + 1024) * numSlots;
    void* ptr = Platform::CreateNamedShm(shmName.c_str(), size, hMap, exists);

    if (!ptr) {
        std::cerr << "Guest: Failed to map shm" << std::endl;
        return;
    }

    ExchangeHeader* ex = (ExchangeHeader*)ptr;
    uint8_t* slotBase = (uint8_t*)ptr + sizeof(ExchangeHeader);
    size_t slotStride = sizeof(SlotHeader) + ex->slotSize;

    bool running = true;
    while(running) {
        for(int i=0; i<numSlots; ++i) {
             SlotHeader* header = (SlotHeader*)(slotBase + i*slotStride);
             uint32_t state = header->state.load(std::memory_order_acquire);

             if (state == SLOT_REQ_READY) {
                 if (header->msgType == MsgType::FLATBUFFER) {
                     // MALICIOUS ACT: Set respSize to cause underflow
                     header->respSize = -10000;

                     header->state.store(SLOT_RESP_READY, std::memory_order_release);

                     // Signal Host
                     if (header->hostState.load(std::memory_order_acquire) == HOST_STATE_WAITING) {
                         std::string respName = shmName + "_slot_" + std::to_string(i) + "_resp";
                         EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());
                         Platform::SignalEvent(hResp);
                         Platform::CloseEvent(hResp);
                     }

                     running = false;
                     break;
                 } else if (header->msgType == MsgType::SHUTDOWN) {
                     running = false;
                     break;
                 }
             }
        }
        Platform::CpuRelax();
    }
    Platform::CloseShm(hMap, ptr, size);
}

int main() {
    DirectHost host;
    std::string name = "BugRepro";
    if (!host.Init(name, 1, 1024)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    std::thread guest(run_malicious_guest, name, 1);

    int ret = 0;
    {
        auto slot = host.GetZeroCopySlot();
        if (!slot.IsValid()) {
             std::cerr << "Failed to get slot" << std::endl;
             return 1;
        }

        std::cout << "Host: Sending..." << std::endl;
        slot.SendFlatBuffer(10);

        std::cout << "Host: Getting Resp Buffer..." << std::endl;
        uint8_t* resp = slot.GetRespBuffer();
        int32_t size = slot.GetRespSize();

        uint8_t* expectedStart = host.GetReqBuffer(0);
        int64_t diff = resp - expectedStart;

        std::cout << "Resp ptr: " << (void*)resp << std::endl;
        std::cout << "Diff from reqBuffer: " << diff << std::endl;
        std::cout << "Resp Size: " << size << std::endl;

        bool ptrFail = (diff > 100000 || diff < -100000);
        bool sizeFail = (size > 512); // Max is 512

        if (ptrFail) {
            std::cout << "BUG DETECTED: Pointer is wildly out of bounds!" << std::endl;
            ret = 1;
        }
        if (sizeFail) {
            std::cout << "BUG DETECTED: Size is larger than buffer!" << std::endl;
            ret = 1;
        }

        if (!ptrFail && !sizeFail) {
            std::cout << "Test Passed: Pointer and Size are safe." << std::endl;
        }
    }

    if (guest.joinable()) guest.join();
    return ret;
}
