// Receive-side delivery of StreamHeader::appMsgType@20 (SPEC §3.3.1
// "Receive-side delivery"). C++ twin of go/stream_apptype_delivery_test.go — the
// two are ONE artifact: the same four obligations, case for case.
//
// WHY THIS FILE EXISTS SEPARATELY FROM THE OTHER appMsgType CHECKS. Until this
// clause the field was WRITE-ONLY: both senders wrote it, both reassemblers
// parsed past it, and the two guards that existed could not see the gap.
//
//   * The send-side guard proves the byte reaches WIRE OFFSET 20. It says
//     nothing about anything downstream of the wire.
//   * The inertness tripwire at the end of test_reassembler_parity_table.cpp
//     proves the byte changes NOTHING. It is a negative assertion by
//     construction and would stay green against a reassembler that never touched
//     offset 20 at all — which is exactly the state it was written to describe.
//
// So neither could fail for "the tag never reaches the handler", and the checks
// below are the only thing that can.
//
// NON-VACUITY, and the trap this repo already documented for dstOffset (a parity
// table mistaken for a placement guard because two different implementations
// produce byte-identical output on every stream that is not dropped): ask
// whether each assertion below could hold if the field were never read. It could
// not.
//
//   * the delivered tag is asserted EQUAL to a value no other header field
//     carries, so "deliver a constant" and "deliver 0" both fail;
//   * one case asserts a ZERO tag on a header whose every other field is
//     nonzero, so "read totalChunks@16 / totalSize@8 / streamId@0 instead" fails;
//   * one case interleaves two streams with different tags and completes them in
//     the reverse of their start order, so a reassembler-wide "last tag seen"
//     member fails.
//
// Confirmed by mutation: delivering a hard-coded constant instead of
// ctx.appMsgType flips these red while the rest of ctest stays green.

#include <shm/StreamReassembler.h>
#include <shm/Stream.h>
#include <shm/IPCUtils.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace shm;

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL: " << (msg) << " (" << #cond << ") at line "    \
                      << __LINE__ << std::endl;                                \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

// Chosen so they collide with no other field of any header built here.
constexpr uint32_t kTagA = 0x1234ABCDu;
constexpr uint32_t kTagB = 0x0BADF00Du;

// appMsgType is REQUIRED, not defaulted: 0 is a legal tag, so a call site that
// forgot it would silently claim "unspecified".
std::vector<uint8_t> startReq(uint64_t streamId, uint64_t totalSize,
                              uint32_t totalChunks, uint32_t appMsgType) {
    StreamHeader h{};
    h.streamId = streamId;
    h.totalSize = totalSize;
    h.totalChunks = totalChunks;
    h.appMsgType = appMsgType;
    std::vector<uint8_t> buf(sizeof(StreamHeader));
    std::memcpy(buf.data(), &h, sizeof(h));
    return buf;
}

std::vector<uint8_t> chunkReq(uint64_t streamId, uint32_t chunkIndex,
                              uint32_t dstOffset,
                              const std::vector<uint8_t>& payload) {
    ChunkHeader h{};
    h.streamId = streamId;
    h.chunkIndex = chunkIndex;
    h.payloadSize = static_cast<uint32_t>(payload.size());
    h.dstOffset = dstOffset;
    h.padding = 0;
    std::vector<uint8_t> buf(sizeof(ChunkHeader) + payload.size());
    std::memcpy(buf.data(), &h, sizeof(h));
    if (!payload.empty()) {
        std::memcpy(buf.data() + sizeof(h), payload.data(), payload.size());
    }
    return buf;
}

MsgType handle(StreamReassembler& r, MsgType type,
               const std::vector<uint8_t>& req) {
    uint8_t resp[64];
    size_t respSize = 0;
    MsgType mt = type;
    bool handled = r.Handle(req.data(), req.size(), resp, respSize, mt);
    CHECK(handled, "Handle should claim stream messages");
    return mt;
}

std::vector<uint8_t> bytes(const char* s) {
    return std::vector<uint8_t>(s, s + std::strlen(s));
}

std::string hex32(uint32_t v) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) {
        out += digits[(v >> shift) & 0xF];
    }
    return out;
}

struct Delivery {
    uint64_t streamId;
    uint32_t appMsgType;
    std::vector<uint8_t> data;
};

