// Regression test for the held-slot session API (v0.8.5, SPEC §3.6
// "Held-slot sessions"): SendHeld re-arms a claimed slot without the per-send
// FREE→re-claim cycle, parking it at SLOT_BUSY between sends.
//
// Covers:
//   1. Re-arm: two consecutive SendHeld calls on one acquisition both succeed.
//   2. Parked-BUSY is not claimable: AcquireSpecificSlot on the held slot
//      times out (tryClaimSlot's zombie branch never steals SLOT_BUSY).
//   3. Lease contract: a held slot whose lease has gone stale IS reclaimable
//      by TryReclaimAbandonedSlot (callers must keep the reclaim threshold
//      above their max inter-send gap).
//   4. msgSeq violation releases the slot to SLOT_FREE and the HeldSlot
//      wrapper disowns it.
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <shm/DirectHost.h>
#include <shm/Platform.h>

using namespace shm;

static bool waitForState(SlotHeader* slot, uint32_t want, int maxSpins = 5000000) {
    for (int i = 0; i < maxSpins; ++i) {
        if (slot->state.load(std::memory_order_acquire) == want) return true;
        std::this_thread::yield();
    }
    return false;
}

int main() {
    DirectHost host;
    std::string name = "HeldSlotTest";
    if (!host.Init(name, 1, 1024)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    std::atomic<int> guestFailures{0};
    std::thread guest([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        ShmHandle hMap;
        bool exists;
        void* ptr = Platform::CreateNamedShm(name.c_str(), 1024 * 1024, hMap, exists);
        if (!ptr) { guestFailures++; return; }

        SlotHeader* slot = (SlotHeader*)((uint8_t*)ptr + sizeof(ExchangeHeader));
        std::string respName = name + "_slot_0_resp";
        EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());

        // Rounds 1+2: well-behaved echo (leave msgSeq untouched).
        // Round 3: corrupt msgSeq to trigger the ProtocolViolation path.
        for (int round = 0; round < 3; ++round) {
            if (!waitForState(slot, SLOT_REQ_READY)) { guestFailures++; break; }
            slot->respSize = 0;
            if (round == 2) slot->msgSeq += 12345; // misbehaving peer
            slot->state.store(SLOT_RESP_READY, std::memory_order_release);
            Platform::SignalEvent(hResp);
        }

        Platform::CloseEvent(hResp);
        Platform::CloseShm(hMap, ptr, 1024 * 1024);
    });

    int rc = 0;
    std::vector<uint8_t> resp;
    {
        // 1. Acquire once, send twice (re-arm without re-claim).
        auto held = host.AcquireHeldSlot(0, 1000);
        if (!held.IsValid()) { std::cerr << "AcquireHeldSlot failed" << std::endl; rc = 1; }

        if (rc == 0 && held.Send(0, MsgType::NORMAL, resp, 2000).HasError()) {
            std::cerr << "Held send #1 failed" << std::endl;
            rc = 1;
        }
        if (rc == 0 && held.Send(0, MsgType::NORMAL, resp, 2000).HasError()) {
            std::cerr << "Held send #2 failed" << std::endl;
            rc = 1;
        }

        // 2. Parked at SLOT_BUSY: a competing claim must NOT succeed.
        if (rc == 0 && host.AcquireSpecificSlot(0, 100) != -1) {
            std::cerr << "Competing claim stole a held (parked-BUSY) slot" << std::endl;
            rc = 1;
        }

        // 3. Lease contract: once the lease is stale, the opt-in reclaim path
        //    may take the held slot.
        if (rc == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!host.TryReclaimAbandonedSlot(0, 1 /* ns — everything is stale */)) {
                std::cerr << "Stale-lease held slot was not reclaimable" << std::endl;
                rc = 1;
            }
        }
        held.Release(); // slot was reclaimed; release is a harmless FREE store
    }

    // 4. msgSeq violation: slot must end at SLOT_FREE and the wrapper disowned.
    if (rc == 0) {
        auto held = host.AcquireHeldSlot(0, 1000);
        if (!held.IsValid()) { std::cerr << "Re-acquire failed" << std::endl; rc = 1; }

        if (rc == 0) {
            auto res = held.Send(0, MsgType::NORMAL, resp, 2000);
            if (!res.HasError() || res.GetError() != Error::ProtocolViolation) {
                std::cerr << "Expected ProtocolViolation on corrupt msgSeq" << std::endl;
                rc = 1;
            }
            if (held.IsValid()) {
                std::cerr << "Wrapper still owns the slot after a violation" << std::endl;
                rc = 1;
            }
            // The violation path stores SLOT_FREE, so an ordinary claim works.
            if (rc == 0 && host.AcquireSpecificSlot(0, 1000) == -1) {
                std::cerr << "Slot not claimable after violation release" << std::endl;
                rc = 1;
            }
        }
    }

    guest.join();
    if (guestFailures.load() != 0) {
        std::cerr << "Fake guest reported failures" << std::endl;
        rc = 1;
    }

    if (rc == 0) std::cout << "test_held_slot PASS" << std::endl;
    return rc;
}
