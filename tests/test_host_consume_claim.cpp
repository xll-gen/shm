// Regression test for the host-side response consume-claim (SPEC §3.4 "Host
// requester consume-claim").
//
// Defect: the host dropped its process-local `activeWait` flag while `state`
// was still SLOT_RESP_READY and only *then* validated msgSeq/msgType, read
// respSize, and copied the response out. tryClaimSlot's zombie branch steals
// any slot in {REQ_READY, RESP_READY, GUEST_BUSY} with `activeWait == 0`, so a
// sibling host thread could claim the slot mid-consume: the consumer then
// reads bytes the guest is overwriting for the new transaction, and its blind
// terminal SLOT_FREE store clobbers the thief's live claim (double-ownership).
//
// `activeWait` is process-local, so this is a host-multithread-only defect —
// the guest never sees the flag and needs no counterpart.
//
// Coverage:
//   1. ZeroCopySlot::Send      — the longest window (the caller holds the
//      response buffer until the wrapper dies) and the xll-gen production
//      path. Fully deterministic: after Send() returns success we are inside
//      the exposed window, so a competing claim from the SAME thread must
//      fail. Pre-fix this steal SUCCEEDS.
//   2. ZeroCopySlot::SendFlatBuffer — same, negative-size (tail-aligned) path.
//   3. SendAcquired / WaitForSlot — the copy-out windows are internal to the
//      call, so they are observed from the fake guest instead: after
//      publishing SLOT_RESP_READY the guest polls `state` and records the
//      FIRST value it transitions to. Fixed: SLOT_BUSY. Pre-fix: SLOT_FREE.
//      A large payload makes the copy-out window microseconds wide against a
//      tight polling loop, so the observation is not racy.
//   4. Timeout must NOT park the slot (transaction still in flight) — the
//      zombie/lease reclaim paths must still recover it.
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <vector>
#include <shm/DirectHost.h>
#include <shm/Platform.h>

using namespace shm;

static const uint32_t kPayload = 256 * 1024; // req half = resp half = 128 KiB
// Must not exceed the region DirectHost::Init created (CreateFileMapping on an
// existing name fails if a LARGER size is requested); MapViewOfFile(.., 0)
// maps the whole object regardless.
static const size_t   kMapSize = 64 /*ExchangeHeader*/ + 128 /*SlotHeader*/ + kPayload;

static std::atomic<bool> g_abortGuest{false};

static bool waitForState(SlotHeader* h, uint32_t want, int timeoutMs = 10000) {
    auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (h->state.load(std::memory_order_acquire) == want) return true;
        if (g_abortGuest.load(std::memory_order_acquire)) return false;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start).count();
        if (ms >= timeoutMs) return false;
        std::this_thread::yield();
    }
}

