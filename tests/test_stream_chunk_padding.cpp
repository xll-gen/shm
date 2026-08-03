// Regression test: the C++ StreamSender must never put UNINITIALIZED bytes on the
// wire. Specifically `ChunkHeader::padding`@20 (SPEC §3.3.2), the last reserved
// slot in ChunkHeader, must be exactly zero in every STREAM_CHUNK request buffer.
//
// WHY THIS IS "MUST BE ZERO" AND NOT "HAPPENS TO BE ZERO"
// ------------------------------------------------------
// The whole 24-byte header is memcpy'd out of a host stack local into the
// guest-visible segment, so a field the sender does not assign ships whatever the
// host stack held. That is a 4-byte-per-chunk host memory disclosure to the peer
// process, but the protocol consequence is the larger one: SPEC §2.1's argument
// for why every earlier field addition (`lease`, `gen`, `fastPathAllowed`) needed
// no wire-version bump is that older peers wrote ZERO into the reserved space
// those fields were carved from. `padding`@20 is the only reserved space left in
// ChunkHeader. A shipped C++ sender writing garbage there permanently forfeits
// that carve-out: a future field at offset 20 could never distinguish "old peer"
// from "new peer wrote this value", so it would force a second version bump. Go's
// putChunkHeader (go/stream_sender.go) already writes an explicit 0. This is
// therefore a wire contract, and "the stack happened to be zero on this run" does
// not satisfy it.
//
// THE TEST'S OWN FAILABILITY
// --------------------------
// Uninitialized stack is very often incidentally zero, so an assertion that just
// reads offset 20 can pass vacuously. Two things address that, and the split
// between them matters — do not mistake the first for the second:
//
//   * dirtyStack() below scribbles 0xDE over the stack region that
//     StreamSender::Send's frame will occupy, so a genuinely unassigned `padding`
//     tends to read 0xDEDEDEDE rather than 0. This is BEST-EFFORT ONLY: frame
//     layout and inlining are not contractual. MEASURED — with `ChunkHeader ch;`
//     (default-init, the defect this test was written for) it reports
//     "padding@20 = 0xdededede" under MinGW/GCC 14 but PASSES under MSVC 19.41
//     /O2, where Send is inlined into main and the scribbled frame is never
//     reused. So it is a bug detector on one toolchain, not a guarantee anywhere.
//
//   * The load-bearing part is the assertion itself plus the fix being
//     `ChunkHeader ch{}` — value-initialization, which zeroes the whole struct
//     unconditionally rather than depending on someone remembering offset 20. The
//     test's failability is proven by mutation: writing 0xDEADBEEF into
//     `ch.padding` in include/shm/Stream.h makes this test fail with
//     "chunk 0: ChunkHeader.padding@20 = 0xdeadbeef, must be 0" on both
//     toolchains. That is what pins "any nonzero value here is a defect".
//
// Secondary assertion: StreamHeader@20 (`appMsgType`, which replaced the other
// formerly-reserved slot at SHM_VERSION 0x00080000) must carry the caller's tag
// verbatim. That pins the opposite property on the same offset — this one is
// written, not zeroed — so a fix that "zeroed everything" would be caught too.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <shm/DirectHost.h>
#include <shm/IPCUtils.h>
#include <shm/Stream.h>

using namespace shm;

namespace {

const uint32_t kAppMsgType = 0x5A5A1234u; // arbitrary, must round-trip verbatim

// Scribble a recognizable pattern over the stack below the caller's frame, which
// is where StreamSender::Send (called next, from the same frame) will place its
// ChunkHeader local. `volatile` keeps the stores from being elided; the array is
// deliberately larger than any plausible Send frame.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void dirtyStack() {
    volatile uint8_t scratch[8192];
    for (size_t i = 0; i < sizeof(scratch); ++i) scratch[i] = 0xDE;
    // Read one byte back so the loop cannot be optimized away as dead stores.
    if (scratch[0] != 0xDE) std::cerr << "unreachable" << std::endl;
}

} // namespace

