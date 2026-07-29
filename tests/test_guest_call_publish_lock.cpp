// Regression test for SPECIFICATION.md §3.5 "Responder publish rule" —
// the C++ mirror of Go's
// TestGuestResponderSteal_NoHeaderWriteBeforeOwnership
// (go/fastpath_test.go).
//
// v0.8.14 sealed the responder's publish TRANSITION (CAS from the owned
// SLOT_BUSY to SLOT_RESP_READY) but left the header-field writes that PRECEDE it
// unconditional:
//
//     if (absRespSize > slot->maxRespSize) {
//         respSize = 0;
//         slot->header->msgType = MsgType::SYSTEM_ERROR;   // <-- unconditional
//     }
//     slot->header->respSize = respSize;                   // <-- unconditional
//     PublishResponse(slot, i);                            // CAS fails -> "dropped"
//
// A handler slower than the Guest sender's response timeout loses the slot: the
// sender disowns it without writing `state`, the Guest's lease reclaimer frees
// it, and a fresh sender claims and republishes it under a NEW transaction. The
// two stores above then land on that new transaction. The publish CAS afterwards
// fails and the response bytes are correctly dropped — but the header is already
// poisoned, and the very next ProcessGuestCalls dispatch reads the poisoned
// msgType and hands the new request to the wrong handler branch. msgSeq is
// untouched (the responder never writes it), so the sender's msgSeq guard
// validates and the caller silently gets a wrong value.
//
// The fix is the two-phase publish: CAS(SLOT_BUSY -> SLOT_DONE) to prove
// ownership, THEN write header fields, THEN CAS(SLOT_DONE -> SLOT_RESP_READY).
//
// Per the §3.5 disown-terminal lesson, this asserts the individual ACTS, not
// just the final outcome: the request txn#2 is dispatched with must carry
// txn#2's own msgType/msgSeq, and respSize must still hold the sentinel the new
// sender left behind — proving the late responder wrote nothing at all.
//
// Determinism: handler #1 blocks on an atomic flag, so the reclaim + republish
// happens while the host provably owns the slot at SLOT_BUSY, and the late
// publish provably happens after the republish. No sleeps carry correctness.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <thread>
#include <shm/DirectHost.h>
#include <shm/IPCUtils.h>
#include <shm/Platform.h>

using namespace shm;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << std::endl;
        ++failures;
    }
}