int main() {
    const std::string name = "HostConsumeClaimTest";

    DirectHost host;
    if (host.Init(name, 1, kPayload).HasError()) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    std::atomic<int> guestFailures{0};
    // First non-RESP_READY state the guest observes after publishing its
    // response, per round. SLOT_BUSY = the host consume-claimed (fixed);
    // SLOT_FREE = the host released before consuming (the defect).
    std::atomic<uint32_t> afterResp[2];
    afterResp[0].store(0xFFFFFFFF);
    afterResp[1].store(0xFFFFFFFF);
    std::atomic<bool> observeRound{false};
    std::atomic<int>  roundIdx{0};

    std::thread guest([&]() {
        ShmHandle hMap;
        bool exists;
        void* ptr = nullptr;
        for (int i = 0; i < 200 && !ptr; ++i) {
            ptr = Platform::CreateNamedShm(name.c_str(), kMapSize, hMap, exists);
            if (!ptr) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!ptr) { guestFailures++; return; }

        SlotHeader* h = (SlotHeader*)((uint8_t*)ptr + sizeof(ExchangeHeader));
        uint8_t* data = (uint8_t*)h + sizeof(SlotHeader);
        // Init: halfSize = payload/2 rounded down to 64; respOffset = halfSize.
        const uint32_t halfSize = ((kPayload / 2) / 64) * 64;
        uint8_t* respBuf = data + halfSize;
        const uint32_t maxResp = kPayload - halfSize;

        std::string respName = name + "_slot_0_resp";
        EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());

        // Rounds: 0 ZeroCopySlot::Send, 1 SendFlatBuffer,
        //         2 SendAcquired, 3 SendAcquiredAsync+WaitForSlot.
        for (int round = 0; round < 4; ++round) {
            if (!waitForState(h, SLOT_REQ_READY)) {
                if (!g_abortGuest.load(std::memory_order_acquire)) guestFailures++;
                break;
            }

            if (round == 1) {
                // Negative respSize: payload lives at the TAIL of respBuffer.
                h->respSize = -(int32_t)16;
                memset(respBuf + (maxResp - 16), 0xA5, 16);
            } else {
                int32_t n = (round >= 2) ? (int32_t)(maxResp / 2) : 16;
                h->respSize = n;
                memset(respBuf, 0x5A, (size_t)n);
            }

            h->state.store(SLOT_RESP_READY, std::memory_order_release);
            Platform::SignalEvent(hResp);

            if (round >= 2 && observeRound.load(std::memory_order_acquire)) {
                int slot = roundIdx.load(std::memory_order_acquire);
                auto obsStart = std::chrono::steady_clock::now();
                for (;;) {
                    uint32_t s = h->state.load(std::memory_order_acquire);
                    if (s != SLOT_RESP_READY) {
                        afterResp[slot].store(s, std::memory_order_release);
                        break;
                    }
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - obsStart).count();
                    if (ms >= 10000) { guestFailures++; break; }
                }
            }
        }

        Platform::CloseEvent(hResp);
        Platform::CloseShm(hMap, ptr, kMapSize);
    });

    int rc = 0;

    // --- 1. ZeroCopySlot::Send: slot must be unstealable while held. ---
    {
        auto zcs = host.GetZeroCopySlot();
        if (!zcs.IsValid()) { std::cerr << "GetZeroCopySlot failed" << std::endl; rc = 1; }

        if (rc == 0) {
            auto res = zcs.Send(8, MsgType::NORMAL, 5000);
            if (res.HasError()) { std::cerr << "ZeroCopySlot::Send failed" << std::endl; rc = 1; }
        }

        if (rc == 0 && host.AcquireSpecificSlot(0, 100) != -1) {
            std::cerr << "STEAL: AcquireSpecificSlot claimed a slot whose response "
                         "is still being held by ZeroCopySlot" << std::endl;
            rc = 1;
        }
        if (rc == 0 && host.TryAcquireSlot() != -1) {
            std::cerr << "STEAL: TryAcquireSlot claimed a slot whose response is "
                         "still being held by ZeroCopySlot" << std::endl;
            rc = 1;
        }
        // Response must still be intact and readable through the wrapper.
        if (rc == 0) {
            if (zcs.GetRespSize() != 16) {
                std::cerr << "Unexpected resp size " << zcs.GetRespSize() << std::endl;
                rc = 1;
            } else {
                const uint8_t* p = zcs.GetRespBuffer();
                for (int i = 0; i < 16 && rc == 0; ++i) {
                    if (p[i] != 0x5A) { std::cerr << "Response corrupted" << std::endl; rc = 1; }
                }
            }
        }
    } // destructor releases to SLOT_FREE

    // --- 2. ZeroCopySlot::SendFlatBuffer (negative respSize / tail payload). ---
    if (rc == 0) {
        auto zcs = host.GetZeroCopySlot();
        if (!zcs.IsValid()) { std::cerr << "GetZeroCopySlot #2 failed" << std::endl; rc = 1; }

        if (rc == 0 && zcs.SendFlatBuffer(8, 5000).HasError()) {
            std::cerr << "SendFlatBuffer failed" << std::endl;
            rc = 1;
        }
        if (rc == 0 && host.AcquireSpecificSlot(0, 100) != -1) {
            std::cerr << "STEAL: competing claim won against a held SendFlatBuffer "
                         "response" << std::endl;
            rc = 1;
        }
        if (rc == 0) {
            if (zcs.GetRespSize() != 16) {
                std::cerr << "Unexpected FB resp size " << zcs.GetRespSize() << std::endl;
                rc = 1;
            } else {
                const uint8_t* p = zcs.GetRespBuffer();
                for (int i = 0; i < 16 && rc == 0; ++i) {
                    if (p[i] != 0xA5) { std::cerr << "FB response corrupted" << std::endl; rc = 1; }
                }
            }
        }
    }

    // --- 3a. SendAcquired: state must go RESP_READY -> BUSY, not -> FREE. ---
    std::vector<uint8_t> resp;
    if (rc == 0) {
        observeRound.store(true, std::memory_order_release);
        roundIdx.store(0, std::memory_order_release);

        int32_t idx = host.AcquireSlot();
        if (idx < 0) { std::cerr << "AcquireSlot failed" << std::endl; rc = 1; }
        if (rc == 0 && host.SendAcquired(idx, 8, MsgType::NORMAL, resp, 5000).HasError()) {
            std::cerr << "SendAcquired failed" << std::endl;
            rc = 1;
        }
    }

    // --- 3b. SendAcquiredAsync + WaitForSlot: same invariant. ---
    if (rc == 0) {
        roundIdx.store(1, std::memory_order_release);

        int32_t idx = host.AcquireSlot();
        if (idx < 0) { std::cerr << "AcquireSlot #2 failed" << std::endl; rc = 1; }
        if (rc == 0 && host.SendAcquiredAsync(idx, 8, MsgType::NORMAL).HasError()) {
            std::cerr << "SendAcquiredAsync failed" << std::endl;
            rc = 1;
        }
        if (rc == 0 && host.WaitForSlot(idx, resp, 5000).HasError()) {
            std::cerr << "WaitForSlot failed" << std::endl;
            rc = 1;
        }
    }

    if (rc != 0) g_abortGuest.store(true, std::memory_order_release);
    guest.join();

    if (rc == 0) {
        const char* labels[2] = { "SendAcquired", "WaitForSlot" };
        for (int i = 0; i < 2; ++i) {
            uint32_t s = afterResp[i].load(std::memory_order_acquire);
            if (s != SLOT_BUSY) {
                std::cerr << "CONSUME-CLAIM MISSING (" << labels[i]
                          << "): slot left SLOT_RESP_READY for state " << s
                          << " (expected SLOT_BUSY=" << SLOT_BUSY
                          << "); the response copy-out ran on a stealable slot"
                          << std::endl;
                rc = 1;
            }
        }
    }

    // --- 4. Timeout must NOT park the slot: the transaction is still in
    //        flight, so the slot stays recoverable by the zombie/lease paths.
    if (rc == 0) {
        int32_t idx = host.AcquireSlot();
        if (idx < 0) { std::cerr << "AcquireSlot #3 failed" << std::endl; rc = 1; }
        if (rc == 0) {
            auto res = host.SendAcquired(idx, 8, MsgType::NORMAL, resp, 50);
            if (!res.HasError() || res.GetError() != Error::Timeout) {
                std::cerr << "Expected Timeout with no guest listening" << std::endl;
                rc = 1;
            }
        }
        // Left at SLOT_REQ_READY with activeWait cleared => zombie-stealable.
        if (rc == 0 && host.AcquireSpecificSlot(0, 1000) == -1) {
            std::cerr << "Timed-out slot was not recoverable by the zombie path"
                      << std::endl;
            rc = 1;
        }
    }

    if (guestFailures.load() != 0) {
        std::cerr << "Fake guest reported failures" << std::endl;
        rc = 1;
    }

    if (rc == 0) std::cout << "test_host_consume_claim PASS" << std::endl;
    return rc;
}
