#include <iostream>
#include <vector>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <shm/DirectHost.h>

int main() {
    shm::Platform::UnlinkShm("InitSafetyTest");
    shm::Platform::UnlinkNamedEvent("InitSafetyTest_slot_0");
    shm::Platform::UnlinkNamedEvent("InitSafetyTest_slot_0_resp");

    std::vector<int> fds;

    // Exhaust FDs
    while(true) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0) {
            if (errno == EMFILE || errno == ENFILE) break;
            break;
        }
        fds.push_back(fd);
    }

    if (fds.size() < 10) {
        // Cleanup and skip
        for(int fd : fds) close(fd);
        std::cout << "Skipped: Not enough FDs to simulate exhaustion" << std::endl;
        return 0;
    }

    // Free 1 FD for shm_open
    close(fds.back()); fds.pop_back();

    bool passed = false;

    {
        shm::DirectHost host;
        // This should fail gracefully because CreateNamedEvent (Req) will fail
        // (assuming CreateNamedShm took the last FD)
        bool res = host.Init("InitSafetyTest", 1, 1024);

        if (!res) {
            passed = true;
            std::cout << "Init correctly returned false." << std::endl;
        } else {
            std::cout << "Init returned true (BUG)." << std::endl;
             std::vector<uint8_t> resp;
             // This line ensures we crash if the fix is partial
             host.Send((const uint8_t*)"test", 4, 0, resp);
        }
    }

    for(int fd : fds) close(fd);
    shm::Platform::UnlinkShm("InitSafetyTest");

    return passed ? 0 : 1;
}
