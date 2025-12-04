#include <iostream>
#include <thread>
#include <vector>
#include <chrono>
#include <atomic>
#include "src/DirectHost.h"
#include "include/IPCUtils.h"

using namespace shm;

void request_thread(DirectHost* host, int id) {
    std::string msg = "Hello " + std::to_string(id);
    std::vector<uint8_t> resp;
    std::cout << "[HostThread " << id << "] Sending Request..." << std::endl;
    bool ok = host->Request((const uint8_t*)msg.data(), msg.size(), resp);
    if (ok) {
        std::cout << "[HostThread " << id << "] Success: " << std::string(resp.begin(), resp.end()) << std::endl;
    } else {
        std::cout << "[HostThread " << id << "] Failed (Shutdown or Error)" << std::endl;
    }
}

int main() {
    std::string shmName = "DirectShutdownTestShm";
    DirectHost host;

    // 1. Init with 256 slots to match Guest expectation
    std::cout << "[Host] Initializing..." << std::endl;
    bool ok = host.Init(shmName, 256, [](std::vector<uint8_t>&&, uint32_t){});
    if (!ok) {
        std::cerr << "[Host] Failed to init SHM" << std::endl;
        return 1;
    }

    // 2. Launch Request Thread
    std::thread t1(request_thread, &host, 1);

    // 3. Sleep to let Request enter wait
    std::cout << "[Host] Sleeping 500ms..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 4. Shutdown while Request is waiting (Guest is sleeping 2s)
    std::cout << "[Host] Shutting Down..." << std::endl;
    host.Shutdown();

    t1.join();

    // 5. Re-Initialize to check if Guest is stuck
    std::cout << "[Host] Re-Initializing (New Instance)..." << std::endl;
    DirectHost host2;
    // Reuse SHM
    ok = host2.Init(shmName, 256, [](std::vector<uint8_t>&&, uint32_t){});
    if (!ok) {
        std::cerr << "[Host] Failed to re-init SHM" << std::endl;
        return 1;
    }

    // 6. Send another request
    std::cout << "[Host] Sending Second Request (Verification)..." << std::endl;
    std::vector<uint8_t> resp2;

    bool ok2 = host2.Request((const uint8_t*)"Retry", 5, resp2);
    if (ok2) {
        std::cout << "[Host] Verification Success!" << std::endl;
        return 0;
    } else {
        std::cout << "[Host] Verification Failed! (Guest Stuck)" << std::endl;
        return 1; // Bug Reproduced
    }
}