// --------------------------------------------------------------------------
// The core end-to-end assertion the send-side guard could never make: the tag
// carried on STREAM_START reaches the completion HANDLER after a multi-chunk
// reassembly, i.e. it survives being stashed in the stream context across every
// chunk.
// --------------------------------------------------------------------------
void TypedHandlerReceivesTagFromStreamStart() {
    std::vector<Delivery> got;
    StreamReassembler r([&](uint64_t id, uint32_t tag, const std::vector<uint8_t>& d) {
        got.push_back(Delivery{id, tag, d});
    });

    const uint64_t streamId = 0xA1;
    CHECK(handle(r, MsgType::STREAM_START, startReq(streamId, 9, 3, kTagA)) == MsgType::NORMAL,
          "STREAM_START should be accepted");
    CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 0, 0, bytes("ABC"))) == MsgType::NORMAL, "chunk 0");
    CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 1, 3, bytes("DEF"))) == MsgType::NORMAL, "chunk 1");
    CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 2, 6, bytes("GHI"))) == MsgType::NORMAL, "chunk 2");

    CHECK(got.size() == 1, "exactly one completion");
    if (got.size() != 1) return;

    if (got[0].appMsgType != kTagA) {
        std::cerr << "FAIL: handler received appMsgType " << hex32(got[0].appMsgType)
                  << ", want " << hex32(kTagA) << std::endl;
        std::cerr << "  StreamHeader::appMsgType@20 did not reach the stream handler. The "
                     "send-side wire-offset guard and the inertness tripwire both stay GREEN "
                     "in that state -- this file is the only thing that can see it."
                  << std::endl;
        ++g_failures;
    }
    CHECK(got[0].streamId == streamId, "streamId delivered unchanged");
    CHECK(got[0].data == bytes("ABCDEFGHI"),
          "surfacing the tag must not perturb the delivered bytes");
}

// --------------------------------------------------------------------------
// The half a "does the tag arrive" check cannot cover. Every other field of this
// header is nonzero, so a receiver that read totalChunks@16 (3), totalSize@8 (9)
// or streamId@0 (0xB2) instead of appMsgType@20 delivers a nonzero value and
// fails here, while passing the case above only if it happened to read the right
// offset. Also pins obligation 4: 0 is a legal value, not "unset".
// --------------------------------------------------------------------------
void ZeroTagIsDeliveredVerbatim() {
    std::vector<Delivery> got;
    StreamReassembler r([&](uint64_t id, uint32_t tag, const std::vector<uint8_t>& d) {
        got.push_back(Delivery{id, tag, d});
    });

    const uint64_t streamId = 0xB2;
    handle(r, MsgType::STREAM_START, startReq(streamId, 9, 3, 0));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 0, 0, bytes("ABC")));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 1, 3, bytes("DEF")));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 2, 6, bytes("GHI")));

    CHECK(got.size() == 1, "exactly one completion");
    if (got.size() != 1) return;
    if (got[0].appMsgType != 0) {
        std::cerr << "FAIL: handler received appMsgType " << hex32(got[0].appMsgType)
                  << " for a header whose appMsgType@20 is 0, want 0." << std::endl;
        std::cerr << "  totalChunks@16 = 3, totalSize@8 = 9, streamId@0 = 0xB2 -- a nonzero "
                     "value here means the receiver read the WRONG FIELD, not that it read "
                     "nothing." << std::endl;
        ++g_failures;
    }
}

// --------------------------------------------------------------------------
// Kills the cheapest wrong implementation: remember the most recent appMsgType
// in the reassembler and hand that to every completion. Two streams are opened
// with different tags and the one started FIRST completes LAST, so a
// last-seen-wins receiver reports kTagB for both (SPEC §3.3.1 obligation 2).
// --------------------------------------------------------------------------
void TagIsPerStreamNotLastSeen() {
    std::vector<Delivery> got;
    StreamReassembler r([&](uint64_t id, uint32_t tag, const std::vector<uint8_t>& d) {
        got.push_back(Delivery{id, tag, d});
    });

    const uint64_t streamA = 0xC3;
    const uint64_t streamB = 0xD4;
    handle(r, MsgType::STREAM_START, startReq(streamA, 6, 2, kTagA));
    handle(r, MsgType::STREAM_START, startReq(streamB, 6, 2, kTagB));

    // Interleave, and finish B before A.
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamA, 0, 0, bytes("aaa")));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamB, 0, 0, bytes("bbb")));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamB, 1, 3, bytes("BBB")));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamA, 1, 3, bytes("AAA")));

    CHECK(got.size() == 2, "both streams complete");
    if (got.size() != 2) return;
    for (const Delivery& d : got) {
        const uint32_t want = (d.streamId == streamA) ? kTagA : kTagB;
        if (d.appMsgType != want) {
            std::cerr << "FAIL: stream " << d.streamId << " delivered appMsgType "
                      << hex32(d.appMsgType) << ", want " << hex32(want)
                      << " -- the tag is being taken from the most recent STREAM_START "
                         "rather than from this stream's own context" << std::endl;
            ++g_failures;
        }
    }
}

