// Pins the index-range contract of DirectHost's slot accessors, documented in
// SlotAllocator.h.
//
// The ranges are deliberately NOT uniform: the request/response accessors take
// host slots only, [0, numSlots), while TryReclaimAbandonedSlot takes the full
// [0, numSlots + numGuestSlots) because crash recovery has to be role-blind
// (SPEC §3.6). That asymmetry had no test and no stated contract, so it read as
// an oversight — and the one accessor whose out-of-range value is easy to
// mistake for data, GetMaxReqSize returning 0, cost a downstream consumer a
// crash: xll-gen sized a FlatBuffers arena to it, got 0 bytes, and the
// allocator handed out nullptr on the first request.
//
// The load-bearing claim proved here is that 0 is UNAMBIGUOUS. DirectHost::Init
// floors the per-direction half-slot at 64 bytes, so every initialized slot
// reports >= 64 and a 0 can only mean "not a host slot". A consumer may
// therefore treat 0 as an error instead of a size.
#include <iostream>
#include <string>
#include <shm/DirectHost.h>

using namespace shm;

static int failures = 0;

static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << std::endl;
        ++failures;
    }
}

int main() {
    const uint32_t kHostSlots  = 2;
    const uint32_t kGuestSlots = 2;

    DirectHost host;
    if (!host.Init("SlotAccessorContractTest", kHostSlots, 1024, kGuestSlots)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    // A valid host slot reports a real capacity. 64 is the floor Init applies
    // (`if (halfSize < 64) halfSize = 64;`), which is what makes 0 a reliable
    // error signal rather than a plausible size.
    {
        DirectHost::HeldSlot held = host.AcquireHeldSlot();
        check(held.IsValid(), "AcquireHeldSlot returned a valid handle");
        check(held.GetMaxReqSize() >= 64,
              "an initialized host slot must report a capacity >= 64 (Init's half-slot floor) — "
              "this is what makes a 0 return unambiguously 'not a host slot'");
        check(held.GetReqBuffer() != nullptr,
              "an initialized host slot must have a non-null request buffer");
    }

    // Every host slot is inside the accessors' range.
    for (uint32_t i = 0; i < kHostSlots; ++i) {
        const std::string at = " (host slot " + std::to_string(i) + ")";
        check(host.GetMaxReqSize((int32_t)i) >= 64, "host slots report a capacity" + at);
        check(host.GetReqBuffer((int32_t)i) != nullptr, "host slots have a request buffer" + at);
    }

    // Guest-slot indices and out-of-range indices are NOT accepted by the
    // request/response accessors: those drive the host's own exchange, and a
    // guest slot's buffers/events belong to the reverse flow (GuestCallWorker).
    for (int32_t idx : {(int32_t)kHostSlots,                      // first guest slot
                        (int32_t)(kHostSlots + kGuestSlots - 1),  // last guest slot
                        (int32_t)(kHostSlots + kGuestSlots),      // past the end
                        (int32_t)-1}) {
        const std::string at = " (index " + std::to_string(idx) + ")";
        check(host.GetMaxReqSize(idx) == 0,
              "GetMaxReqSize must return 0 outside [0, numSlots)" + at);
        check(host.GetReqBuffer(idx) == nullptr,
              "GetReqBuffer must return nullptr outside [0, numSlots)" + at);
        check(!host.WaitForSlotEvent(idx, 0),
              "WaitForSlotEvent must return false outside [0, numSlots)" + at);
        host.SignalSlot(idx); // must be an inert no-op, not a crash
    }

    // The reclaim path takes the FULL range on purpose. It must not reject a
    // guest-slot index the way the accessors above do. All slots are fresh here,
    // so every call returns false — but for the documented reason (FREE / no
    // lease), not because the index was refused. The distinction is checked by
    // the out-of-range index, which must also be false.
    for (uint32_t i = 0; i < kHostSlots + kGuestSlots; ++i) {
        check(!host.TryReclaimAbandonedSlot((int32_t)i, 1),
              "a fresh slot is not reclaimable (index " + std::to_string(i) + ")");
    }
    check(!host.TryReclaimAbandonedSlot((int32_t)(kHostSlots + kGuestSlots), 1),
          "TryReclaimAbandonedSlot must refuse an index past numSlots + numGuestSlots");
    check(!host.TryReclaimAbandonedSlot(-1, 1),
          "TryReclaimAbandonedSlot must refuse a negative index");

    host.Shutdown();

    if (failures != 0) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "test_slot_accessor_contract: all checks passed" << std::endl;
    return 0;
}
