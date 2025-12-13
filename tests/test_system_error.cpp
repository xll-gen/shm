#include <iostream>
#include <shm/DirectHost.h>
#include <thread>
#include <chrono>
#include <cstring>
#include <cassert>

using namespace shm;

int main() {
    HostConfig config;
    config.shmName = "TestSystemError";
    config.numHostSlots = 1;
    config.numGuestSlots = 1;
    config.payloadSize = 1024; // Small payload

    DirectHost host;
    if (!host.Init(config)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    bool exists;
    ShmHandle hMap;
    void* addr = Platform::CreateNamedShm("TestSystemError", 4096, hMap, exists);
    if (!addr) {
        std::cerr << "Failed to map SHM" << std::endl;
        return 1;
    }

    // Slot 1 Header Start = 64 + 1152 = 1216.
    uint64_t offsetSlot1Header = 64 + (128 + 1024);
    SlotHeader* header = (SlotHeader*)((uint8_t*)addr + offsetSlot1Header);

    // Set invalid ReqSize
    header->reqSize = 2048; // > 1024
    header->msgType = MsgType::GUEST_CALL;
    header->msgSeq = 99; // Simulating Guest Sequence
    header->state.store(SLOT_REQ_READY);

    std::cout << "Calling ProcessGuestCalls..." << std::endl;
    int processed = host.ProcessGuestCalls([](const uint8_t*, int32_t, uint8_t*, uint32_t, MsgType) -> int32_t {
        return 0;
    });

    if (processed != 1) {
        std::cerr << "ProcessGuestCalls failed to process slot. processed=" << processed << std::endl;
        return 1;
    }

    if (header->respSize != 0) {
        std::cerr << "Expected respSize 0, got " << header->respSize << std::endl;
        return 1;
    }

    if (header->msgType != MsgType::SYSTEM_ERROR) {
        std::cerr << "Expected MsgType::SYSTEM_ERROR (127), got " << (int)header->msgType << std::endl;
        return 1;
    }

    if (header->state.load() != SLOT_RESP_READY) {
        std::cerr << "Expected SLOT_RESP_READY, got " << header->state.load() << std::endl;
        return 1;
    }

    if (header->msgSeq != 99) {
        std::cerr << "Expected msgSeq 99, got " << header->msgSeq << std::endl;
        return 1;
    }

    Platform::CloseShm(hMap, addr, 4096);
    std::cout << "PASS" << std::endl;
    return 0;
}
