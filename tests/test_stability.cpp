#include <iostream>
#include <vector>
#include <shm/DirectHost.h>
#include <shm/Result.h>

using namespace shm;

int main() {
    DirectHost host;
    std::string name = "StabilityTest";
    // Init with 1 slot
    if (!host.Init(name, 1, 1024)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    bool allPass = true;

    // Test 1: Send with nullptr data and non-zero size
    std::cout << "Test 1: Send(nullptr, 10)... ";
    std::vector<uint8_t> resp;
    auto res1 = host.Send(nullptr, 10, MsgType::NORMAL, resp);
    if (res1.HasError() && res1.GetError() == Error::InvalidArgs) {
        std::cout << "PASS (InvalidArgs detected)" << std::endl;
    } else {
        std::cout << "FAIL (Expected InvalidArgs, got " << (res1.HasError() ? "Error" : "Success") << ")" << std::endl;
        allPass = false;
    }

    // Test 2: SendToSlot with nullptr data and non-zero size
    std::cout << "Test 2: SendToSlot(0, nullptr, 10)... ";
    auto res2 = host.SendToSlot(0, nullptr, 10, MsgType::NORMAL, resp);
    if (res2.HasError() && res2.GetError() == Error::InvalidArgs) {
        std::cout << "PASS (InvalidArgs detected)" << std::endl;
    } else {
        std::cout << "FAIL (Expected InvalidArgs, got " << (res2.HasError() ? "Error" : "Success") << ")" << std::endl;
        allPass = false;
    }

    // Test 3: SendToSlot invalid index
    std::cout << "Test 3: SendToSlot(99, ...)... ";
    std::vector<uint8_t> data(10);
    auto res3 = host.SendToSlot(99, data.data(), 10, MsgType::NORMAL, resp);
    if (res3.HasError() && res3.GetError() == Error::ResourceExhausted) {
        // Implementation returns ResourceExhausted or maybe InvalidArgs depending on AcquireSpecificSlot?
        // AcquireSpecificSlot returns -1 if out of bounds.
        // SendToSlot returns ResourceExhausted if AcquireSpecificSlot returns -1.
        std::cout << "PASS (ResourceExhausted detected)" << std::endl;
    } else {
        std::cout << "FAIL (Expected ResourceExhausted, got " << (res3.HasError() ? "Error" : "Success") << ")" << std::endl;
        // Print error if possible
        allPass = false;
    }

    // Cleanup
    host.Shutdown();

    return allPass ? 0 : 1;
}
