#include <iostream>
#include <thread>
#include <vector>
#include <shm/DirectHost.h>
#include <chrono>

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
    auto start = std::chrono::steady_clock::now();

    while(running) {
        // Timeout to prevent hanging
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() > 2) break;

        for(int i=0; i<numSlots; ++i) {
             SlotHeader* header = (SlotHeader*)(slotBase + i*slotStride);
             uint32_t state = header->state.load(std::memory_order_acquire);

             if (state == SLOT_REQ_READY) {
                 // Reply immediately
                 header->respSize = 0;
                 header->state.store(SLOT_RESP_READY, std::memory_order_release);

                 std::string respName = shmName + "_slot_" + std::to_string(i) + "_resp";
                 EventHandle h = Platform::CreateNamedEvent(respName.c_str());
                 Platform::SignalEvent(h);
                 Platform::CloseEvent(h);
                 running = false;
             }
        }
        Platform::CpuRelax();
    }
    Platform::CloseShm(hMap, ptr, size);
}

int main() {
    DirectHost host;
    std::string name = "TestSendNull";
    if (!host.Init(name, 1, 1024)) return 1;

    std::thread guest(run_mock_guest, name, 1);
    Platform::ThreadYield();

    std::vector<uint8_t> resp;

    // This should fail (return -1)
    // We send a positive size but nullptr data.
    int ret = host.Send(nullptr, 10, MsgType::NORMAL, resp, 1000);

    if (guest.joinable()) guest.join();

    if (ret != -1) {
        std::cout << "Test FAILED: Send(nullptr, 10) returned " << ret << " (expected -1)" << std::endl;
        return 1;
    }

    std::cout << "Test PASSED" << std::endl;
    return 0;
}
