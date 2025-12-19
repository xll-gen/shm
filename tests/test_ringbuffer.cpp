#include <shm/DirectHost.h>
#include <shm/RingBuffer.h>
#include <iostream>
#include <thread>
#include <vector>
#include <numeric>

using namespace shm;

int main() {
    HostConfig config;
    config.shmName = "RingBufferTest";
    config.numHostSlots = 1; // Single Slot
    config.payloadSize = 1024 * 1024; // 1MB

    DirectHost host;
    if (host.Init(config).HasError()) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    std::cout << "Host initialized. Launching Guest..." << std::endl;

    int32_t slotIdx = 0;
    RingBufferSender sender(&host, slotIdx);
    if (sender.Init().HasError()) {
        std::cerr << "Failed to init RingBufferSender" << std::endl;
        return 1;
    }

    std::atomic<bool> guestRunning{true};
    std::atomic<bool> errorFound{false};

    std::thread guestThread([&]() {
        // Simulate Guest reading
        uint8_t* reqBuf = host.GetReqBuffer(slotIdx);
        RingBufferHeader* header = (RingBufferHeader*)reqBuf;
        uint8_t* dataBase = reqBuf + sizeof(RingBufferHeader);
        uint32_t capacity = host.GetMaxReqSize(slotIdx) - sizeof(RingBufferHeader);

        uint64_t expectedVal = 0;

        while (guestRunning || header->writeOffset.load(std::memory_order_acquire) > header->readOffset.load(std::memory_order_relaxed)) {
            uint64_t w = header->writeOffset.load(std::memory_order_acquire);
            uint64_t r = header->readOffset.load(std::memory_order_relaxed);

            if (w > r) {
                uint32_t available = (uint32_t)(w - r);
                uint32_t readIdx = r % capacity;

                // Read one byte
                uint8_t val = dataBase[readIdx];
                if (val != (expectedVal & 0xFF)) {
                    std::cerr << "Mismatch at offset " << r << "! Expected " << (expectedVal & 0xFF) << " got " << (int)val << std::endl;
                    errorFound = true;
                    return;
                }
                expectedVal++;

                header->readOffset.store(r + 1, std::memory_order_release);

                // Simulate signalling back if needed (optional for this specific test)
            } else {
                 std::this_thread::yield();
            }
        }
    });

    // Host writes
    std::vector<uint8_t> data(1024); // Multiple of 256 to ensure continuous pattern
    std::iota(data.begin(), data.end(), 0);

    // Write 20MB (Enough to wrap around many times)
    for (int i = 0; i < 20000; ++i) {
        if (sender.Write(data.data(), data.size()).HasError()) {
            std::cerr << "Write failed" << std::endl;
            break;
        }
        if (errorFound) break;
    }

    guestRunning = false;
    guestThread.join();

    if (errorFound) {
        std::cerr << "Test Failed" << std::endl;
        return 1;
    }

    std::cout << "Test Finished Successfully" << std::endl;
    return 0;
}
