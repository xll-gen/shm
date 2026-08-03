// Regression test for the maxStreamSize ceiling (SPEC §3.3.4 bounds table,
// SPEC §3.3.2's "`dstOffset` is uint32, which suffices only because maxStreamSize
// is 1 GiB"), on BOTH sides of the C++ stream API.
//
// WHAT WAS WRONG
// --------------
// `ChunkHeader::dstOffset` became load-bearing at SHM_VERSION 0x00080000, but the
// bound that makes a uint32 sufficient was documentation only:
//   * `StreamReassemblerConfig::maxStreamSize` is a public mutable size_t with no
//     assert, no clamp and no test, while the Go peer's `MaxStreamSize` is a hard
//     const. A host that raised the knob accepted a STREAM_START its Go peer
//     rejects outright — an accept/reject parity break that SPEC §3.3.4 forbids in
//     so many words ("a stream accepted by one peer is accepted by the other").
//   * `StreamSender::Send` truncated instead of refusing:
//     `ch.dstOffset = (uint32_t)(size - remaining)` wraps past 4 GiB, so every
//     chunk after the wrap is placed at a wrong-but-in-bounds address — silent
//     corruption with no detectable signal.
//
// WHICH CEILING, AND WHY
// ----------------------
// 1 GiB (`shm::kMaxStreamSize`), not 4 GiB. Two bounds apply and the stricter one
// is enforced: 4 GiB is what a uint32 dstOffset can address, but 1 GiB is what the
// Go peer's hard const accepts and what SPEC §3.3.4 states normatively. Enforcing
// 4 GiB would leave the parity break — the actual reported defect — in place for
// the whole 1–4 GiB range. There is deliberately no second, looser "physically
// expressible" limit anywhere in the code: one number, one meaning.

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include <shm/DirectHost.h>
#include <shm/IPCUtils.h>
#include <shm/Stream.h>
#include <shm/StreamReassembler.h>

using namespace shm;

