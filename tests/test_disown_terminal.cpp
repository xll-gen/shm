// Regression test for SPECIFICATION.md §3.5 "Disown is terminal (step 6) —
// MANDATORY (v0.8.16+)", C++ side.
//
// The clause binds BOTH implementations, but the deterministic per-act
// regression test existed only in Go (go/guest_slot_forfeit_test.go). C++'s
// BEHAVIOR was already correct — HeldSlot/ZeroCopySlot set slotIdx = -1 at the
// forfeit point and every accessor is gated on IsValid() — and
// test_held_slot.cpp covers the OUTCOME (the slot ends at SLOT_FREE and the
// wrapper has disowned it). What was missing is an assertion per FORBIDDEN ACT.
// The distinction matters: an outcome test still passes if a forfeited handle
// hands out buffers or republishes SLOT_REQ_READY, because both happen on a
// slot the outcome test has already stopped inspecting.
//
// §3.5 enumerates what a forfeiting sender MUST NOT do until it re-acquires:
//   (a) write reqSize / msgType / msgSeq,
//   (b) set its process-local active-wait flag,
//   (c) publish SLOT_REQ_READY (or any other state),
//   (d) hand out the request/response buffers.
// This test forfeits a HeldSlot via a response timeout, then asserts each act
// individually with the slot's header bytes snapshotted — so an intruding write
// is caught even when it writes a value the msgSeq guard would accept, which
// per the SPEC note it would (the intruder rewrites msgSeq too).
//
// (b) is asserted through its only observable consequence. activeWait is
// process-local (SlotAllocator::Slot, not SlotHeader) and not reachable from
// outside DirectHost, but §3.6.1 gates the intra-process zombie steal on
// activeWait == 0. So "a fresh TryAcquireHeldSlot can still steal the disowned
// slot" is exactly the statement that the forfeited handle left no active-wait
// flag behind.
//
// Deterministic: nothing answers the send, so the timeout is guaranteed; no
// sleeps beyond it and no reliance on an interleaving.
#include <iostream>
#include <vector>
#include <cstdint>
#include <shm/DirectHost.h>
#include <shm/Platform.h>

using namespace shm;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << std::endl;
        ++failures;
    }
}

