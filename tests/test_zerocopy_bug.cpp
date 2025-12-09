#include <iostream>
#include <thread>
#include <cstring>
#include <atomic>
#include <vector>
#include <shm/DirectHost.h>

using namespace shm;

// Mock Guest that reads data based on reqSize (Start or End aligned)
// and verifies it matches "DATA".
void run_mock_guest_integrity(std::string shmName, int numSlots) {
    ShmHandle hMap;
    bool exists;
    // Calculate size: 64 + (128 + 1024) * numSlots
    // Note: Assuming default 1024 dataSize.
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
                 int32_t reqSize = header->reqSize;
                 uint8_t* dataPtr = nullptr;
                 int32_t absSize = reqSize < 0 ? -reqSize : reqSize;

                 uint8_t* reqBufStart = (uint8_t*)header + sizeof(SlotHeader) + ex->reqOffset;
                 uint32_t maxReq = ex->respOffset - ex->reqOffset;

                 if (reqSize >= 0) {
                     // Start aligned
                     dataPtr = reqBufStart;
                 } else {
                     // End aligned
                     dataPtr = reqBufStart + maxReq - absSize;
                 }

                 // Verify Data
                 if (absSize == 4 && memcmp(dataPtr, "DATA", 4) == 0) {
                     // Success
                     // Write "OK"
                     uint8_t* respBuf = (uint8_t*)header + sizeof(SlotHeader) + ex->respOffset;
                     memcpy(respBuf, "OK", 2);
                     header->respSize = 2;
                 } else {
                     // Failure (Data Mismatch or Size Mismatch)
                     // Write "FAIL"
                     uint8_t* respBuf = (uint8_t*)header + sizeof(SlotHeader) + ex->respOffset;
                     memcpy(respBuf, "FAIL", 4);
                     header->respSize = 4;
                 }

                 header->state.store(SLOT_RESP_READY, std::memory_order_release);

                 // If shutdown? No, we just loop until main thread kills us or we assume 1 request
                 // We will exit after 1 request for this test.
                 running = false;
                 break;
             }
        }
        Platform::CpuRelax();
    }
    Platform::CloseShm(hMap, ptr, size);
}

int main() {
    DirectHost host;
    std::string name = "ZeroCopyIntegrityTest";
    // 1024 byte slots
    if (!host.Init(name, 1, 1024)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    std::thread guest(run_mock_guest_integrity, name, 1);

    // Test
    {
        auto slot = host.GetZeroCopySlot();
        if (!slot.IsValid()) {
             std::cerr << "Failed to get slot" << std::endl;
             return 1;
        }

        // Write "DATA" to request (Start Aligned)
        uint8_t* req = slot.GetReqBuffer();
        memcpy(req, "DATA", 4);

        // FIXED: Use Send with positive size (Start Aligned).
        // reqSize = 4. Guest looks at Start. Data is at Start. Guest finds "DATA".
        // Guest writes "OK".

        slot.Send(4, MsgType::NORMAL);

        // Check Response
        uint8_t* resp = slot.GetRespBuffer();
        int32_t respSize = slot.GetRespSize();

        if (respSize == 2 && memcmp(resp, "OK", 2) == 0) {
            std::cout << "Success: Received OK" << std::endl;
        } else {
            std::cout << "Failed: Received ";
            std::cout.write((char*)resp, respSize);
            std::cout << std::endl;
            if (guest.joinable()) guest.join();
            return 1; // Return 1 to signal failure
        }
    }

    if (guest.joinable()) guest.join();
    return 0;
}
