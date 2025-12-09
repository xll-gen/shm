#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <atomic>
#include <shm/DirectHost.h>
#include <shm/Platform.h>
#include <shm/IPCUtils.h>

using namespace shm;

int main() {
    std::string shmName = "TestGuestCallNegativeResp";
    DirectHost host;
    // 1 Host Slot, 1 Guest Slot. Slot Size 1024.
    if (!host.Init(shmName, 1, 1024, 1)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    // Attach manually as "Guest"
    ShmHandle hMapFile;
    bool exists = false;
    // Total size calculation: Header + (SlotHeader + SlotSize) * 2
    // 64 + (128 + 1024) * 2 = 64 + 1152 * 2 = 64 + 2304 = 2368
    void* shmBase = Platform::CreateNamedShm(shmName.c_str(), 4096, hMapFile, exists);
    if (!shmBase) {
        std::cerr << "Guest failed to attach" << std::endl;
        host.Shutdown();
        return 1;
    }

    ExchangeHeader* ex = (ExchangeHeader*)shmBase;
    uint32_t numSlots = ex->numSlots; // 1
    uint32_t slotSize = ex->slotSize; // 1024
    uint32_t respOffset = ex->respOffset; // 512 (approx)

    // Guest Slot is at index 1 (numSlots)
    size_t exchangeHeaderSize = sizeof(ExchangeHeader);
    size_t perSlotTotal = sizeof(SlotHeader) + slotSize;

    uint8_t* ptr = (uint8_t*)shmBase + exchangeHeaderSize + (perSlotTotal * numSlots);
    SlotHeader* guestSlotHeader = (SlotHeader*)ptr;
    uint8_t* guestSlotData = ptr + sizeof(SlotHeader);
    uint8_t* guestReqBuf = guestSlotData + ex->reqOffset;
    uint8_t* guestRespBuf = guestSlotData + ex->respOffset;
    uint32_t maxRespSize = slotSize - respOffset;

    // Simulate Guest sending a call
    const char* msg = "Hello";
    memcpy(guestReqBuf, msg, 5);
    guestSlotHeader->reqSize = 5;
    guestSlotHeader->msgType = MsgType::GUEST_CALL;
    guestSlotHeader->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

    // Host processes in background thread or loop.
    // Handler writes to END of buffer (simulating zero-copy flatbuffer) and returns negative.
    host.ProcessGuestCalls([&](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxRespSize, MsgType msgType) -> int32_t {
        // Write "DONE" to end.
        const char* response = "DONE";
        int len = 4;
        // We can now use maxRespSize safely!
        if (maxRespSize >= (uint32_t)len) {
            memcpy(resp + maxRespSize - len, response, len);
            return -len;
        }
        return 0;
    });

    // Guest waits for response
    int retries = 0;
    while (guestSlotHeader->state.load(std::memory_order_acquire) != SLOT_RESP_READY) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        retries++;
        if (retries > 1000) break;
    }

    if (guestSlotHeader->state.load(std::memory_order_acquire) != SLOT_RESP_READY) {
        std::cerr << "Timeout waiting for host" << std::endl;
        host.Shutdown();
        Platform::CloseShm(hMapFile, shmBase, 4096);
        return 1;
    }

    int32_t respSize = guestSlotHeader->respSize;
    // We expect negative size now!
    if (respSize != -4) {
        std::cerr << "Expected respSize -4 (End Aligned), got " << respSize << std::endl;
        host.Shutdown();
        Platform::CloseShm(hMapFile, shmBase, 4096);
        return 1;
    }

    // Guest reads from END
    int32_t absSize = -respSize;
    uint32_t offset = maxRespSize - absSize;
    char* dataAtEnd = (char*)guestRespBuf + offset;

    bool success = false;
    if (strncmp(dataAtEnd, "DONE", 4) == 0) {
        success = true;
        std::cout << "Success!" << std::endl;
    } else {
        std::cerr << "Data mismatch at end. Got: " << std::string(dataAtEnd, 4) << std::endl;
    }

    host.Shutdown();
    Platform::CloseShm(hMapFile, shmBase, 4096);
    return success ? 0 : 1;
}
