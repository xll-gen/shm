// The ONLY C++ guard on ABSOLUTE PLACEMENT — the headline property of
// SHM_VERSION 0x00080000. C++ twin of go/stream_dstoffset_placement_test.go.
//
// WHY NOTHING ELSE IN ctest CAN SEE THIS. The receiver requires
// ChunkHeader::dstOffset to equal the running in-order cursor at the moment an
// index becomes in-order, and drops the stream otherwise (SPEC §3.3.4 placement
// check 2). So for every stream that is NOT dropped, "write at dstOffset" and
// "append at my own cursor" put every byte at the same address, produce
// byte-identical delivered output, identical accept/reject replies and identical
// parity digests. That is a theorem about the invariant, not a property of a
// particular arrival order — reversing the order does not help, because the old
// parking receiver drained in INDEX order too. (It is also why all 20 legacy
// parity digests survived the 0x00080000 rework unchanged.) Before this file
// existed, a C++ regression to "park a payload copy, drain in index order, append
// at the cursor" passed all 45 ctest tests.
//
// WHAT THE EXISTING static_asserts DO NOT COVER. StreamContext::Placed is pinned
// to 16 bytes and trivially copyable, which excludes an owning parked payload. It
// says nothing about WHERE a chunk's bytes are written. A receiver that placed
// every chunk at `ctx.offset` instead of at `header->dstOffset` keeps Placed
// untouched, keeps every digest identical, and corrupts every out-of-order stream
// that the cursor check happens not to reject first. That is the residual risk
// this file aims at.
//
// THE ONE OBSERVABLE THAT DIFFERS is the destination buffer's content BEFORE the
// cursor reaches it. `buf` is handed to the callback only at completion, so the
// observation is taken mid-stream through StreamReassembler::PeekStream — the
// counterpart of Go's newStreamReassemblerPeek seam.

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

// dstOffset is REQUIRED, not defaulted: 0 is a legal offset for chunk 0, so a
// call site that forgot it would silently claim "offset 0".
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

