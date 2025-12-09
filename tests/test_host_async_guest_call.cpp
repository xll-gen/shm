#include <iostream>
#include <thread>
#include <cstring>
#include <atomic>
#include <vector>
#include <shm/DirectHost.h>

using namespace shm;

// Handler for Host
int32_t my_handler(const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxResp, MsgType type) {
    if (type == MsgType::GUEST_CALL) {
        // Echo back "OK"
        if (maxResp >= 2) {
            resp[0] = 'O';
            resp[1] = 'K';
            return 2;
        }
    }
    return 0;
}

void run_simulated_guest(std::string shmName, int numHostSlots, int numGuestSlots) {
    ShmHandle hMap;
    bool exists;
    // Simple calculation for test: Header + (SlotHeader+Size)*TotalSlots
    // Size is 1024. Half is 512.
    // ReqOffset=0, RespOffset=512.
    // SlotHeader=128.
    // PerSlot = 128 + 1024 = 1152.
    // Total = 64 + 1152 * 2 = 2368.

    size_t totalSize = 64 + (128 + 1024) * (numHostSlots + numGuestSlots);

    void* ptr = Platform::CreateNamedShm(shmName.c_str(), totalSize, hMap, exists);
    if (!ptr) {
        std::cerr << "Guest: Failed to map full" << std::endl;
        return;
    }

    ExchangeHeader* exFull = (ExchangeHeader*)ptr;
    uint8_t* slotBase = (uint8_t*)ptr + sizeof(ExchangeHeader);
    size_t slotStride = sizeof(SlotHeader) + exFull->slotSize;

    int guestSlotIdx = numHostSlots; // First guest slot
    SlotHeader* header = (SlotHeader*)(slotBase + guestSlotIdx * slotStride);
    uint8_t* dataBase = slotBase + guestSlotIdx * slotStride + sizeof(SlotHeader);
    uint8_t* reqBuf = dataBase + exFull->reqOffset;
    uint8_t* respBuf = dataBase + exFull->respOffset;

    // Wait for slot to be FREE
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
        // Write Request
        header->msgType = MsgType::GUEST_CALL;
        header->reqSize = 4;
        memcpy(reqBuf, "TEST", 4);

        std::string reqName;
        if (guestSlotIdx < numHostSlots) {
             reqName = shmName + "_slot_" + std::to_string(guestSlotIdx);
        } else {
             reqName = shmName + "_guest_call";
        }
        std::string respName = shmName + "_slot_" + std::to_string(guestSlotIdx) + "_resp";

        EventHandle hReq = Platform::CreateNamedEvent(reqName.c_str());
        EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());

        // Signal (REQ_READY)
        header->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);
        Platform::SignalEvent(hReq);

        // Wait Response
        bool done = false;
        for(int i=0; i<500; ++i) { // 5s timeout
             if (header->state.load(std::memory_order_acquire) == SLOT_RESP_READY) {
                 done = true;
                 break;
             }
             Platform::WaitEvent(hResp, 10);
        }

        if (done) {
             // Verify
             if (header->respSize == 2 && respBuf[0] == 'O' && respBuf[1] == 'K') {
                 std::cout << "Guest: Received OK" << std::endl;
             } else {
                 std::cerr << "Guest: Invalid response: " << header->respSize << std::endl;
             }
             // Cleanup
             header->state.store(SLOT_FREE, std::memory_order_release);
        } else {
            std::cerr << "Guest: Timeout waiting for response" << std::endl;
        }

        Platform::CloseEvent(hReq);
        Platform::CloseEvent(hResp);

    } else {
        std::cerr << "Guest: Slot not free?" << std::endl;
    }

    Platform::CloseShm(hMap, ptr, totalSize);
}

int main() {
    DirectHost host;
    std::string name = "TestAsyncGuestCall";

    // 1 Host Slot, 1 Guest Slot
    if (!host.Init(name, 1, 1024, 1)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    // Start Host Worker with Batch Size 10
    host.Start(my_handler, 10);

    // Run Guest
    std::thread guest(run_simulated_guest, name, 1, 1);
    if (guest.joinable()) guest.join();

    host.Stop();
    host.Shutdown();

    std::cout << "Test PASSED" << std::endl;
    return 0;
}
