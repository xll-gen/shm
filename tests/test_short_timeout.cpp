#include <iostream>
#include <chrono>
#include <shm/DirectHost.h>

using namespace shm;

int main() {
    DirectHost host;
    // 1 Slot
    if (!host.Init("TimeoutBug", 1, 1024, 0)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    std::cout << "Testing Send with 1ms timeout..." << std::endl;
    std::vector<uint8_t> resp;

    auto start = std::chrono::steady_clock::now();
    // Use a very small timeout.
    // Since there is no Guest, this MUST timeout.
    // The spin loop will fail quickly.
    // Then it should check timeout and return.
    auto res = host.Send(nullptr, 0, MsgType::NORMAL, resp, 1);
    auto end = std::chrono::steady_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "Send returned " << (res ? "Success" : "Error") << " after " << duration << "ms" << std::endl;

    if (!res.HasError()) {
        std::cerr << "FAILED: Send should have timed out and returned Error" << std::endl;
        return 1;
    }

    // We expect it to be fast. The spin loop (5000 iterations) takes negligible time (us).
    // If the bug exists (hardcoded 100ms wait), it will be >= 100ms.
    if (duration > 50) {
        std::cerr << "FAILED: Timeout took too long! Expected < 50ms, got " << duration << "ms" << std::endl;
        host.Shutdown();
        return 1;
    }

    std::cout << "PASSED" << std::endl;
    host.Shutdown();
    return 0;
}
