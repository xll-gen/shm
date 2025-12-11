#include <iostream>
#include <thread>
#include <cstring>
#include <atomic>
#include <vector>
#include <shm/DirectHost.h>

using namespace shm;

void run_mock_guest(std::string shmName, int numSlots) {
    ShmHandle hMap;
    bool exists;
    size_t size = 64 + (128 + 1024) * numSlots;
    void* ptr = Platform::CreateNamedShm(shmName.c_str(), size, hMap, exists);

    if (!ptr) return;

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

                 // We expect negative size
                 if (reqSize >= 0) {
                      // Unexpected for this test
                      header->respSize = 0;
                 } else {
                     int32_t absSize = -reqSize;
                     uint8_t* reqBufStart = (uint8_t*)header + sizeof(SlotHeader) + ex->reqOffset;
                     uint32_t maxReq = ex->respOffset - ex->reqOffset;

                     uint8_t* dataPtr = reqBufStart + maxReq - absSize;

                     // Check if data matches "TEST"
                     if (absSize == 4 && memcmp(dataPtr, "TEST", 4) == 0) {
                          // Success
                          char* respPtr = (char*)((uint8_t*)header + sizeof(SlotHeader) + ex->respOffset);
                          strcpy(respPtr, "PASS");
                          header->respSize = 5;
                     } else {
                          // Fail
                          char* respPtr = (char*)((uint8_t*)header + sizeof(SlotHeader) + ex->respOffset);
                          strcpy(respPtr, "FAIL");
                          header->respSize = 5;
                     }
                 }
                 header->state.store(SLOT_RESP_READY, std::memory_order_release);
                 running = false;
                 // Signal the Host
                 // We need to create the event handle.
                 std::string respName = shmName + "_slot_" + std::to_string(i) + "_resp";
                 EventHandle h = Platform::CreateNamedEvent(respName.c_str());
                 Platform::SignalEvent(h);
                 Platform::CloseEvent(h);
                 break;
             }
        }
        Platform::CpuRelax();
    }
    Platform::CloseShm(hMap, ptr, size);
}

int main() {
    DirectHost host;
    std::string name = "ReproNegativeSend";
    if (!host.Init(name, 1, 1024)) return 1;

    std::thread guest(run_mock_guest, name, 1);
    Platform::ThreadYield();

    std::vector<uint8_t> resp;
    // Send "TEST" with size -4 (End Aligned)
    auto res = host.Send((const uint8_t*)"TEST", -4, MsgType::FLATBUFFER, resp);

    if (guest.joinable()) guest.join();

    if (!res.HasError() && res.Value() > 0 && std::string((char*)resp.data()) == "PASS") {
        std::cout << "Test PASSED" << std::endl;
        return 0;
    } else {
        std::cout << "Test FAILED: " << (!res.HasError() && res.Value() > 0 ? std::string((char*)resp.data()) : "No Resp/Incorrect") << std::endl;
        return 1;
    }
}
