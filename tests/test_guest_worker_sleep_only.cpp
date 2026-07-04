// Regression for the guest-call doorbell gate on the SLEEP-ONLY worker path
// (HostConfig::guestWorkerSpin=false, v0.8.6). The spin-default path is covered
// end-to-end by the benchmark and the guest_call_* suite; this exercises the
// branch a real Go guest sender drives when the host disables the spin phase:
// a request published while the worker is PARKED (having published
// hostState=WAITING) must be serviced via the gated doorbell, not left asleep.
//
// A fake in-process guest maps the same SHM (as test_slot_recovery does),
// waits until the worker parks (hostState==HOST_STATE_WAITING on the guest
// slot), then performs the exact two-sided Dekker the Go sender does: store
// state=SLOT_REQ_READY (seq_cst), load hostState (seq_cst), SignalEvent iff
// WAITING. It asserts the request is serviced within 2s — well under any
// pathological path. (Note: a fully lost doorbell would still self-heal at the
// worker's 1s WaitEvent cap + unconditional ProcessGuestCalls, so this is
// path coverage of the parked→doorbell→service flow and a hang/broken-servicing
// detector, not a sub-1s lost-wakeup detector per se.)
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <shm/DirectHost.h>
#include <shm/IPCUtils.h>
#include <shm/Platform.h>

using namespace shm;

int main() {
    const std::string name = "GuestWorkerSleepOnlyTest";
    const uint32_t numHostSlots = 1;
    const uint32_t numGuestSlots = 1;
    const uint32_t payloadSize = 4096;

    DirectHost host;
    HostConfig cfg;
    cfg.shmName = name;
    cfg.numHostSlots = numHostSlots;
    cfg.numGuestSlots = numGuestSlots;
    cfg.payloadSize = payloadSize;
    cfg.guestWorkerSpin = false; // <-- the path under test
    if (host.Init(cfg).HasError()) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    // Echo handler served by the sleep-only worker.
    host.Start([](const uint8_t* req, int32_t reqSize, uint8_t* resp,
                  uint32_t maxRespSize, MsgType) -> int32_t {
        int32_t n = reqSize;
        if (n > (int32_t)maxRespSize) n = (int32_t)maxRespSize;
        memcpy(resp, req, n);
        return n;
    });

    std::atomic<int> rc{0};
    std::thread guest([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        ShmHandle hMap;
        bool exists = false;
        void* base = Platform::CreateNamedShm(name.c_str(), 1024 * 1024, hMap, exists);
        if (!base) { rc = 2; return; }

        uint8_t* p = (uint8_t*)base;
        ExchangeHeader* ex = (ExchangeHeader*)p;
        const size_t exSize = 64; // max(sizeof(ExchangeHeader), 64), mirrors Init
        const size_t perSlot = sizeof(SlotHeader) + ex->slotSize;
        SlotHeader* gh = (SlotHeader*)(p + exSize + (size_t)ex->numSlots * perSlot);
        uint8_t* greq = (uint8_t*)gh + sizeof(SlotHeader) + ex->reqOffset;

        std::string reqEvName  = name + "_guest_call";
        std::string respEvName = name + "_slot_" + std::to_string(ex->numSlots) + "_resp";
        EventHandle hReq  = Platform::OpenEvent(reqEvName.c_str());
        EventHandle hResp = Platform::OpenEvent(respEvName.c_str());
        if (!hReq || !hResp) { rc = 3; return; }

        // Wait until the sleep-only worker has parked (published WAITING).
        int spins = 0;
        while (gh->hostState.load(std::memory_order_seq_cst) != HOST_STATE_WAITING) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (++spins > 5000) { rc = 4; return; } // worker never parked
        }

        // Publish a request and run the Go sender's doorbell Dekker.
        const char* payload = "ping";
        memcpy(greq, payload, 4);
        gh->reqSize = 4;
        gh->msgType = MsgType::GUEST_CALL;
        uint32_t seq = 12345;
        gh->msgSeq = seq;

        gh->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);
        if (gh->hostState.load(std::memory_order_seq_cst) == HOST_STATE_WAITING) {
            Platform::SignalEvent(hReq);
        }

        // Await the response, comfortably under the 1s park cap.
        auto start = std::chrono::steady_clock::now();
        bool got = false;
        while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2000)) {
            if (gh->state.load(std::memory_order_acquire) == SLOT_RESP_READY) { got = true; break; }
            Platform::WaitEvent(hResp, 50);
        }
        if (!got) { rc = 5; return; }         // lost wakeup / not serviced
        if (gh->respSize != 4) { rc = 6; return; }
        // Response payload lives at the resp buffer; verify the echoed bytes.
        uint8_t* gresp = (uint8_t*)gh + sizeof(SlotHeader) + ex->respOffset;
        if (memcmp(gresp, payload, 4) != 0) { rc = 7; return; }
        if (gh->msgSeq != seq) { rc = 8; return; }

        // Release.
        gh->state.store(SLOT_FREE, std::memory_order_release);
        Platform::CloseEvent(hReq);
        Platform::CloseEvent(hResp);
        Platform::CloseShm(hMap, base, 1024 * 1024);
    });

    guest.join();
    host.Stop();
    host.Shutdown();

    if (rc.load() != 0) {
        std::cerr << "test_guest_worker_sleep_only FAILED, rc=" << rc.load() << std::endl;
        return 1;
    }
    std::cout << "test_guest_worker_sleep_only PASS" << std::endl;
    return 0;
}
