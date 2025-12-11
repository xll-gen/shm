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
    // ExchangeHeader (64)
    // Slot 0: Header(128) + Data(128) = 256
    size_t size = 64 + (256) * numSlots;

    void* ptr = Platform::CreateNamedShm(shmName.c_str(), size, hMap, exists);
    if (!ptr) return;

    ExchangeHeader* ex = (ExchangeHeader*)ptr;
    uint8_t* slotBase = (uint8_t*)ptr + sizeof(ExchangeHeader);
    size_t slotStride = sizeof(SlotHeader) + ex->slotSize;

    // We only loop once for simplicity
    bool processed = false;
    int loopCount = 0;
    while(!processed && loopCount < 100000000) { // Limit to avoid infinite loop
        for(int i=0; i<numSlots; ++i) {
             SlotHeader* header = (SlotHeader*)(slotBase + i*slotStride);
             uint32_t state = header->state.load(std::memory_order_acquire);

             if (state == SLOT_REQ_READY) {
                 header->respSize = 0;
                 header->state.store(SLOT_RESP_READY, std::memory_order_release);

                 std::string respName = shmName + "_slot_" + std::to_string(i) + "_resp";
                 EventHandle h = Platform::CreateNamedEvent(respName.c_str());
                 Platform::SignalEvent(h);
                 Platform::CloseEvent(h);
                 processed = true;
             }
        }
        Platform::CpuRelax();
        loopCount++;
    }
    Platform::CloseShm(hMap, ptr, size);
}

int main() {
    DirectHost host;
    std::string name = "TestSendTruncation";
    // DataSize = 128. MaxReqSize = 64.
    if (!host.Init(name, 1, 128)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    std::thread guest(run_mock_guest, name, 1);
    Platform::ThreadYield();

    std::vector<uint8_t> resp;
    // Send 100 bytes (Expect fail because MaxReqSize is 64)
    std::vector<uint8_t> data(100, 'A');

    auto res = host.Send(data.data(), (int32_t)data.size(), MsgType::NORMAL, resp);

    if (guest.joinable()) guest.join();

    if (res.HasError()) {
        std::cout << "Test PASSED: Send returned Error for overflow." << std::endl;
        return 0;
    } else {
        std::cout << "Test FAILED: Send returned success (" << res.Value() << ") but should have failed." << std::endl;
        return 1;
    }
}
