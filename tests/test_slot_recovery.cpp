// Slot recovery after a host-side response timeout.
//
// CONTRACT UNDER TEST: when a host send times out, the slot must not be left
// permanently unusable -- a later send on the same slot has to succeed.
//
// WHY THIS FILE WAS REWRITTEN (2026-08-02): the mock guest used to bound its
// waits with an ITERATION COUNT (`if (++sanity > 1000000) break;` around
// std::this_thread::yield()) and then proceeded to publish a response whether
// the wait had succeeded or not. Measured on this machine, 1,000,000 yields is
// ~105 ms, so the guest's SECOND wait -- which starts at t~250 ms -- gave up at
// t~355 ms, while the host's second send lands at t~350 ms (50 ms timeout +
// 300 ms sleep). A five-millisecond margin decided the run, which is why the
// test failed far more often than it passed (measured 5/8 and 4/5 on a pristine
// tree) and why it was worth almost nothing as a regression gate.
//
// The fix is to bound every wait by TIME, generously, and to make a wait that
// expires a LOUD FAILURE instead of falling through to a blind publish. The
// deadlines below are ~2 orders of magnitude larger than the intervals the test
// actually depends on, so scheduling noise cannot decide the outcome; if one of
// them ever expires, that is a real defect and the message says which wait.
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <shm/DirectHost.h>
#include <shm/Platform.h>

using namespace shm;

namespace {

constexpr int kHostTimeoutMs   = 50;    // call 1 is meant to time out
constexpr int kGuestStallMs    = 200;   // > kHostTimeoutMs, so call 1 DOES time out
constexpr int kHostSettleMs    = 300;   // host pause before the recovery send
constexpr int kCall2TimeoutMs  = 1000;  // call 2 is meant to succeed
constexpr int kStateWaitMs     = 10000; // wall-clock bound on a mock-guest wait

// Wait until `slot` reaches `want`, bounded by wall-clock time rather than by
// an iteration count. Returns false on expiry -- callers must treat that as a
// failure and NOT publish anything.
bool WaitForState(SlotHeader* slot, uint32_t want, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (slot->state.load(std::memory_order_acquire) != want) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

int main() {
    DirectHost host;
    std::string name = "SlotRecoveryTest";
    if (!host.Init(name, 1, 1024)) {
        std::cerr << "Host Init failed" << std::endl;
        return 1;
    }

    std::atomic<bool> guestFailed{false};
    std::string guestError;

    std::thread guest([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        ShmHandle hMap;
        bool exists;
        void* ptr = Platform::CreateNamedShm(name.c_str(), 1024 * 1024, hMap, exists);
        if (!ptr) {
            guestError = "guest: CreateNamedShm failed";
            guestFailed.store(true);
            return;
        }

        SlotHeader* slot = (SlotHeader*)((uint8_t*)ptr + sizeof(ExchangeHeader));

        std::string reqName = name + "_slot_0";
        std::string respName = name + "_slot_0_resp";
        EventHandle hReq = Platform::CreateNamedEvent(reqName.c_str());
        EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());

        // --- call 1: see the request, stall past the host's timeout, then
        //     answer anyway. The late answer is the point: it leaves a stale
        //     RESP_READY on a slot the host has already given up on, which is
        //     exactly the state recovery has to cope with.
        if (!WaitForState(slot, SLOT_REQ_READY, kStateWaitMs)) {
            guestError = "guest: timed out waiting for the FIRST request (SLOT_REQ_READY)";
            guestFailed.store(true);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(kGuestStallMs));
            slot->state.store(SLOT_RESP_READY, std::memory_order_release);
            Platform::SignalEvent(hResp);

            // --- call 2: the recovery send. This must arrive.
            if (!WaitForState(slot, SLOT_REQ_READY, kStateWaitMs)) {
                guestError = "guest: timed out waiting for the SECOND request (SLOT_REQ_READY) "
                             "- the host never re-published on the recovered slot";
                guestFailed.store(true);
            } else {
                slot->state.store(SLOT_RESP_READY, std::memory_order_release);
                Platform::SignalEvent(hResp);
            }
        }

        Platform::CloseEvent(hReq);
        Platform::CloseEvent(hResp);
        Platform::CloseShm(hMap, ptr, 1024 * 1024);
    });

    std::vector<uint8_t> resp;
    auto res = host.SendToSlot(0, nullptr, 0, MsgType::NORMAL, resp, kHostTimeoutMs);

    if (!res.HasError()) {
        std::cerr << "Call 1: expected timeout, got success" << std::endl;
        guest.join();
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(kHostSettleMs));

    res = host.SendToSlot(0, nullptr, 0, MsgType::NORMAL, resp, kCall2TimeoutMs);

    guest.join();

    if (guestFailed.load()) {
        std::cerr << guestError << std::endl;
        return 1;
    }

    if (res.HasError()) {
        std::cerr << "Call 2 failed (Recovery failed), error=" << (int)res.GetError() << std::endl;
        return 1;
    }

    std::cout << "Slot recovered after a response timeout." << std::endl;
    return 0;
}