static bool waitFlag(std::atomic<bool>& f, int timeoutMs) {
    for (int i = 0; i < timeoutMs; ++i) {
        if (f.load(std::memory_order_acquire)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return f.load(std::memory_order_acquire);
}

int main() {
    const std::string shmName = "TestGuestCallPublishLock";
    const size_t kMapSize = 4096;

    // txn#2's identity. Distinct from GUEST_CALL and from SYSTEM_ERROR so the
    // poisoning is observable at all — the single thing the pre-existing
    // guest-call tests could not see.
    const MsgType kType2 = (MsgType)200;
    const uint32_t kSeq2 = 4242;
    // A respSize no participant would ever legitimately write, so seeing it
    // intact proves the late responder did not touch the field.
    const int32_t kRespSentinel = -31337;

    DirectHost host;
    if (!host.Init(shmName, 1, 1024, 1)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    ShmHandle hMapFile;
    bool exists = false;
    void* shmBase = Platform::CreateNamedShm(shmName.c_str(), kMapSize, hMapFile, exists);
    if (!shmBase) {
        std::cerr << "Guest failed to attach" << std::endl;
        host.Shutdown();
        return 1;
    }

    ExchangeHeader* ex = (ExchangeHeader*)shmBase;
    const uint32_t numSlots = ex->numSlots;
    const uint32_t slotSize = ex->slotSize;
    uint8_t* slotBase = (uint8_t*)shmBase + sizeof(ExchangeHeader)
                        + (size_t)(sizeof(SlotHeader) + slotSize) * numSlots;
    SlotHeader* gh = (SlotHeader*)slotBase;
    uint8_t* gReq = slotBase + sizeof(SlotHeader) + ex->reqOffset;

    std::atomic<int> calls{0};
    std::atomic<bool> entered1{false};
    std::atomic<bool> release1{false};
    std::atomic<bool> entered2{false};
    std::atomic<uint32_t> seenMsgType2{0};
    std::atomic<int32_t> seenRespSize2{0};
    std::atomic<uint32_t> seenMsgSeq2{0};

    auto handler = [&](const uint8_t* req, int32_t reqSize, uint8_t* resp,
                       uint32_t maxRespSize, MsgType mt) -> int32_t {
        if (calls.fetch_add(1) == 0) {
            entered1.store(true, std::memory_order_release);
            while (!release1.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // Deliberately oversized: this drives the response-overflow branch,
            // which is the site that writes msgType before proving ownership.
            return (int32_t)maxRespSize + 64;
        }
        // Snapshot the header EXACTLY as the worker handed this request over,
        // before anything writes a response for it.
        seenMsgType2.store((uint32_t)mt, std::memory_order_relaxed);
        seenRespSize2.store(gh->respSize, std::memory_order_relaxed);
        seenMsgSeq2.store(gh->msgSeq, std::memory_order_relaxed);
        entered2.store(true, std::memory_order_release);
        if (reqSize > 0 && (uint32_t)reqSize <= maxRespSize) {
            memcpy(resp, req, (size_t)reqSize);
        }
        return reqSize;
    };

    // Drive ProcessGuestCalls from a dedicated thread: handler #1 blocks inside
    // it, so the reclaim has to run on the main (guest) thread.
    std::atomic<bool> pumpRun{true};
    std::thread pump([&]() {
        while (pumpRun.load(std::memory_order_acquire)) {
            host.ProcessGuestCalls(handler);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    // --- Guest publishes transaction #1 (§3.5 steps 1-3) ---------------------
    const char* req1 = "AAAAA";
    memcpy(gReq, req1, 5);
    gh->reqSize = 5;
    gh->msgType = MsgType::GUEST_CALL;
    gh->msgSeq = 1;
    gh->gen.fetch_add(1, std::memory_order_acq_rel);
    gh->lease.store(Platform::MonotonicNanos(), std::memory_order_release);
    gh->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

    check(waitFlag(entered1, 3000), "the host worker dispatched txn#1 to the handler");
    check(gh->state.load(std::memory_order_acquire) == SLOT_BUSY,
          "precondition: the host owns the slot at SLOT_BUSY while its handler runs");

    // --- The sender's response timeout fires; the reclaimer frees the slot ---
    // Replay of §3.6.1's reclaimer handshake verbatim: snapshot gen, observe
    // state, win the gen CAS, then CAS state -> SLOT_FREE.
    {
        uint64_t genObserved = gh->gen.load(std::memory_order_acquire);
        uint32_t observed = gh->state.load(std::memory_order_acquire);
        check(gh->gen.compare_exchange_strong(genObserved, genObserved + 1,
                                              std::memory_order_acq_rel),
              "reclaim replay: gen CAS must win (nothing else is claiming)");
        check(gh->state.compare_exchange_strong(observed, SLOT_FREE,
                                                std::memory_order_acq_rel),
              "reclaim replay: state CAS SLOT_BUSY -> SLOT_FREE must win");
    }

    // --- A fresh sender claims the recycled slot and publishes txn#2 ---------
    {
        gh->gen.fetch_add(1, std::memory_order_acq_rel);
        uint32_t expected = SLOT_FREE;
        check(gh->state.compare_exchange_strong(expected, SLOT_GUEST_BUSY,
                                                std::memory_order_acquire),
              "new sender: claim CAS SLOT_FREE -> SLOT_GUEST_BUSY must win");
        gh->lease.store(Platform::MonotonicNanos(), std::memory_order_release);
    }
    const char* req2 = "BBBB";
    memcpy(gReq, req2, 4);
    gh->reqSize = 4;
    gh->msgType = kType2;
    gh->msgSeq = kSeq2;
    // Not part of the sender's normal writes — a tripwire on a field only the
    // responder is supposed to touch, and only once it owns the slot.
    gh->respSize = kRespSentinel;
    gh->state.store(SLOT_REQ_READY, std::memory_order_seq_cst);

    // --- The late handler returns and attempts its publish ------------------
    release1.store(true, std::memory_order_release);

    check(waitFlag(entered2, 3000),
          "txn#2 must be dispatched to the handler by the next ProcessGuestCalls sweep");

    check(seenMsgType2.load(std::memory_order_relaxed) == (uint32_t)kType2,
          "txn#2 must be dispatched with ITS OWN msgType — the late responder must not "
          "write header.msgType before proving ownership");
    if (seenMsgType2.load(std::memory_order_relaxed) != (uint32_t)kType2) {
        std::cerr << "  seen msgType=" << seenMsgType2.load()
                  << " want " << (uint32_t)kType2
                  << " (SYSTEM_ERROR is " << (uint32_t)MsgType::SYSTEM_ERROR << ")" << std::endl;
    }
    check(seenRespSize2.load(std::memory_order_relaxed) == kRespSentinel,
          "header.respSize must still hold the sentinel at txn#2 dispatch — the late "
          "responder must not write header.respSize before proving ownership");
    if (seenRespSize2.load(std::memory_order_relaxed) != kRespSentinel) {
        std::cerr << "  seen respSize=" << seenRespSize2.load()
                  << " want " << kRespSentinel << std::endl;
    }
    check(seenMsgSeq2.load(std::memory_order_relaxed) == kSeq2,
          "header.msgSeq must be txn#2's at dispatch (the responder never writes msgSeq)");

    // --- txn#2 must complete correctly, on its own terms --------------------
    bool responded = false;
    for (int i = 0; i < 3000; ++i) {
        if (gh->state.load(std::memory_order_acquire) == SLOT_RESP_READY) {
            responded = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    check(responded, "txn#2 must reach SLOT_RESP_READY");
    check(gh->msgSeq == kSeq2, "txn#2's msgSeq must survive the exchange");
    check(gh->msgType == kType2,
          "txn#2's response must not be stamped SYSTEM_ERROR by the previous transaction");
    check(gh->respSize == 4, "txn#2's respSize must be its own echo length");

    pumpRun.store(false, std::memory_order_release);
    pump.join();

    Platform::CloseShm(hMapFile, shmBase, kMapSize);
    host.Shutdown();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "test_guest_call_publish_lock: all checks passed" << std::endl;
    return 0;
}
