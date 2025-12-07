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
    std::string shmName = "ReproBugSHM";
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
    // We can just open with a large enough size or read the header first.
    // Let's just open 4096.
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
    size_t exchangeHeaderSize = sizeof(ExchangeHeader); // 64
    size_t perSlotTotal = sizeof(SlotHeader) + slotSize; // 128 + 1024 = 1152

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
    guestSlotHeader->msgType = MSG_TYPE_GUEST_CALL;
    guestSlotHeader->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

    // Host processes in background thread or loop.
    // We can just run ProcessGuestCalls once since we set the state.

    // Handler tries to return negative size (End Aligned)
    // It writes "DONE" to the START of the response buffer (because that's what it gets).
    // And returns -4.
    host.ProcessGuestCalls([&](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t msgType) -> int32_t {
        memcpy(resp, "DONE", 4);
        return -4;
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
    if (respSize != 4) {
        std::cerr << "Expected respSize 4 (Start Aligned), got " << respSize << std::endl;
        host.Shutdown();
        Platform::CloseShm(hMapFile, shmBase, 4096);
        return 1;
    }

    // Guest reads from START (Host flipped sign to avoid memmove)
    char* dataAtStart = (char*)guestRespBuf;

    // We don't print logic here, just check.
    bool success = false;
    if (strncmp(dataAtStart, "DONE", 4) == 0) {
        success = true;
    }

    host.Shutdown();
    Platform::CloseShm(hMapFile, shmBase, 4096);
    return success ? 0 : 1;
}