namespace {

std::vector<uint8_t> startReq(uint64_t streamId, uint64_t totalSize,
                              uint32_t totalChunks) {
    StreamHeader h{};
    h.streamId = streamId;
    h.totalSize = totalSize;
    h.totalChunks = totalChunks;
    h.appMsgType = 0;
    std::vector<uint8_t> buf(sizeof(StreamHeader));
    std::memcpy(buf.data(), &h, sizeof(h));
    return buf;
}

MsgType handleStart(StreamReassembler& r, const std::vector<uint8_t>& req) {
    uint8_t resp[64];
    size_t respSize = 0;
    MsgType mt = MsgType::STREAM_START;
    r.Handle(req.data(), req.size(), resp, respSize, mt);
    return mt;
}

bool receiverHalf() {
    StreamReassemblerConfig over;
    over.maxStreamSize = static_cast<size_t>(3) << 30; // 3 GiB: within uint32
                                                       // dstOffset range, but far
                                                       // above the Go peer's bound

    if (over.IsValid()) {
        std::cerr << "FAIL: StreamReassemblerConfig{maxStreamSize=3 GiB}.IsValid() "
                     "returned true; the wire ceiling is "
                  << kMaxStreamSize << std::endl;
        return false;
    }

    StreamReassembler r([](uint64_t, const std::vector<uint8_t>&) {}, over);

    // The over-set value must not survive construction. Observed through the
    // effective config rather than inferred, so the assertion names the number.
    if (r.Config().maxStreamSize != kMaxStreamSize) {
        std::cerr << "FAIL: constructor kept maxStreamSize = "
                  << r.Config().maxStreamSize << ", expected it clamped to "
                  << kMaxStreamSize << std::endl;
        return false;
    }

    // Behavioral consequence: a STREAM_START above the ceiling is refused even
    // though the caller asked for a config that would have admitted it. Note this
    // costs no memory — the bounds check in Handle precedes the buf.resize, so
    // nothing near 1 GiB is ever committed by this test.
    const uint64_t aboveCeiling = static_cast<uint64_t>(kMaxStreamSize) + 1;
    if (handleStart(r, startReq(1, aboveCeiling, 4)) != MsgType::SYSTEM_ERROR) {
        std::cerr << "FAIL: STREAM_START with totalSize " << aboveCeiling
                  << " was ACCEPTED; the Go peer rejects it, so the two "
                     "reassemblers disagree on the same stream" << std::endl;
        return false;
    }
    const uint64_t wayAbove = static_cast<uint64_t>(2) << 30; // 2 GiB
    if (handleStart(r, startReq(2, wayAbove, 4)) != MsgType::SYSTEM_ERROR) {
        std::cerr << "FAIL: STREAM_START with totalSize " << wayAbove
                  << " was ACCEPTED" << std::endl;
        return false;
    }

    // ...and the clamp must not have broken ordinary operation.
    if (handleStart(r, startReq(3, 4096, 4)) != MsgType::NORMAL) {
        std::cerr << "FAIL: a well-formed 4 KiB STREAM_START was rejected after "
                     "the clamp" << std::endl;
        return false;
    }

    // A config exactly AT the ceiling is legal and untouched.
    StreamReassemblerConfig atCeiling;
    atCeiling.maxStreamSize = kMaxStreamSize;
    if (!atCeiling.IsValid()) {
        std::cerr << "FAIL: maxStreamSize == kMaxStreamSize reported invalid"
                  << std::endl;
        return false;
    }
    StreamReassembler r2([](uint64_t, const std::vector<uint8_t>&) {}, atCeiling);
    if (r2.Config().maxStreamSize != kMaxStreamSize) {
        std::cerr << "FAIL: an at-ceiling config was altered by the constructor"
                  << std::endl;
        return false;
    }

    return true;
}

bool senderHalf() {
    DirectHost host;
    HostConfig config;
    config.shmName = "TestStreamSizeCeiling";
    config.numHostSlots = 1;
    config.payloadSize = 512;
    if (host.Init(config).HasError()) {
        std::cerr << "FAIL: could not init host" << std::endl;
        return false;
    }

    StreamSender sender(&host);

    // The size guard in StreamSender::Send runs before any slot is acquired and
    // before `data` is dereferenced, which is what makes this assertion cheap: a
    // one-byte buffer with an over-ceiling declared size never gets read, and no
    // guest is needed because nothing is sent. If the guard were ever moved after
    // slot acquisition this call would block instead of returning, so the test
    // also pins that ordering.
    uint8_t oneByte = 0;

    struct Case {
        const char* label;
        size_t size;
    };
    const Case cases[] = {
        {"kMaxStreamSize + 1", kMaxStreamSize + 1},
        {"2 GiB", static_cast<size_t>(2) << 30},
        // Past 4 GiB the uint32 dstOffset cast wraps: this is the case that
        // produced in-bounds-but-wrong placement rather than an error.
        {"4 GiB + 1 (dstOffset wrap)", (static_cast<size_t>(4) << 30) + 1},
        {"SIZE_MAX", static_cast<size_t>(-1)},
    };

    bool ok = true;
    for (const Case& c : cases) {
        auto res = sender.Send(&oneByte, c.size, 100);
        if (!res.HasError()) {
            std::cerr << "FAIL: StreamSender::Send accepted a " << c.label
                      << " stream; it cannot express that stream's offsets and the "
                         "peer would reject it anyway" << std::endl;
            ok = false;
        } else if (res.GetError() != Error::InvalidArgs) {
            std::cerr << "FAIL: StreamSender::Send(" << c.label
                      << ") returned error " << static_cast<int>(res.GetError())
                      << ", expected Error::InvalidArgs ("
                      << static_cast<int>(Error::InvalidArgs) << ")" << std::endl;
            ok = false;
        }
    }

    // NOT asserted here: that a size exactly AT the ceiling passes the guard. Such
    // a Send proceeds to acquire a slot and then blocks forever (no guest services
    // it in this process), and distinguishing "refused as too large" from "refused
    // for some later reason" is not possible through a single Error code. The
    // boundary is instead pinned by construction: the guard is `size >
    // kMaxStreamSize`, and tests/test_stream.cpp exercises the accepting side of it
    // end-to-end.

    host.Shutdown();
    return ok;
}

} // namespace

int main() {
    // Compile-time half of the same contract: the shipped default must be
    // expressible. (This mirrors the static_assert in StreamReassembler.h; keeping
    // a copy here means a header edit that deletes that assert still trips.)
    static_assert(StreamReassemblerConfig{}.maxStreamSize <= kMaxStreamSize,
                  "default maxStreamSize exceeds the wire ceiling");
    static_assert(kMaxStreamSize <= 0xFFFFFFFFull,
                  "kMaxStreamSize must be addressable by a uint32 ChunkHeader::dstOffset");

    if (!receiverHalf()) return 1;
    if (!senderHalf()) return 1;

    std::cout << "Test Passed (maxStreamSize ceiling enforced on both the "
                 "reassembler config and the sender)" << std::endl;
    return 0;
}
