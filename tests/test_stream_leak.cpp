#include "shm/DirectHost.h"
#include "shm/Stream.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>

using namespace shm;

int main() {
    const std::string SHM_NAME = "TestStreamLeak";
    const int NUM_SLOTS = 4;
    const int PAYLOAD_SIZE = 4096;

    HostConfig config;
    config.shmName = SHM_NAME;
    config.numHostSlots = NUM_SLOTS;
    config.payloadSize = PAYLOAD_SIZE;

    DirectHost host;
    if (!host.Init(config)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }
    host.SetTimeout(100); // 100ms timeout

    std::atomic<bool> running{true};

    // Guest Thread simulation
    std::thread guest([&]() {
        // Map SHM manually
        ShmHandle hMap;
        bool exists;
        // Total size calculation (rough)
        size_t totalSize = 1024 * 1024; // Adequate for test
        void* base = Platform::CreateNamedShm(SHM_NAME.c_str(), totalSize, hMap, exists);
        if (!base) return;

        ExchangeHeader* ex = (ExchangeHeader*)base;
        SlotHeader* slots = (SlotHeader*)((uint8_t*)base + sizeof(ExchangeHeader));
        size_t slotStride = sizeof(SlotHeader) + ex->slotSize;

        while (running) {
            for (int i = 0; i < NUM_SLOTS; ++i) {
                SlotHeader* header = (SlotHeader*)((uint8_t*)slots + (i * slotStride));
                uint32_t state = header->state.load(std::memory_order_acquire);
                if (state == SLOT_REQ_READY) {
                    // Simulate processing
                    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // Cause timeout

                    // Respond (even though Host gave up)
                    header->respSize = 0;
                    header->msgType = MsgType::NORMAL;
                    header->state.store(SLOT_RESP_READY, std::memory_order_release);

                    // Signal event
                    std::string respName = SHM_NAME + "_slot_" + std::to_string(i) + "_resp";
                    EventHandle h = Platform::CreateNamedEvent(respName.c_str());
                    Platform::SignalEvent(h);
                    Platform::CloseEvent(h);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Platform::CloseShm(hMap, base, totalSize);
    });

    StreamSender sender(&host, 2);
    std::vector<uint8_t> data(10000, 0xAA); // Requires multiple chunks

    // Attempt sends that should timeout
    int timeouts = 0;
    for (int i = 0; i < 10; ++i) {
        auto res = sender.Send(data.data(), data.size(), i);
        if (res.GetError() == Error::Timeout) {
            timeouts++;
        }
        // If we leak slots, eventually AcquireSlot will hang/fail.
        // We rely on the fact that we can continue loop.
    }

    running = false;
    guest.join();
    host.Shutdown();

    if (timeouts > 0) {
        std::cout << "Successfully triggered timeouts and recovered." << std::endl;
        return 0;
    } else {
        std::cerr << "Did not trigger timeouts?" << std::endl;
        return 1;
    }
}