// --------------------------------------------------------------------------
// The second completion path (obligation 3). An empty stream never reaches the
// chunk path at all: it completes inside STREAM_START, so a fix applied only to
// the chunk-completion path leaves this one delivering 0.
// --------------------------------------------------------------------------
void EmptyStreamCarriesTheTag() {
    std::vector<Delivery> got;
    StreamReassembler r([&](uint64_t id, uint32_t tag, const std::vector<uint8_t>& d) {
        got.push_back(Delivery{id, tag, d});
    });

    const uint64_t streamId = 0xE5;
    CHECK(handle(r, MsgType::STREAM_START, startReq(streamId, 0, 0, kTagA)) == MsgType::NORMAL,
          "empty STREAM_START should be accepted");

    CHECK(got.size() == 1, "an empty stream completes inside STREAM_START");
    if (got.size() != 1) return;
    if (got[0].appMsgType != kTagA) {
        std::cerr << "FAIL: empty-stream delivery carried appMsgType "
                  << hex32(got[0].appMsgType) << ", want " << hex32(kTagA)
                  << " -- the totalChunks==0 completion path is a SECOND delivery site and "
                     "must carry the tag too" << std::endl;
        ++g_failures;
    }
    CHECK(got[0].data.empty(), "empty stream delivers no bytes");
}

// --------------------------------------------------------------------------
// Last clause of obligation 2: a STREAM_START that replaces a live context for
// the same streamId replaces its tag along with the rest of its state. A
// receiver that captured the tag once and refused to overwrite it reports kTagA.
// --------------------------------------------------------------------------
void RestartReplacesTheTag() {
    std::vector<Delivery> got;
    StreamReassembler r([&](uint64_t id, uint32_t tag, const std::vector<uint8_t>& d) {
        got.push_back(Delivery{id, tag, d});
    });

    const uint64_t streamId = 0xF6;
    handle(r, MsgType::STREAM_START, startReq(streamId, 6, 2, kTagA));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 0, 0, bytes("xxx")));

    handle(r, MsgType::STREAM_START, startReq(streamId, 6, 2, kTagB));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 0, 0, bytes("yyy")));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 1, 3, bytes("zzz")));

    CHECK(got.size() == 1, "exactly one completion after the restart");
    if (got.size() != 1) return;
    if (got[0].appMsgType != kTagB) {
        std::cerr << "FAIL: delivered appMsgType " << hex32(got[0].appMsgType) << ", want "
                  << hex32(kTagB) << " -- a STREAM_START that replaces a live context must "
                     "replace its tag too" << std::endl;
        ++g_failures;
    }
}

// --------------------------------------------------------------------------
// Obligation 3's second half: a dropped stream fires no completion at all, tag
// included. Chunk 1 points out of bounds on a 6-byte stream.
// --------------------------------------------------------------------------
void DroppedStreamDeliversNothing() {
    std::vector<Delivery> got;
    StreamReassembler r([&](uint64_t id, uint32_t tag, const std::vector<uint8_t>& d) {
        got.push_back(Delivery{id, tag, d});
    });

    const uint64_t streamId = 0x17;
    handle(r, MsgType::STREAM_START, startReq(streamId, 6, 2, kTagA));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 0, 0, bytes("xxx")));
    CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 1, 99, bytes("yyy"))) ==
              MsgType::SYSTEM_ERROR,
          "an out-of-bounds chunk must drop the stream");

    CHECK(got.empty(), "a dropped stream must not complete");
}

// --------------------------------------------------------------------------
// Obligation 1, the additive one: the pre-existing (streamId, data) constructor
// keeps its signature and its behavior with a tag set on the wire. If this
// change had modified OnStreamFn instead of adding a parallel one, this function
// would not COMPILE — which is the point.
// --------------------------------------------------------------------------
void UntypedConstructorIsUnchanged() {
    std::vector<std::vector<uint8_t>> delivered;
    uint64_t seenId = 0;
    StreamReassembler r([&](uint64_t id, const std::vector<uint8_t>& d) {
        seenId = id;
        delivered.push_back(d);
    });

    const uint64_t streamId = 0x28;
    handle(r, MsgType::STREAM_START, startReq(streamId, 9, 3, kTagA));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 0, 0, bytes("ABC")));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 1, 3, bytes("DEF")));
    handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 2, 6, bytes("GHI")));

    CHECK(delivered.size() == 1, "untyped constructor still delivers exactly once");
    CHECK(seenId == streamId, "untyped constructor still reports the stream id");
    if (delivered.size() == 1) {
        CHECK(delivered[0] == bytes("ABCDEFGHI"),
              "untyped constructor still delivers the whole stream");
    }
}

} // namespace

int main() {
    TypedHandlerReceivesTagFromStreamStart();
    ZeroTagIsDeliveredVerbatim();
    TagIsPerStreamNotLastSeen();
    EmptyStreamCarriesTheTag();
    RestartReplacesTheTag();
    DroppedStreamDeliversNothing();
    UntypedConstructorIsUnchanged();

    if (g_failures == 0) {
        std::cout << "Test Passed" << std::endl;
        return 0;
    }
    std::cerr << g_failures << " check(s) failed" << std::endl;
    return 1;
}
