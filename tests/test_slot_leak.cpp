#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <shm/DirectHost.h>

using namespace shm;

int main() {
    std::string shmName = "ReproLeakTest";
    Platform::UnlinkShm(shmName.c_str());
    Platform::UnlinkNamedEvent((shmName + "_slot_0").c_str());
    Platform::UnlinkNamedEvent((shmName + "_slot_0_resp").c_str());

    DirectHost host;
    if (!host.Init(shmName, 1, 1024)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    std::cout << "1. Sending with short timeout to induce leak..." << std::endl;
    {
        auto z = host.GetZeroCopySlot();
        // Send with 10ms timeout. No guest connected, so it will timeout.
        bool res = z.Send(10, MsgType::NORMAL, 10);
        if (res) {
            std::cerr << "Send should have timed out!" << std::endl;
            return 1;
        }
        std::cout << "   Timed out as expected." << std::endl;
        // z destructor will NOT release slot because Send invalidated it (slotIdx = -1).
        // Slot 0 is now stuck in SLOT_REQ_READY with activeWait=false.
    }

    std::cout << "2. Attempting to acquire slot again..." << std::endl;
    std::atomic<bool> acquired{false};

    std::thread t([&](){
        auto z2 = host.GetZeroCopySlot();
        acquired = true;
    });

    // Wait 500ms. If fix works, it should acquire instantly.
    // If bug exists, it will loop forever.
    auto start = std::chrono::steady_clock::now();
    while(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500)) {
        if (acquired) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!acquired) {
        std::cout << "FAIL: Restore failed. Slot leaked and caused hang." << std::endl;
        t.detach(); // Let it leak
        Platform::UnlinkShm(shmName.c_str());
        return 1;
    }

    t.join();
    std::cout << "PASS: Slot reclaimed successfully." << std::endl;

    Platform::UnlinkShm(shmName.c_str());
    return 0;
}
