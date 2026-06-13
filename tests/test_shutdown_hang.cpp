#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <shm/DirectHost.h>

int main() {
    shm::Platform::UnlinkShm("ShutdownHangTest");
    shm::Platform::UnlinkNamedEvent("ShutdownHangTest_slot_0");
    shm::Platform::UnlinkNamedEvent("ShutdownHangTest_slot_0_resp");

    // Portable watchdog: a detached thread force-exits the process if the test
    // body does not finish within the timeout (replaces POSIX SIGALRM/alarm).
    std::atomic<bool> done{false};
    std::thread watchdog([&done]() {
        for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!done.load(std::memory_order_acquire)) {
            std::cerr << "Test timed out (HANG DETECTED)!" << std::endl;
            std::_Exit(1);
        }
    });
    watchdog.detach();

    shm::DirectHost host;
    if (!host.Init("ShutdownHangTest", 1, 1024)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    // Manually leak the slot.
    // Send with a short timeout (50ms). No guest attached, so it will timeout.
    // Send returns -1, slot is leaked (State != FREE).
    std::cout << "Leaking slot 0 via timeout..." << std::endl;
    std::vector<uint8_t> resp;
    auto res = host.Send(nullptr, 0, shm::MsgType::NORMAL, resp, 50);

    if (!res.HasError()) {
        std::cerr << "Expected Send to fail/timeout" << std::endl;
        return 1;
    }

    std::cout << "Slot leaked. Attempting Shutdown..." << std::endl;
    // This should hang if the bug exists
    host.SendShutdown();

    std::cout << "Shutdown completed." << std::endl;
    done.store(true, std::memory_order_release);
    return 0;
}