int main() {
    DirectHost host;
    HostConfig config;
    config.shmName = "TestStreamChunkPadding";
    config.numHostSlots = 1;
    config.payloadSize = 512; // small slots, so one 4 KiB send makes many chunks

    if (host.Init(config).HasError()) {
        std::cerr << "FAIL: could not init host" << std::endl;
        return 1;
    }

    const uint64_t streamId = 0xABCDEF0123456789ull;

    std::atomic<bool> guestRunning{true};
    std::atomic<int> chunksSeen{0};
    std::atomic<int> startsSeen{0};
    std::atomic<uint32_t> badPadding{0};
    std::atomic<int> badPaddingChunk{-1};
    std::atomic<uint32_t> seenAppMsgType{0};
    std::atomic<bool> mapFailed{false};

    // Minimal mock guest: services slot 0 and inspects the RAW request bytes,
    // never the C++ struct — the point is what crossed the boundary.
    std::thread guest([&]() {
        ShmHandle hMap;
        bool exists = false;
        void* addr = Platform::CreateNamedShm(config.shmName.c_str(), 64, hMap, exists);
        if (!addr) {
            mapFailed = true;
            return;
        }
        ExchangeHeader* ex = static_cast<ExchangeHeader*>(addr);
        const uint32_t slotSize = ex->slotSize;
        const uint32_t numSlots = ex->numSlots;
        const uint32_t reqOffset = ex->reqOffset;
        const size_t totalSize =
            sizeof(ExchangeHeader) + numSlots * (sizeof(SlotHeader) + slotSize);
        Platform::CloseShm(hMap, addr, 64);

        addr = Platform::CreateNamedShm(config.shmName.c_str(), totalSize, hMap, exists);
        if (!addr) {
            mapFailed = true;
            return;
        }

        const std::string respName = config.shmName + "_slot_0_resp";
        EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());

        uint8_t* slotBase = static_cast<uint8_t*>(addr) + sizeof(ExchangeHeader);
        SlotHeader* header = reinterpret_cast<SlotHeader*>(slotBase);
        const uint8_t* reqBuf = slotBase + sizeof(SlotHeader) + reqOffset;

        while (guestRunning) {
            while (header->state.load(std::memory_order_acquire) != SLOT_REQ_READY) {
                if (!guestRunning) {
                    Platform::CloseEvent(hResp);
                    Platform::CloseShm(hMap, addr, totalSize);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(10));
            }

            const MsgType type = header->msgType;
            if (type == MsgType::STREAM_CHUNK) {
                uint64_t wireStreamId = 0;
                uint32_t wireIndex = 0;
                uint32_t wirePadding = 0;
                std::memcpy(&wireStreamId, reqBuf + 0, sizeof(wireStreamId));
                std::memcpy(&wireIndex, reqBuf + 8, sizeof(wireIndex));
                std::memcpy(&wirePadding, reqBuf + 20, sizeof(wirePadding));
                if (wireStreamId == streamId) {
                    if (wirePadding != 0 && badPaddingChunk.load() < 0) {
                        badPadding.store(wirePadding);
                        badPaddingChunk.store(static_cast<int>(wireIndex));
                    }
                    chunksSeen++;
                }
            } else if (type == MsgType::STREAM_START) {
                uint64_t wireStreamId = 0;
                uint32_t wireAppMsgType = 0;
                std::memcpy(&wireStreamId, reqBuf + 0, sizeof(wireStreamId));
                std::memcpy(&wireAppMsgType, reqBuf + 20, sizeof(wireAppMsgType));
                if (wireStreamId == streamId) {
                    seenAppMsgType.store(wireAppMsgType);
                    startsSeen++;
                }
            }

            header->respSize = 0;
            header->msgType = MsgType::NORMAL;
            header->state.store(SLOT_RESP_READY, std::memory_order_release);
            Platform::SignalEvent(hResp);
        }
        Platform::CloseEvent(hResp);
        Platform::CloseShm(hMap, addr, totalSize);
    });

    std::vector<uint8_t> data(4096);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i & 0xFF);

    dirtyStack();
    auto res = StreamSender(&host).Send(data.data(), data.size(), streamId, kAppMsgType);

    guestRunning = false;
    guest.join();
    host.Shutdown();

    if (mapFailed) {
        std::cerr << "FAIL: mock guest could not map the segment" << std::endl;
        return 1;
    }
    if (res.HasError()) {
        std::cerr << "FAIL: Send returned error " << static_cast<int>(res.GetError())
                  << std::endl;
        return 1;
    }
    if (startsSeen.load() != 1) {
        std::cerr << "FAIL: expected exactly 1 STREAM_START, saw " << startsSeen.load()
                  << std::endl;
        return 1;
    }
    if (chunksSeen.load() < 2) {
        std::cerr << "FAIL: expected several STREAM_CHUNKs, saw " << chunksSeen.load()
                  << " — the test would not be exercising the header write"
                  << std::endl;
        return 1;
    }

    if (badPaddingChunk.load() >= 0) {
        std::cerr << std::hex << std::showbase
                  << "FAIL: chunk " << std::dec << badPaddingChunk.load()
                  << ": ChunkHeader.padding@20 = " << std::hex << badPadding.load()
                  << ", must be 0. Uninitialized host stack crossed the process "
                     "boundary; SPEC 2.1's reserved-space carve-out requires this "
                     "slot to read as zero on the wire." << std::dec << std::endl;
        return 1;
    }

    if (seenAppMsgType.load() != kAppMsgType) {
        std::cerr << std::hex << std::showbase
                  << "FAIL: StreamHeader.appMsgType@20 = " << seenAppMsgType.load()
                  << ", expected " << kAppMsgType
                  << " — the caller's routing tag must reach the wire verbatim"
                  << std::dec << std::endl;
        return 1;
    }

    std::cout << "Test Passed (" << chunksSeen.load()
              << " chunks, ChunkHeader.padding@20 zero on every one)" << std::endl;
    return 0;
}
