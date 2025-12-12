#include <iostream>
#include <vector>
#include <cstring>
#include <shm/DirectHost.h>

using namespace shm;

int main() {
    // 1. Declare ZeroCopySlot outside scope
    DirectHost::ZeroCopySlot z(nullptr, -1);

    {
        DirectHost host;
        HostConfig config;
        config.shmName = "TestUAF";
        config.numHostSlots = 1;

        if (!host.Init(config)) {
            std::cerr << "Init failed" << std::endl;
            return 1;
        }

        // 2. Acquire Slot
        z = host.GetZeroCopySlot();
        if (!z.IsValid()) {
             std::cerr << "Failed to acquire" << std::endl;
             return 1;
        }
        std::cout << "Acquired slot." << std::endl;

        // 3. Host goes out of scope here.
        // DirectHost destructor runs. slots vector is destroyed.
    }

    std::cout << "Host destroyed. Z is still alive." << std::endl;

    // 4. Z destructor runs here.
    // It calls host->slots[...].state.store(...)
    // host pointer is dangling (points to stack location of destroyed object).
    // host->slots is destroyed.
    // Use-After-Free!
    return 0;
}
