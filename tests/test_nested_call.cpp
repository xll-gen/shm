#include <iostream>
#include <thread>
#include <cstring>
#include <atomic>
#include <vector>
#include <shm/DirectHost.h>

using namespace shm;

// Handler for Host that performs a nested call
int32_t nested_handler(DirectHost* host, const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxResp, MsgType type) {
    if (type == MsgType::GUEST_CALL) {
        // Check content
        if (reqSize >= 4 && memcmp(req, "CALL", 4) == 0) {
            // Perform Nested Call
            std::vector<uint8_t> nestedResp;
            std::string nestedData = "NESTED";

            // Send to Slot 0 (Host Slot)
            auto res = host->SendToSlot(0, (const uint8_t*)nestedData.data(), (int32_t)nestedData.size(), MsgType::NORMAL, nestedResp);

            if (res.HasError()) {
                std::cerr << "Host: Nested Send failed: " << (int)res.GetError() << std::endl;
                return 0;
            }

            if (nestedResp.size() == 5 && memcmp(nestedResp.data(), "REPLY", 5) == 0) {
                 // Success
                 if (maxResp >= 2) {
                     resp[0] = 'O';
                     resp[1] = 'K';
                     return 2;
                 }
            } else {
                 std::cerr << "Host: Unexpected nested response" << std::endl;
            }
        }
    }
    return 0;
}

void run_simulated_guest(std::string shmName, int numHostSlots, int numGuestSlots) {
    ShmHandle hMap;
    bool exists;
    size_t totalSize = 64 + (128 + 1024) * (numHostSlots + numGuestSlots);

    void* ptr = Platform::CreateNamedShm(shmName.c_str(), totalSize, hMap, exists);
    if (!ptr) {
        std::cerr << "Guest: Failed to map" << std::endl;
        return;
    }

    ExchangeHeader* exFull = (ExchangeHeader*)ptr;
    uint8_t* slotBase = (uint8_t*)ptr + sizeof(ExchangeHeader);
    size_t slotStride = sizeof(SlotHeader) + exFull->slotSize;

    std::atomic<bool> running{true};

    // Thread B: Process Host Requests on Slot 0
    std::thread worker([&]() {
        SlotHeader* header = (SlotHeader*)(slotBase + 0 * slotStride);
        uint8_t* dataBase = slotBase + 0 * slotStride + sizeof(SlotHeader);
        uint8_t* reqBuf = dataBase + exFull->reqOffset;
        uint8_t* respBuf = dataBase + exFull->respOffset;

        std::string reqName = shmName + "_slot_0";
        std::string respName = shmName + "_slot_0_resp";
        EventHandle hReq = Platform::CreateNamedEvent(reqName.c_str());
        EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());

        while(running) {
             uint32_t state = header->state.load(std::memory_order_acquire);
             if (state == SLOT_REQ_READY) {
                 // Process
                 // Assume it's "NESTED"
                 // Write "REPLY"
                 memcpy(respBuf, "REPLY", 5);
                 header->respSize = 5;
                 header->state.store(SLOT_RESP_READY, std::memory_order_release);
                 Platform::SignalEvent(hResp);
             } else {
                 Platform::WaitEvent(hReq, 10);
             }
        }
        Platform::CloseEvent(hReq);
        Platform::CloseEvent(hResp);
    });

    // Thread A: Make Guest Call on Slot 1
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Wait for host to start

    int guestSlotIdx = numHostSlots; // 1
    SlotHeader* header = (SlotHeader*)(slotBase + guestSlotIdx * slotStride);
    uint8_t* dataBase = slotBase + guestSlotIdx * slotStride + sizeof(SlotHeader);
    uint8_t* reqBuf = dataBase + exFull->reqOffset;
    uint8_t* respBuf = dataBase + exFull->respOffset;

    // Acquire
    int retries = 0;
    while(retries < 100) {
        uint32_t expected = SLOT_FREE;
        if (header->state.compare_exchange_strong(expected, SLOT_BUSY, std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        retries++;
    }

    if (header->state.load() == SLOT_BUSY) {
        header->msgType = MsgType::GUEST_CALL;
        header->reqSize = 4;
        memcpy(reqBuf, "CALL", 4);

        std::string respName = shmName + "_slot_" + std::to_string(guestSlotIdx) + "_resp";
        EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());
        // Signal is shared "guest_call"
        std::string reqName = shmName + "_guest_call";
        EventHandle hReq = Platform::CreateNamedEvent(reqName.c_str());

        header->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);
        Platform::SignalEvent(hReq);

        // Wait Response
        bool done = false;
        for(int i=0; i<500; ++i) { // 5s
             if (header->state.load(std::memory_order_acquire) == SLOT_RESP_READY) {
                 done = true;
                 break;
             }
             Platform::WaitEvent(hResp, 10);
        }

        if (done) {
             if (header->respSize == 2 && respBuf[0] == 'O' && respBuf[1] == 'K') {
                 std::cout << "Guest: Received OK from Nested Call" << std::endl;
             } else {
                 std::cerr << "Guest: Invalid response: " << header->respSize << std::endl;
             }
             header->state.store(SLOT_FREE, std::memory_order_release);
        } else {
            std::cerr << "Guest: Timeout waiting for response" << std::endl;
        }
        Platform::CloseEvent(hReq);
        Platform::CloseEvent(hResp);
    }

    running = false;
    worker.join();
    Platform::CloseShm(hMap, ptr, totalSize);
}

int main() {
    DirectHost host;
    std::string name = "TestNestedCall";

    // 1 Host Slot, 1 Guest Slot
    if (!host.Init(name, 1, 1024, 1)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    // Capture host pointer
    auto handler = [&](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxResp, MsgType type) -> int32_t {
        return nested_handler(&host, req, reqSize, resp, maxResp, type);
    };

    host.Start(handler, 10);

    std::thread guest(run_simulated_guest, name, 1, 1);
    if (guest.joinable()) guest.join();

    host.Stop();
    host.Shutdown();

    std::cout << "Test Finished" << std::endl;
    return 0;
}