int main() {
    DirectHost host;
    const std::string name = "DisownTerminalTest";
    if (!host.Init(name, 1, 1024)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    // Map the same segment a peer would, to read the raw SlotHeader the
    // forfeited wrapper is forbidden to touch.
    ShmHandle hMap;
    bool exists = false;
    void* base = Platform::CreateNamedShm(name.c_str(), 1024 * 1024, hMap, exists);
    if (!base) {
        std::cerr << "peer mapping failed" << std::endl;
        return 1;
    }
    SlotHeader* hdr = (SlotHeader*)((uint8_t*)base + sizeof(ExchangeHeader));

    {
        DirectHost::HeldSlot held = host.AcquireHeldSlot();
        check(held.IsValid(), "AcquireHeldSlot returned a valid handle");
        if (!held.IsValid()) return 1;

        check(held.Index() == 0, "the single host slot is index 0");
        check(held.GetReqBuffer() != nullptr, "a held slot hands out its request buffer");
        check(held.GetMaxReqSize() >= 64, "a held slot reports its buffer capacity");

        // Forfeit: nothing answers, so the send times out. Per §3.6(c) the
        // transaction is still in flight, so the holder disowns WITHOUT writing
        // state and the wrapper invalidates itself.
        std::vector<uint8_t> resp;
        held.GetReqBuffer()[0] = 0x11;
        auto sendRes = held.Send(1, (MsgType)1, resp, 100);
        check(sendRes.HasError(), "the unanswered held send must fail");
        check(sendRes.GetError() == Error::Timeout,
              "the unanswered held send must fail with Timeout");
        check(!held.IsValid(),
              "a timed-out held send must forfeit the handle (IsValid() == false)");
        check(held.Index() < 0, "a forfeited handle must report Index() < 0");

        // Snapshot everything a NEW owner's transaction would depend on. §3.5 is
        // explicit that the slot may already belong to someone else at this
        // point (zombie steal / lease reclaim), which is why each act below has
        // to be inert rather than merely "eventually harmless".
        const uint32_t wantState   = hdr->state.load(std::memory_order_acquire);
        const int32_t  wantReqSize = hdr->reqSize;
        const MsgType  wantMsgType = hdr->msgType;
        const uint32_t wantMsgSeq  = hdr->msgSeq;
        const uint64_t wantGen     = hdr->gen.load(std::memory_order_acquire);

        // (d) buffers must not be handed out.
        check(held.GetReqBuffer() == nullptr,
              "(d) a forfeited handle must NOT hand out the request buffer");
        check(held.GetMaxReqSize() == 0,
              "(d) a forfeited handle must NOT report a buffer capacity");

        // (a)(c) a re-Send must be refused BEFORE writing reqSize / msgType /
        // msgSeq or publishing a state.
        std::vector<uint8_t> resp2;
        auto again = held.Send(1, (MsgType)2, resp2, 100);
        check(again.HasError(), "(a,c) a re-Send on a forfeited handle must fail");
        check(again.GetError() == Error::InvalidArgs,
              "(a,c) the refusal must be InvalidArgs — rejected up front, not a second timeout");

        check(hdr->state.load(std::memory_order_acquire) == wantState,
              "(c) a forfeited handle must NOT publish any state");
        check(hdr->reqSize == wantReqSize, "(a) a forfeited handle must NOT write reqSize");
        check(hdr->msgType == wantMsgType, "(a) a forfeited handle must NOT write msgType");
        check(hdr->msgSeq == wantMsgSeq, "(a) a forfeited handle must NOT write msgSeq");
        check(hdr->gen.load(std::memory_order_acquire) == wantGen,
              "a forfeited handle must NOT bump gen (only an acquisition claims)");

        // Release() on a forfeited handle must not blind-store SLOT_FREE: the
        // transaction is in flight (or already re-owned), and freeing it here is
        // the double-ownership §3.5 forbids.
        held.Release();
        check(hdr->state.load(std::memory_order_acquire) == wantState,
              "Release() on a forfeited handle must NOT store SLOT_FREE over a live transaction");

        // (b) the forfeited handle left no active-wait flag: §3.6.1 gates the
        // intra-process zombie steal on activeWait == 0, so this acquisition
        // succeeding IS the assertion. (It also confirms the disowned slot is
        // recoverable rather than wedged.)
        DirectHost::HeldSlot stolen = host.TryAcquireHeldSlot();
        check(stolen.IsValid(),
              "(b) the disowned slot must still be stealable — a leftover active-wait flag "
              "would remove it from the zombie-steal set and wedge the slot");
        check(stolen.Index() == 0, "the stolen slot must be the same slot 0");
    }

    // ZeroCopySlot carries the same clause with different forfeit points. Its
    // handle-invalidation contract is what is pinned here: an invalid handle
    // hands out nothing and refuses to send. Constructed directly with idx = -1
    // so the check is deterministic and independent of pool state.
    {
        DirectHost::ZeroCopySlot zc(&host, -1);
        check(!zc.IsValid(), "a ZeroCopySlot built with idx = -1 is invalid");
        check(zc.GetReqBuffer() == nullptr,
              "(d) an invalid ZeroCopySlot must NOT hand out the request buffer");
        check(zc.GetMaxReqSize() == 0,
              "(d) an invalid ZeroCopySlot must NOT report a buffer capacity");
        auto r = zc.Send(1, (MsgType)3, 100);
        check(r.HasError() && r.GetError() == Error::InvalidArgs,
              "(a,c) an invalid ZeroCopySlot must refuse Send with InvalidArgs");
    }

    Platform::CloseShm(hMap, base, 1024 * 1024);
    host.Shutdown();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "test_disown_terminal: all checks passed" << std::endl;
    return 0;
}
