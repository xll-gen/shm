#include <iostream>
#include <vector>
#include <cstring>
#include <shm/DirectHost.h>
#include <shm/Platform.h>

using namespace shm;

int main() {
    DirectHost host;
    HostConfig config;
    config.shmName = "TestGuestCallOverflow";
    config.numHostSlots = 0;
    config.numGuestSlots = 1;
    config.payloadSize = 1024; // Small payload

    if (!host.Init(config)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    // Access the guest slot directly to simulate a Guest
    // We reuse CreateNamedShm which handles opening existing SHM
    ShmHandle hMap;
    bool exists;
    uint64_t mapSize = 4096; // Should be enough for header + 1 slot
    void* addr = Platform::CreateNamedShm("TestGuestCallOverflow", mapSize, hMap, exists);

    if (!addr) {
         std::cerr << "Failed to map SHM" << std::endl;
         return 1;
    }

    ExchangeHeader* ex = (ExchangeHeader*)addr;
    uint8_t* base = (uint8_t*)addr + sizeof(ExchangeHeader);

    // Guest slots start after host slots. numHostSlots=0. So index 0.
    SlotHeader* slot = (SlotHeader*)base;

    // 1. Simulate Guest Request
    slot->reqSize = 10;
    slot->msgType = MsgType::GUEST_CALL;
    slot->state.store(SLOT_REQ_READY, std::memory_order_release);

    // 2. Define Handler that overflows
    auto handler = [](const uint8_t* req, int32_t reqSize, uint8_t* respBuf, uint32_t maxResp, MsgType type) -> int32_t {
        // Return 2x capacity
        return (int32_t)(maxResp * 2);
    };

    // 3. Process
    int processed = host.ProcessGuestCalls(handler);

    if (processed != 1) {
        std::cerr << "Failed to process guest call. Processed: " << processed << std::endl;
        Platform::CloseShm(hMap, addr, mapSize);
        return 1;
    }

    // 4. Verify Response Size
    int32_t respSize = slot->respSize;
    int32_t maxResp = (int32_t)(ex->slotSize - ex->respOffset);

    std::cout << "Max Resp: " << maxResp << ", Actual Resp: " << respSize << std::endl;

    bool passed = false;

    // Expectation: Should be 0 (Safe Failure)
    // Current Bug: Should be maxResp (Truncation)
    if (respSize == 0) {
        std::cout << "PASS: Response size is 0 (Safe)." << std::endl;
        passed = true;
    } else if (respSize == maxResp) {
        std::cout << "FAIL: Response size was truncated to max." << std::endl;
        passed = false;
    } else {
        std::cout << "FAIL: Unexpected response size." << std::endl;
        passed = false;
    }

    Platform::CloseShm(hMap, addr, mapSize);
    return passed ? 0 : 1;
}
