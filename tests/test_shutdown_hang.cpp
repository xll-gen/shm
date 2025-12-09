#include <iostream>
#include <thread>
#include <atomic>
#include <shm/DirectHost.h>
#include <unistd.h>
#include <signal.h>

void alarm_handler(int signum) {
    std::cerr << "Test timed out (HANG DETECTED)!" << std::endl;
    _exit(1);
}

int main() {
    shm::Platform::UnlinkShm("ShutdownHangTest");
    shm::Platform::UnlinkNamedEvent("ShutdownHangTest_slot_0");
    shm::Platform::UnlinkNamedEvent("ShutdownHangTest_slot_0_resp");

    signal(SIGALRM, alarm_handler);
    alarm(2); // 2 seconds timeout

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
    int res = host.Send(nullptr, 0, shm::MsgType::NORMAL, resp, 50);

    if (res != -1) {
        std::cerr << "Expected Send to fail/timeout" << std::endl;
        return 1;
    }

    std::cout << "Slot leaked. Attempting Shutdown..." << std::endl;
    // This should hang if the bug exists
    host.SendShutdown();

    std::cout << "Shutdown completed." << std::endl;
    return 0;
}
