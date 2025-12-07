#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>
#include <chrono>
#include <shm/DirectHost.h>
#include <shm/Platform.h>

using namespace shm;

int main() {
    DirectHost host;
    std::string name = "MsgIdTest";
    if (!host.Init(name, 1, 1024)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    // Manually map SHM to act as Guest
    ShmHandle hMap;
    bool exists;
    size_t size = 64 + (128 + 1024) * 1;
    void* ptr = Platform::CreateNamedShm(name.c_str(), size, hMap, exists);

    if (!ptr) {
        std::cerr << "Failed to map SHM" << std::endl;
        return 1;
    }

    ExchangeHeader* ex = (ExchangeHeader*)ptr;
    uint8_t* slotBase = (uint8_t*)ptr + sizeof(ExchangeHeader);
    SlotHeader* header = (SlotHeader*)slotBase; // Slot 0

    // Set sentinel MsgId
    header->msgId = 999;

    std::atomic<bool> threadRunning{true};

    std::thread hostThread([&]() {
        std::vector<uint8_t> resp;
        // Send Msg 1
        host.Send(nullptr, 0, MSG_TYPE_NORMAL, resp);
    });

    // Wait for REQ_READY
    int timeout = 0;
    while (header->state.load(std::memory_order_acquire) != SLOT_REQ_READY) {
        Platform::CpuRelax();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        timeout++;
        if (timeout > 2000) {
            std::cerr << "Timeout waiting for REQ_READY" << std::endl;
            exit(1);
        }
    }

    uint32_t msgId = header->msgId;
    std::cout << "MsgId: " << msgId << std::endl;

    bool passed = false;
    if (msgId == 999) {
        std::cout << "FAIL: Host did not update msgId!" << std::endl;
        passed = false;
    } else {
        std::cout << "PASS: Host updated msgId to " << msgId << std::endl;
        passed = true;
    }

    // Cleanup
    header->state.store(SLOT_RESP_READY, std::memory_order_release);
    std::string respName = name + "_slot_0_resp";
    EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());
    Platform::SignalEvent(hResp);
    Platform::CloseEvent(hResp);

    hostThread.join();
    Platform::CloseShm(hMap, ptr, size);

    return passed ? 0 : 1;
}