std::string quote(const uint8_t* p, size_t n) {
    std::string out = "\"";
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = p[i];
        if (c >= 0x20 && c < 0x7F) {
            out += static_cast<char>(c);
        } else {
            static const char hex[] = "0123456789ABCDEF";
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    out += "\"";
    return out;
}

// Delivers a chunk that is AHEAD of the in-order cursor and then, while the
// stream is still incomplete, inspects the destination buffer.
//
// Post-0x00080000 the payload must ALREADY be at buf[dstOffset], and the region
// the cursor has not reached must still be untouched. A parking receiver fails the
// first assertion (zeros at dstOffset); a receiver that wrote at its own cursor
// instead of at dstOffset fails the second (payload at buf[0]).
void aheadOfCursorChunkIsResidentAtDstOffset() {
    const uint64_t streamId = 0x0801;
    // Deliberately NON-UNIFORM sizes so dstOffset is not idx*size for any idx:
    // chunk 0 is 1 byte at 0, chunk 1 is 2 bytes at 1, chunk 2 is 3 bytes at 3. A
    // receiver that reconstructed the offset from the index instead of reading the
    // field cannot land chunk 2 at 3.
    const uint64_t totalSize = 6;
    const std::vector<uint8_t> chunk2 = bytes("CCC");

    std::vector<std::vector<uint8_t>> delivered;
    StreamReassembler r([&](uint64_t, const std::vector<uint8_t>& data) {
        delivered.push_back(data);
    });

    CHECK(handle(r, MsgType::STREAM_START, startReq(streamId, totalSize, 3)) ==
              MsgType::NORMAL,
          "valid STREAM_START must ACK");

    // Chunk 2 only. The cursor is still at index 0 / offset 0, so this chunk is
    // ahead of the cursor and the stream cannot complete.
    CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 2, 3, chunk2)) ==
              MsgType::NORMAL,
          "ahead-of-cursor chunk 2 must ACK");
    CHECK(delivered.empty(),
          "stream completed after one of three chunks — there is no mid-stream "
          "window left to observe");

    StreamReassembler::StreamSnapshot snap;
    if (!r.PeekStream(streamId, snap)) {
        std::cerr << "FAIL: stream " << streamId
                  << " is not in flight — the observation window is gone, so "
                     "this test would assert nothing"
                  << std::endl;
        ++g_failures;
        return;
    }

    CHECK(snap.next == 0 && snap.offset == 0,
          "cursor moved on an ahead-of-cursor chunk; the chunk was not actually "
          "ahead of the cursor and the test proves nothing");
    CHECK(snap.buf.size() == totalSize,
          "destination buffer is not sized by totalSize");
    if (snap.buf.size() != totalSize) return;

    // THE DISCRIMINATOR. dstOffset = 3, payload "CCC".
    if (std::memcmp(snap.buf.data() + 3, chunk2.data(), chunk2.size()) != 0) {
        std::cerr
            << "FAIL: destination buffer holds " << quote(snap.buf.data() + 3, 3)
            << " at dstOffset 3 while the stream is incomplete, want \"CCC\".\n"
               "       An ahead-of-cursor chunk is NOT being written to "
               "buf[dstOffset] on arrival — it is being PARKED as a payload copy, "
               "which is exactly the pre-SHM_VERSION-0x00080000 behavior the bump "
               "exists to replace. No output-byte or parity-digest test can catch "
               "this (see this file's header), so this assertion is the only guard."
            << std::endl;
        ++g_failures;
    }
    // And it went to the ABSOLUTE offset, not to the cursor.
    const uint8_t zeros[3] = {0, 0, 0};
    if (std::memcmp(snap.buf.data(), zeros, 3) != 0) {
        std::cerr << "FAIL: destination buffer holds "
                  << quote(snap.buf.data(), 3)
                  << " at offsets 0..2, want zeros.\n"
                     "       The ahead-of-cursor chunk was appended at the "
                     "in-order cursor instead of placed at its declared dstOffset."
                  << std::endl;
        ++g_failures;
    }

    // The record left behind is bookkeeping only: it names the destination and
    // the size, and its bytes are already counted as ahead-of-cursor residency.
    auto placed = snap.placed.find(2);
    if (placed == snap.placed.end()) {
        std::cerr << "FAIL: chunk 2 is not recorded ahead of the cursor — "
                     "nothing tracks the out-of-order index"
                  << std::endl;
        ++g_failures;
    } else {
        CHECK(placed->second.first == 3 && placed->second.second == 3,
              "parked record must be {dstOffset:3, size:3} — the recorded "
              "destination is what the drain compares against the cursor");
    }
    CHECK(snap.oooBytes == 3,
          "oooBytes must count the 3 bytes placed ahead of the cursor (the "
          "§3.3.4 running-length bound)");

    // Finish the stream so the test also proves the mid-stream state it observed
    // is a state a NORMAL stream passes through, not a stuck one.
    CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 0, 0, bytes("A"))) ==
              MsgType::NORMAL,
          "chunk 0 must ACK");
    CHECK(handle(r, MsgType::STREAM_CHUNK, chunkReq(streamId, 1, 1, bytes("BB"))) ==
              MsgType::NORMAL,
          "chunk 1 must ACK");
    if (delivered.size() != 1) {
        std::cerr << "FAIL: " << delivered.size()
                  << " deliveries, want exactly 1" << std::endl;
        ++g_failures;
    } else if (delivered[0] != bytes("ABBCCC")) {
        std::cerr << "FAIL: delivered "
                  << quote(delivered[0].data(), delivered[0].size())
                  << ", want \"ABBCCC\"" << std::endl;
        ++g_failures;
    }
}

