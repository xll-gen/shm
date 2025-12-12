#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <shm/DirectHost.h>
#include <shm/IPCUtils.h>

using namespace shm;

void ThreadWorker(DirectHost* host, int slotIdx) {
    std::vector<uint8_t> resp;
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};

    // Send normal request
    auto result = host->SendToSlot(slotIdx, data, sizeof(data), MsgType::NORMAL, resp);
    if (result.HasError()) {
        if (result.GetError() == Error::ProtocolViolation) {
            std::cout << "Correctly received ProtocolViolation error." << std::endl;
            exit(0);
        } else {
            std::cerr << "Received wrong error: " << (int)result.GetError() << std::endl;
            exit(1);
        }
    } else {
         std::cerr << "Unexpected success! Should have failed." << std::endl;
         exit(1);
    }
}

int main() {
    DirectHost host;
    HostConfig config;
    config.shmName = "TestMsgSeqVal";
    config.numHostSlots = 1;
    config.payloadSize = 1024;

    if (!host.Init(config)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    std::thread t(ThreadWorker, &host, 0);

    // Manually act as Guest
    // Map existing SHM
    bool exists = false;
    ShmHandle hMap;
    // We try to open.
    void* ptr = Platform::CreateNamedShm("TestMsgSeqVal", 1024 + 4096, hMap, exists);

    if (!ptr) {
         // Retry loop in case host is slow to create
         for(int i=0; i<10; ++i) {
             std::this_thread::sleep_for(std::chrono::milliseconds(100));
             ptr = Platform::CreateNamedShm("TestMsgSeqVal", 1024 + 4096, hMap, exists);
             if (ptr) break;
         }
    }

    if (!ptr) {
        std::cerr << "Failed to open SHM" << std::endl;
        exit(1);
    }

    ExchangeHeader* ex = (ExchangeHeader*)ptr;
    (void)ex;

    // Slot 0 is at offset sizeof(ExchangeHeader)
    uint8_t* base = (uint8_t*)ptr + sizeof(ExchangeHeader);
    SlotHeader* slot = (SlotHeader*)base;

    // Wait for REQ_READY
    int attempts = 0;
    while (slot->state.load() != SLOT_REQ_READY) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        attempts++;
        if (attempts > 100) break;
    }

    if (slot->state.load() == SLOT_REQ_READY) {
        // Corrupt MsgSeq
        slot->msgSeq = 999999;

        // Signal Response
        slot->state.store(SLOT_RESP_READY);
        std::string respName = "TestMsgSeqVal_slot_0_resp";
        EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());
        Platform::SignalEvent(hResp);
        Platform::CloseEvent(hResp);
    } else {
        std::cerr << "Timed out waiting for REQ_READY" << std::endl;
        exit(1);
    }

    t.join();

    Platform::CloseShm(hMap, ptr, 1024+4096);
    return 0;
}
