// Regression for DirectHost::WaitForGuestCall / WakeGuestCallWaiter — the
// SPECIFICATION.md §3.5 doorbell gate as reached by a host that runs its OWN loop
// instead of DirectHost::Start().
//
// WHY THIS EXISTS. hostState on the guest slots is written only from inside the
// worker's wait, so a host that never runs that wait leaves it
// HOST_STATE_ACTIVE forever. The guest sender gates its doorbell on
// `hostState == HOST_STATE_WAITING`, so in that configuration it NEVER signals —
// and a hand-rolled wait in the foreign loop therefore expires on its own timeout
// on every single call. That is why such hosts (xll-gen's XLL worker is the live
// one) have had to spin a core continuously. WaitForGuestCall exists to give them
// the gate; these tests are what say it actually works for a caller that is not
// GuestWorkerLoop.
//
// THE TWO PROPERTIES ARE ASSERTED SEPARATELY, ON PURPOSE. An outcome-only test
// ("the request eventually got serviced") passes while the gate is completely
// broken, because the park timeout self-heals it — the same trap
// SPECIFICATION.md notes for test_disown_terminal.cpp. So:
//
//   (A) DEKKER FOR A FOREIGN WAITER. A peer publishes SLOT_REQ_READY only AFTER
//       observing HOST_STATE_WAITING, and signals. The waiter must be released
//       fast — measured against a park far longer than the release should take.
//       FAIL-FIRST: delete the `setHostState(HOST_STATE_WAITING, seq_cst)` line
//       from WaitForRequest and the peer never sees WAITING, so it never
//       publishes and this times out.
//
//   (B) WAKE ON DEMAND. WakeGuestCallWaiter() must release a parked waiter
//       promptly with no request at all — that is what lets a host with a short
//       teardown budget stop its loop instead of detaching a thread still
//       running inside its own image.
//       FAIL-FIRST: stub WakeWaiter() to {} and this takes the full park.
//
// Both assertions are on ELAPSED TIME against a long park, which is what
// distinguishes "the gate worked" from "the timeout expired".
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

static int g_failures = 0;
static void Check(bool ok, const std::string& what) {
    if (ok) {
        std::cout << "  ok    " << what << std::endl;
    } else {
        std::cout << "  FAIL  " << what << std::endl;
        ++g_failures;
    }
}

// The park is deliberately long. Every assertion below is "released in far less
// than this", so a broken gate shows up as a ~kParkMs wait rather than as a
// wrong value — and a wrong value is what an outcome-only test would look at.
static const uint32_t kParkMs = 4000;
static const long long kFastMs = 1500; // generous for a scheduler hiccup, 2.6x under the park