// The many-chunk form of the same observation, and the one that would catch a
// receiver that placed *some* ahead-of-cursor chunks directly and parked others
// (e.g. a size threshold, or placement only for the first parked index).
//
// Every chunk except index 0 is delivered, in reverse order, so N-1 chunks are
// ahead of the cursor at once. The whole destination except chunk 0's slot must
// already be filled while the stream is still incomplete.
void parkedChunksAreAllResidentBeforeTheGapFills() {
    const uint64_t streamId = 0x0802;
    const uint32_t totalChunks = 16;
    const uint32_t chunkLen = 8;
    const uint64_t totalSize = static_cast<uint64_t>(totalChunks) * chunkLen;

    auto payload = [&](uint32_t i) {
        return std::vector<uint8_t>(chunkLen,
                                    static_cast<uint8_t>('a' + i));
    };

    int deliveries = 0;
    StreamReassembler r(
        [&](uint64_t, const std::vector<uint8_t>&) { ++deliveries; });

    CHECK(handle(r, MsgType::STREAM_START,
                 startReq(streamId, totalSize, totalChunks)) == MsgType::NORMAL,
          "valid STREAM_START must ACK");

    for (uint32_t i = totalChunks - 1; i >= 1; --i) {
        MsgType mt = handle(r, MsgType::STREAM_CHUNK,
                            chunkReq(streamId, i, i * chunkLen, payload(i)));
        CHECK(mt == MsgType::NORMAL,
              "ahead-of-cursor chunk " + std::to_string(i) + " must ACK");
    }
    CHECK(deliveries == 0, "stream completed with chunk 0 missing");

    StreamReassembler::StreamSnapshot snap;
    if (!r.PeekStream(streamId, snap)) {
        std::cerr << "FAIL: stream " << streamId
                  << " is not in flight — nothing to observe" << std::endl;
        ++g_failures;
        return;
    }

    CHECK(snap.placed.size() == totalChunks - 1,
          "wrong number of indices recorded ahead of the cursor: got " +
              std::to_string(snap.placed.size()) + ", want " +
              std::to_string(totalChunks - 1));
    CHECK(snap.buf.size() == totalSize, "destination buffer mis-sized");
    if (snap.buf.size() != totalSize) return;

    for (uint32_t i = 1; i < totalChunks; ++i) {
        const std::vector<uint8_t> want = payload(i);
        if (std::memcmp(snap.buf.data() + i * chunkLen, want.data(), chunkLen) !=
            0) {
            std::cerr << "FAIL: chunk " << i
                      << " is not resident at its dstOffset " << i * chunkLen
                      << " while the stream is incomplete: buf holds "
                      << quote(snap.buf.data() + i * chunkLen, chunkLen)
                      << ", want " << quote(want.data(), chunkLen)
                      << ".\n       This ahead-of-cursor chunk was parked as a "
                         "payload copy (or placed at the cursor) instead of "
                         "written to buf[dstOffset] on arrival."
                      << std::endl;
            ++g_failures;
            break;
        }
    }
    // Chunk 0's slot is the only untouched region.
    const std::vector<uint8_t> zeros(chunkLen, 0);
    if (std::memcmp(snap.buf.data(), zeros.data(), chunkLen) != 0) {
        std::cerr << "FAIL: buf[0.." << chunkLen << ") = "
                  << quote(snap.buf.data(), chunkLen)
                  << ", want zeros — the missing chunk's region must be untouched"
                  << std::endl;
        ++g_failures;
    }

    CHECK(handle(r, MsgType::STREAM_CHUNK,
                 chunkReq(streamId, 0, 0, payload(0))) == MsgType::NORMAL,
          "gap-filling chunk 0 must ACK");
    CHECK(deliveries == 1, "stream must complete exactly once once the gap fills");
}

// PeekStream is an observation seam and must stay one: a stream that has been
// delivered or dropped is gone, so a test that peeks too late cannot mistake a
// stale snapshot for a live one.
void peekReportsAbsenceRatherThanStaleState() {
    const uint64_t streamId = 0x0803;
    StreamReassembler r([](uint64_t, const std::vector<uint8_t>&) {});
    StreamReassembler::StreamSnapshot snap;

    CHECK(!r.PeekStream(streamId, snap), "unknown stream must not be reported");
    CHECK(handle(r, MsgType::STREAM_START, startReq(streamId, 2, 1)) ==
              MsgType::NORMAL,
          "valid STREAM_START must ACK");
    CHECK(r.PeekStream(streamId, snap), "in-flight stream must be observable");
    CHECK(handle(r, MsgType::STREAM_CHUNK,
                 chunkReq(streamId, 0, 0, bytes("hi"))) == MsgType::NORMAL,
          "completing chunk must ACK");
    CHECK(!r.PeekStream(streamId, snap),
          "a completed stream must be gone from the map, not observable");
}

} // namespace

int main() {
    aheadOfCursorChunkIsResidentAtDstOffset();
    parkedChunksAreAllResidentBeforeTheGapFills();
    peekReportsAbsenceRatherThanStaleState();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "Test Passed (every chunk is placed at its absolute dstOffset "
                 "on arrival, ahead of the in-order cursor)"
              << std::endl;
    return 0;
}
