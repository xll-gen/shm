#include <iostream>
#include <thread>
#include <atomic>
#include <shm/DirectHost.h>

using namespace shm;

int main() {
    DirectHost host;
    HostConfig config;
    config.shmName = "TestGuestCallRace";
    config.numHostSlots = 0;
    config.numGuestSlots = 1;
    config.payloadSize = 1024;

    if (!host.Init(config)) {
        return 1;
    }

    std::atomic<int> callCount{0};
    auto handler = [&](const uint8_t*, int32_t, uint8_t*, uint32_t, MsgType) -> int32_t {
        callCount++;
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Widen race window
        return 0;
    };

    std::atomic<bool> running{true};

    // Thread 1
    std::thread t1([&]() {
        while(running) {
            host.ProcessGuestCalls(handler);
        }
    });

    // Thread 2
    std::thread t2([&]() {
        while(running) {
            host.ProcessGuestCalls(handler);
        }
    });

    // Simulate Guest
    ShmHandle hMap;
    bool exists;
    void* ptr = Platform::CreateNamedShm("TestGuestCallRace", 4096, hMap, exists); // Sufficient size
    ExchangeHeader* ex = (ExchangeHeader*)ptr;
    uint8_t* base = (uint8_t*)ptr + sizeof(ExchangeHeader);
    SlotHeader* slot = (SlotHeader*)base; // Guest Slot 0

    for(int i=0; i<10; ++i) {
        // Wait Free
        while(slot->state.load() != SLOT_FREE) {
             if (slot->state.load() == SLOT_RESP_READY) {
                 slot->state.store(SLOT_FREE);
             }
             Platform::CpuRelax();
        }

        slot->msgType = MsgType::GUEST_CALL;
        slot->reqSize = 4;
        slot->state.store(SLOT_REQ_READY, std::memory_order_release);

        // Wait Response
        while(slot->state.load() != SLOT_RESP_READY) {
            Platform::CpuRelax();
        }

        // Consume
        slot->state.store(SLOT_FREE, std::memory_order_release);
    }

    running = false;
    t1.join();
    t2.join();
    Platform::CloseShm(hMap, ptr, 4096);

    std::cout << "Call Count: " << callCount << std::endl;
    if (callCount == 10) {
        std::cout << "PASS: No race detected." << std::endl;
        return 0;
    } else {
        std::cout << "FAIL: Race detected! Count=" << callCount << " (Expected 10)" << std::endl;
        return 1;
    }
}
