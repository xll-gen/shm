#include <shm/IPCHost.h>
#include <iostream>

int main() {
    shm::IPCHost host;
    // Just instantiate to trigger template/method compilation
    // Need to call a method that uses the constants
    // Init doesn't use them. Call does.

    // We can't actually init/run because we need SHM setup, but just linking is enough to check constants?
    // Constants are used in Call() and SendHeartbeat() and SendShutdown().
    // If we call them, the compiler will instantiate the code.

    // But we don't need to run it successfully.

    std::vector<uint8_t> resp;
    host.Call(nullptr, 0, resp);
    host.SendHeartbeat();
    host.SendShutdown();

    std::cout << "IPCHost compiled." << std::endl;
    return 0;
}