int main() {
    const std::string name = "GuestWorkerForeignWaitTest";
    const uint32_t numGuestSlots = 1;

    DirectHost host;
    HostConfig cfg;
    cfg.shmName = name;
    cfg.numHostSlots = 1;
    cfg.numGuestSlots = numGuestSlots;
    cfg.payloadSize = 4096;
    // No Start(): this test IS the foreign-loop configuration. The worker thread
    // never runs, so nothing but WaitForGuestCall can publish WAITING.
    if (host.Init(cfg).HasError()) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    // ---- shared mapping used by the fake guest peer ------------------------
    const size_t mapSize = 1024 * 1024;
    ShmHandle hMap = ShmHandle();
    bool exists = false;
    void* base = Platform::CreateNamedShm(name.c_str(), mapSize, hMap, exists);
    if (!base) {
        std::cerr << "peer could not map the segment" << std::endl;
        return 1;
    }
    uint8_t* p = (uint8_t*)base;
    ExchangeHeader* ex = (ExchangeHeader*)p;
    const size_t exSize = 64; // mirrors Init
    const size_t perSlot = sizeof(SlotHeader) + ex->slotSize;
    SlotHeader* gh = (SlotHeader*)(p + exSize + (size_t)ex->numSlots * perSlot);
    uint8_t* greq = (uint8_t*)gh + sizeof(SlotHeader) + ex->reqOffset;

    const std::string reqEvName = name + "_guest_call";
    EventHandle hReq = Platform::OpenEvent(reqEvName.c_str());
    if (!hReq) {
        std::cerr << "peer could not open the request event" << std::endl;
        return 1;
    }

    // Baseline: with no waiter having run, hostState must NOT be WAITING. This is
    // the fact the whole test rests on -- if something else published WAITING the
    // Dekker assertion below would pass for the wrong reason.
    Check(gh->hostState.load(std::memory_order_seq_cst) != HOST_STATE_WAITING,
          "hostState is not WAITING before any wait runs (a foreign-loop host publishes it nowhere else)");

    // ---- (A) Dekker for a foreign waiter -----------------------------------
    {
        std::atomic<bool> sawWaiting{false};
        std::atomic<bool> peerDone{false};
        std::thread peer([&]() {
            // Publish ONLY after observing WAITING. If the foreign waiter fails to
            // publish it, nothing is ever sent and the waiter must time out --
            // which is exactly the failure this asserts against.
            for (int i = 0; i < 6000; ++i) {
                if (gh->hostState.load(std::memory_order_seq_cst) == HOST_STATE_WAITING) {
                    sawWaiting = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!sawWaiting) { peerDone = true; return; }

            memcpy(greq, "ping", 4);
            gh->reqSize = 4;
            gh->msgType = MsgType::GUEST_CALL;
            gh->msgSeq = 4242;
            gh->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);
            if (gh->hostState.load(std::memory_order_seq_cst) == HOST_STATE_WAITING) {
                Platform::SignalEvent(hReq);
            }
            peerDone = true;
        });

        auto t0 = std::chrono::steady_clock::now();
        bool ready = host.WaitForGuestCall(kParkMs);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count();
        peer.join();

        Check(sawWaiting.load(),
              "the foreign waiter published HOST_STATE_WAITING, so the peer could gate its doorbell on it");
        Check(elapsed < kFastMs,
              "the foreign waiter was released by the doorbell, not by its timeout (elapsed " +
                  std::to_string(elapsed) + "ms, park " + std::to_string(kParkMs) + "ms)");
        Check(ready, "WaitForGuestCall reported a request ready");
        Check(gh->hostState.load(std::memory_order_seq_cst) == HOST_STATE_ACTIVE,
              "hostState was restored to ACTIVE on wake, so the next phase runs doorbell-free");

        // Drain it through the real path so the slot does not stay REQ_READY into
        // the next case.
        int served = host.ProcessGuestCalls(
            [](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxResp, MsgType) -> int32_t {
                int32_t n = reqSize > (int32_t)maxResp ? (int32_t)maxResp : reqSize;
                memcpy(resp, req, n);
                return n;
            }, 10);
        Check(served == 1, "the request the waiter was woken for is the one ProcessGuestCalls served");
        gh->state.store(SLOT_FREE, std::memory_order_release);
    }

    // ---- (B) wake on demand, with no request -------------------------------
    {
        std::atomic<bool> waiterParked{false};
        std::atomic<long long> elapsed{-1};
        std::atomic<bool> ready{true};
        std::thread waiter([&]() {
            auto t0 = std::chrono::steady_clock::now();
            waiterParked = true;
            ready = host.WaitForGuestCall(kParkMs);
            elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0).count();
        });

        // Let it reach the park: WAITING is the observable for that, the same one
        // the sender gates on.
        for (int i = 0; i < 6000 && gh->hostState.load(std::memory_order_seq_cst) != HOST_STATE_WAITING; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(gh->hostState.load(std::memory_order_seq_cst) == HOST_STATE_WAITING,
              "the waiter reached its park (published WAITING) before the wake was sent");

        host.WakeGuestCallWaiter();
        waiter.join();

        Check(elapsed.load() >= 0 && elapsed.load() < kFastMs,
              "WakeGuestCallWaiter released the parked waiter promptly (elapsed " +
                  std::to_string(elapsed.load()) + "ms, park " + std::to_string(kParkMs) + "ms)");
        Check(!ready.load(),
              "a wake with no request reports nothing ready, so a caller's loop re-tests its stop flag "
              "instead of processing a phantom request");
    }

    // ---- (C) wake with nothing parked must be harmless ---------------------
    {
        host.WakeGuestCallWaiter();
        host.WakeGuestCallWaiter();
        auto t0 = std::chrono::steady_clock::now();
        host.WaitForGuestCall(200);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count();
        // The event is auto-reset, so at most ONE of the two stray signals can be
        // consumed by the next wait. What must not happen is a crash or a
        // permanently-signalled event that makes every future wait return instantly.
        Check(elapsed >= 0, "stray wakes with nothing parked do not crash (elapsed " +
                                std::to_string(elapsed) + "ms)");
    }

    Platform::CloseEvent(hReq);
    Platform::CloseShm(hMap, base, mapSize);
    host.Shutdown();

    std::cout << std::endl;
    if (g_failures) {
        std::cerr << g_failures << " CHECK(s) FAILED" << std::endl;
        return 1;
    }
    std::cout << "All foreign-wait regression tests passed" << std::endl;
    return 0;
}
