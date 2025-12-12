#include <iostream>
#include <vector>
#include <cstring>
#include <shm/DirectHost.h>

using namespace shm;

int main() {
    DirectHost host;
    std::string name = "ReproNestedCorruption";
    // Allocate 2 Host Slots to allow 1 level of recursion
    HostConfig config;
    config.shmName = name;
    config.numHostSlots = 2;
    config.payloadSize = 1024;

    if (!host.Init(config)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    {
        std::cout << "1. Acquiring Outer Slot..." << std::endl;
        auto slot1 = host.GetZeroCopySlot();
        if (!slot1.IsValid()) {
            std::cerr << "Failed to acquire Outer Slot" << std::endl;
            return 1;
        }

        // Write Data to Outer
        uint8_t* buf1 = slot1.GetReqBuffer();
        const char* data1 = "OUTER_DATA_PAYLOAD";
        memcpy(buf1, data1, strlen(data1) + 1);

        std::cout << "   Outer Slot acquired. Data written." << std::endl;

        {
            std::cout << "2. Acquiring Inner Slot (Recursion)..." << std::endl;
            auto slot2 = host.GetZeroCopySlot();
            if (!slot2.IsValid()) {
                std::cerr << "Failed to acquire Inner Slot" << std::endl;
                return 1;
            }

            // Check for collision
            uint8_t* buf2 = slot2.GetReqBuffer();
            if (buf1 == buf2) {
                std::cerr << "BUG DETECTED: Inner Slot points to same buffer as Outer Slot!" << std::endl;
                return 1;
            }

            // Write Data to Inner
            const char* data2 = "INNER_DATA";
            memcpy(buf2, data2, strlen(data2) + 1);

            std::cout << "   Inner Slot acquired. Data written." << std::endl;

            // Verify Outer Data Integrity
            if (memcmp(buf1, data1, strlen(data1) + 1) != 0) {
                std::cerr << "BUG DETECTED: Outer Data corrupted by Inner Write!" << std::endl;
                std::cerr << "Expected: " << data1 << ", Got: " << buf1 << std::endl;
                return 1;
            }

            std::cout << "3. Releasing Inner Slot..." << std::endl;
            // slot2 destroyed here
        }

        std::cout << "4. Verifying Outer Data again..." << std::endl;
        if (memcmp(buf1, data1, strlen(data1) + 1) != 0) {
            std::cerr << "BUG DETECTED: Outer Data corrupted after Inner Release!" << std::endl;
            return 1;
        }
    } // slot1 destroyed here

    std::cout << "Test Passed: Nested ZeroCopySlot usage is safe." << std::endl;

    // DirectHost destructor handles Shutdown safely
    return 0;
}
