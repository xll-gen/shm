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

    // Do NOT set global timeout (it is 10s default)
    // host.SetTimeout(2000);

    // Prepare message
    std::vector<uint8_t> resp;
    std::vector<uint8_t> data(10, 0);

    std::cout << "Host sending with 2000ms timeout... (Should timeout in ~2s)" << std::endl;

    auto start = std::chrono::steady_clock::now();
    // Pass 2000ms as per-call timeout
    int result = host.Send(data.data(), 10, MsgType::NORMAL, resp, 2000);
    auto end = std::chrono::steady_clock::now();

    std::cout << "Host returned: " << result << std::endl;
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Duration: " << duration << "ms" << std::endl;

    host.Shutdown();
    Platform::CloseShm(hMapFile, shmBase, size);
    Platform::UnlinkShm(shmName.c_str());
    Platform::UnlinkNamedEvent((shmName + "_slot_0").c_str());
    Platform::UnlinkNamedEvent((shmName + "_slot_0_resp").c_str());

    // Expect timeout (result -1)
    if (result < 0) {
         // Expect duration approx 2000ms
         if (duration >= 1900 && duration < 3000) {
             return 0; // Success
         }
         std::cerr << "Duration mismatch. Expected ~2000ms, got " << duration << "ms" << std::endl;
         return 1;
    }

    return 1;
}
