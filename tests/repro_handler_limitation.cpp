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

// A "generic" handler that tries to write to the end.
// Now it receives maxRespSize, so it can do it correctly.

int32_t GenericHandler(const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxRespSize, uint32_t msgType) {
    const char* response = "DONE";
    int len = 4;

    // Correctly using maxRespSize
    if (maxRespSize < (uint32_t)len) return 0; // Safety check

    memcpy(resp + maxRespSize - len, response, len);
    return -len; // Negative indicates end-aligned
}

int main() {
    std::string shmName = "ReproHandlerLimitation";
    DirectHost host;
    // Init with a DIFFERENT size than assumed.
    // 2048 total slot size => ~1024 response size.
    if (!host.Init(shmName, 1, 2048, 1)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    // Attach manually as "Guest"
    ShmHandle hMapFile;
    bool exists = false;
    void* shmBase = Platform::CreateNamedShm(shmName.c_str(), 8192, hMapFile, exists);
    if (!shmBase) {
        std::cerr << "Guest failed to attach" << std::endl;
        host.Shutdown();
        return 1;
    }

    ExchangeHeader* ex = (ExchangeHeader*)shmBase;
    uint32_t slotSize = ex->slotSize; // 2048
    uint32_t respOffset = ex->respOffset; // 1024

    uint32_t numSlots = ex->numSlots;
    size_t exchangeHeaderSize = sizeof(ExchangeHeader);
    size_t perSlotTotal = sizeof(SlotHeader) + slotSize;

    uint8_t* ptr = (uint8_t*)shmBase + exchangeHeaderSize + (perSlotTotal * numSlots);
    SlotHeader* guestSlotHeader = (SlotHeader*)ptr;
    uint8_t* guestSlotData = ptr + sizeof(SlotHeader);
    uint8_t* guestReqBuf = guestSlotData + ex->reqOffset;
    uint8_t* guestRespBuf = guestSlotData + ex->respOffset;
    uint32_t maxRespSize = slotSize - respOffset; // Should be 1024

    // Simulate Guest sending a call
    const char* msg = "Hello";
    memcpy(guestReqBuf, msg, 5);
    guestSlotHeader->reqSize = 5;
    guestSlotHeader->msgType = MSG_TYPE_GUEST_CALL;
    guestSlotHeader->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

    // Host processes
    host.ProcessGuestCalls(GenericHandler);

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
        Platform::CloseShm(hMapFile, shmBase, 8192);
        return 1;
    }

    int32_t respSize = guestSlotHeader->respSize;
    // We expect negative size
    if (respSize != -4) {
        std::cerr << "Expected respSize -4, got " << respSize << std::endl;
        host.Shutdown();
        Platform::CloseShm(hMapFile, shmBase, 8192);
        return 1;
    }

    // Guest reads from ACTUAL END (based on maxRespSize = 1024)
    int32_t absSize = -respSize;
    uint32_t offset = maxRespSize - absSize; // 1024 - 4 = 1020
    char* dataAtEnd = (char*)guestRespBuf + offset;

    bool success = false;
    if (strncmp(dataAtEnd, "DONE", 4) == 0) {
        success = true;
        std::cout << "Success!" << std::endl;
    } else {
        std::cerr << "Data mismatch at end. Expected 'DONE' at offset " << offset << std::endl;
        // Check where it actually wrote
        char* wrongOffset = (char*)guestRespBuf + 512 - 4;
        if (strncmp(wrongOffset, "DONE", 4) == 0) {
             std::cerr << "Found data at offset " << (512-4) << " instead." << std::endl;
        } else {
             // Maybe at the very end?
        }
    }

    host.Shutdown();
    Platform::CloseShm(hMapFile, shmBase, 8192);
    return success ? 0 : 1;
}
