#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <atomic>
#include <cstdlib>
#include <shm/DirectHost.h>
#include <shm/Platform.h>
#include <shm/IPCUtils.h>

using namespace shm;

int main() {
    std::string shmName = "TestHangSHM";

    // Cleanup first
    Platform::UnlinkShm(shmName.c_str());
    Platform::UnlinkNamedEvent((shmName + "_slot_0").c_str());
    Platform::UnlinkNamedEvent((shmName + "_slot_0_resp").c_str());

    DirectHost host;
    // 1 Host Slot.
    if (!host.Init(shmName, 1, 1024)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    // Connect as Guest (Manual)
    ShmHandle hMapFile;
    bool exists = false;
    uint64_t size = 4096; // Enough for header + 1 slot
    void* shmBase = Platform::CreateNamedShm(shmName.c_str(), size, hMapFile, exists);
    if (!shmBase) {
        std::cerr << "Guest attach failed" << std::endl;
        host.Shutdown();
        return 1;
    }

    // Prepare message
    std::vector<uint8_t> resp;
    std::vector<uint8_t> data(10, 0);

    std::cout << "Host sending... (This should hang if bug is present)" << std::endl;

    // Send should return -1 if timeout is implemented, or hang if not.
    int result = host.Send(data.data(), 10, MSG_TYPE_NORMAL, resp);

    std::cout << "Host returned: " << result << std::endl;

    host.Shutdown();
    Platform::CloseShm(hMapFile, shmBase, size);
    Platform::UnlinkShm(shmName.c_str());
    Platform::UnlinkNamedEvent((shmName + "_slot_0").c_str());
    Platform::UnlinkNamedEvent((shmName + "_slot_0_resp").c_str());

    // If result is negative, it means we detected the hang (timeout).
    if (result < 0) {
         return 0; // Success (Bug Fixed behavior)
    }

    // If result >= 0, it means it somehow succeeded?
    // Since Guest did nothing, this shouldn't happen unless logic is very broken.
    return 1;
}
