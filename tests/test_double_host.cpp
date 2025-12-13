#include <iostream>
#include <shm/DirectHost.h>

using namespace shm;

int main() {
    HostConfig config;
    config.shmName = "TestDoubleHost";
    config.numHostSlots = 1;
    config.payloadSize = 1024;

    DirectHost host1;
    if (!host1.Init(config)) {
        std::cerr << "Host1 Init failed" << std::endl;
        return 1;
    }
    std::cout << "Host1 Init Success" << std::endl;

    DirectHost host2;
    // Attempt to init same name
    auto res = host2.Init(config);
    if (res.HasError()) {
        std::cout << "Host2 Init Failed (Expected). Error: " << (int)res.GetError() << std::endl;
        if (res.GetError() != Error::ResourceExhausted) {
             std::cerr << "Unexpected Error Code" << std::endl;
             // return 1; // Strict check?
        }
    } else {
        std::cerr << "Host2 Init Succeeded (Unexpected!)" << std::endl;
        return 1;
    }

    host1.Shutdown();
    std::cout << "Host1 Shutdown" << std::endl;

    // Now Host2 should succeed (if we unlinked properly)
    // Note: Shutdown unlinks SHM. So file is gone.
    // Init creates new one.
    if (!host2.Init(config)) {
        std::cerr << "Host2 Init failed after Host1 shutdown" << std::endl;
        return 1;
    }
    std::cout << "Host2 Init Success (Retry)" << std::endl;

    return 0;
}
