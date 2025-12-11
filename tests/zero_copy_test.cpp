#include <iostream>
#include <thread>
#include <cstring>
#include <atomic>
#include <vector>
#include <shm/DirectHost.h>

using namespace shm;

void run_mock_guest(std::string shmName, int numSlots) {
    // Open SHM
    ShmHandle hMap;
    bool exists;
    // Assume Host created it already. Platform::CreateNamedShm maps it.
    // We need to calculate size.
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

    // Simple loop to serve 1 request
    bool running = true;
    while(running) {
        for(int i=0; i<numSlots; ++i) {
             SlotHeader* header = (SlotHeader*)(slotBase + i*slotStride);
             uint32_t state = header->state.load(std::memory_order_acquire);

             if (state == SLOT_REQ_READY) {
                 // Check Msg ID
                 if (header->msgType == MsgType::FLATBUFFER) {
                     // Verify negative size
                     if (header->reqSize >= 0) {
                         std::cerr << "Guest: Expected negative size for FlatBuffer!" << std::endl;
                     }

                     // Write Response
                     // For zero-copy, if we want to mimic writing at the end
                     // We would write to respBuffer.
                     // Host expects response in respBuffer.

                     // Let's just write "OK" to response buffer
                     // For simplicity, we assume standard response area for now
                     // But wait, user ZeroCopySlot access response buffer directly.

                     uint8_t* respBuf = (uint8_t*)header + sizeof(SlotHeader) + ex->respOffset;
                     // Write 4 bytes "DONE"
                     memcpy(respBuf, "DONE", 4);
                     header->respSize = 4;

                     header->state.store(SLOT_RESP_READY, std::memory_order_release);
                     running = false; // Exit after one success
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
    std::string name = "ZeroCopyTest";
    if (!host.Init(name, 1, 1024)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    // Spawn Mock Guest
    std::thread guest(run_mock_guest, name, 1);

    // Test ZeroCopySlot
    {
        auto slot = host.GetZeroCopySlot();
        if (!slot.IsValid()) {
             std::cerr << "Failed to get slot" << std::endl;
             return 1;
        }

        // Write "DATA" to request
        uint8_t* req = slot.GetReqBuffer();
        memcpy(req, "DATA", 4);

        std::cout << "Sending FlatBuffer..." << std::endl;
        if (slot.SendFlatBuffer(4).HasError()) {
            std::cerr << "SendFlatBuffer failed" << std::endl;
            if (guest.joinable()) guest.join();
            return 1;
        }

        // Check Response
        uint8_t* resp = slot.GetRespBuffer();
        if (memcmp(resp, "DONE", 4) == 0) {
            std::cout << "Success: Received DONE" << std::endl;
        } else {
            std::cerr << "Failed: Response mismatch" << std::endl;
            return 1;
        }
    }
    // Slot released here

    if (guest.joinable()) guest.join();

    std::cout << "Test Passed" << std::endl;
    return 0;
}
