#include <iostream>
#include <vector>
#include <thread>
#include <cstring>
#include <shm/DirectHost.h>

using namespace shm;

int main() {
    DirectHost host;
    // 1 Host Slot (idx 0), 1 Guest Slot (idx 1)
    if (!host.Init("ReproCrash", 1, 4096, 1)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    // Calculate total size to map it manually as a client
    // ExchangeHeader (64)
    // Slot 0: Header(128) + Data(4096) = 4224
    // Slot 1: Header(128) + Data(4096) = 4224
    // Total: 64 + 4224 + 4224 = 8512
    size_t totalSize = 8512;
    ShmHandle hMap;
    bool exists = false;
    void* clientAddr = Platform::CreateNamedShm("ReproCrash", totalSize, hMap, exists);

    if (!clientAddr) {
         std::cerr << "Failed to map client" << std::endl;
         return 1;
    }

    // Find the Guest Slot (Index 1)
    // Offset: ExchangeHeader (64) + Slot 0 (4224)
    uint8_t* slot1Ptr = (uint8_t*)clientAddr + 64 + 4224;
    SlotHeader* header = (SlotHeader*)slot1Ptr;

    // Corrupt the header with invalid negative size
    // SlotSize is 4096. MaxReqSize is 2048 (50% split).
    // We use -5000 to be strictly greater than both MaxReqSize and SlotSize.
    header->reqSize = -5000;
    header->msgType = MSG_TYPE_GUEST_CALL;
    header->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

    std::cout << "Processing Guest Calls with Invalid Size..." << std::endl;

    bool handlerCalled = false;
    // New Signature
    host.ProcessGuestCalls([&](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t msgType) -> int32_t {
        handlerCalled = true;
        std::cout << "Handler called with size: " << reqSize << std::endl;
        return 0;
    });

    if (handlerCalled) {
        std::cerr << "FAILED: Handler was called for invalid size!" << std::endl;
        return 1;
    }

    // Verify slot state was cleared to RESP_READY with size 0
    // Note: Host updates state to RESP_READY and signals RespEvent.
    // The loop in ProcessGuestCalls waits for nothing, it just returns.

    // We need to wait a tiny bit or just check? ProcessGuestCalls is synchronous for the processing part.
    // So the state change happened.

    if (header->state.load() != SLOT_RESP_READY) {
         std::cerr << "FAILED: Slot state was not reset to RESP_READY. State: " << header->state.load() << std::endl;
         return 1;
    }
    if (header->respSize != 0) {
         std::cerr << "FAILED: RespSize was not set to 0. Size: " << header->respSize << std::endl;
         return 1;
    }

    std::cout << "PASSED: Handler not called and slot reset." << std::endl;

    Platform::CloseShm(hMap, clientAddr, totalSize);
    host.Shutdown();
    return 0;
}
