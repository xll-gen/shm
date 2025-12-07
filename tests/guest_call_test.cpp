#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <shm/DirectHost.h>

using namespace shm;

int main() {
    DirectHost host;
    // 1 Host Slot, 1 Guest Slot
    if (!host.Init("GuestCallTest", 1, 4096, 1)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    std::cout << "Host initialized. Waiting for Guest Call..." << std::endl;

    bool received = false;
    int timeout = 0;
    while (!received && timeout < 1000) { // 10s roughly
        int processed = host.ProcessGuestCalls([&](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t msgType) -> int32_t {
            std::cout << "Received Guest Call! Type: " << msgType << " Size: " << reqSize << std::endl;
            if (reqSize >= 4) {
                std::string s((const char*)req, 4);
                std::cout << "Data: " << s << std::endl;
            } else {
                std::cout << "Data too small" << std::endl;
            }

            // Write response
            std::string reply = "ACK";
            memcpy(resp, reply.data(), reply.size());
            return (int32_t)reply.size();
        });

        if (processed > 0) {
            received = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            timeout++;
        }
    }

    host.Shutdown();

    if (received) {
        std::cout << "Test Passed." << std::endl;
        return 0;
    } else {
        std::cerr << "Test Failed: Timeout" << std::endl;
        return 1;
    }
}
