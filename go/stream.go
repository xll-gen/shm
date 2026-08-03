package shm

import (
	"encoding/binary"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"
)

// StreamHeader represents the metadata for a Stream Start message.
type StreamHeader struct {
	StreamID    uint64
	TotalSize   uint64
	TotalChunks uint32
	// AppMsgType is an application-defined routing tag, OPAQUE to shm
	// (0 = unspecified). Was Reserved before SHM_VERSION 0x00080000.
	AppMsgType uint32
}

// ChunkHeader represents the metadata for a Stream Chunk message.
type ChunkHeader struct {
	StreamID    uint64
	ChunkIndex  uint32
	PayloadSize uint32
	// DstOffset is the ABSOLUTE byte offset of this chunk's payload within the
	// reassembled stream. Was Reserved before SHM_VERSION 0x00080000; giving it
	// meaning is precisely why that bump exists, because a pre-0x00080000 sender
	// writes 0 here and 0 is a LEGAL offset for chunk 0 — so a new reader against
	// an old sender would stack every chunk at offset 0 with nothing to detect it.
	DstOffset uint32
	Padding   uint32 // Padding to match C++ alignment (24 bytes)
}

// Compile-time size assertions for streaming protocol headers.
// StreamHeader must be exactly 24 bytes per SPECIFICATION.md §3.3.1.
// ChunkHeader is currently 24 bytes in Go; see the C++ side for the matching
// definition. Any size drift triggers a build failure here.
var (
	_ [24 - unsafe.Sizeof(StreamHeader{})]byte
	_ [unsafe.Sizeof(StreamHeader{}) - 24]byte
	_ [24 - unsafe.Sizeof(ChunkHeader{})]byte
	_ [unsafe.Sizeof(ChunkHeader{}) - 24]byte
)

// Per-field offset guards (AGENTS.md R25 idiom, same dual zero-array shape as
// direct.go's SlotHeader/ExchangeHeader guards). The size asserts above cannot
// see a size-preserving field reorder, and the two 24-byte stream headers are
// full of same-width fields: swapping ChunkHeader.DstOffset with
// ChunkHeader.Padding, or StreamHeader.TotalChunks with StreamHeader.AppMsgType,
// still compiles at 24 bytes. That would be a SILENT wire break, not a build
// error — DstOffset is read by the peer, and the three test files that build
// chunk bytes via binary.Write on this struct would move with the bug, so the
// suite could not catch it either.
//
// These mirror the C++ offsetof static_asserts in include/shm/Stream.h
// (§3.3.1/§3.3.2 offset columns in SPECIFICATION.md); change all three together.
var (
	_ [0 - unsafe.Offsetof(StreamHeader{}.StreamID)]byte
	_ [unsafe.Offsetof(StreamHeader{}.StreamID) - 0]byte
	_ [8 - unsafe.Offsetof(StreamHeader{}.TotalSize)]byte
	_ [unsafe.Offsetof(StreamHeader{}.TotalSize) - 8]byte
	_ [16 - unsafe.Offsetof(StreamHeader{}.TotalChunks)]byte
	_ [unsafe.Offsetof(StreamHeader{}.TotalChunks) - 16]byte
	_ [20 - unsafe.Offsetof(StreamHeader{}.AppMsgType)]byte
	_ [unsafe.Offsetof(StreamHeader{}.AppMsgType) - 20]byte

	_ [0 - unsafe.Offsetof(ChunkHeader{}.StreamID)]byte
	_ [unsafe.Offsetof(ChunkHeader{}.StreamID) - 0]byte
	_ [8 - unsafe.Offsetof(ChunkHeader{}.ChunkIndex)]byte
	_ [unsafe.Offsetof(ChunkHeader{}.ChunkIndex) - 8]byte
	_ [12 - unsafe.Offsetof(ChunkHeader{}.PayloadSize)]byte
	_ [unsafe.Offsetof(ChunkHeader{}.PayloadSize) - 12]byte
	_ [16 - unsafe.Offsetof(ChunkHeader{}.DstOffset)]byte
	_ [unsafe.Offsetof(ChunkHeader{}.DstOffset) - 16]byte
	_ [20 - unsafe.Offsetof(ChunkHeader{}.Padding)]byte
	_ [unsafe.Offsetof(ChunkHeader{}.Padding) - 20]byte
)

// Wire sizes of the streaming headers, as untyped constants so the parsers
// below index the request buffer without a per-call unsafe.Sizeof conversion.
// The dual zero-array asserts above pin both to 24 bytes at compile time.
const (
	streamHeaderSize = int(unsafe.Sizeof(StreamHeader{}))
	chunkHeaderSize  = int(unsafe.Sizeof(ChunkHeader{}))
)

// Reassembly bounds for peer-supplied stream headers. A corrupt or
// malicious StreamHeader must not drive huge allocations (TotalChunks is
// used to size a slice of slices; TotalSize sizes the final buffer).
const (
	// MaxStreamChunks bounds StreamHeader.TotalChunks.
	MaxStreamChunks = 1 << 20
	// MaxStreamSize bounds StreamHeader.TotalSize (1 GiB).
	MaxStreamSize = 1 << 30
	// MaxConcurrentStreams bounds the number of partially-reassembled streams
	// held in memory at once. A peer that opens streams (StreamStart) but never
	// delivers all chunks — because it crashed, was killed, or is malicious —
	// would otherwise accumulate streamContexts (each holding up to TotalSize
	// bytes) forever. When a new StreamStart would exceed this bound, the
	// least-recently-active in-flight stream is evicted.
	MaxConcurrentStreams = 1024
	// MaxParkedChunks bounds how many chunks one stream may hold parked ahead of
	// its in-order cursor (streamContext.ooo).
	//
	// The SPEC §3.3.4 running-byte guard (fits/oooBytes) bounds the *payload* a
	// stream can make resident by its advertised TotalSize. It cannot bound
	// per-chunk bookkeeping, for two independent reasons: a parked entry costs
	// memory at zero payload, and a zero-length chunk passes the byte guard by
	// construction (offset+oooBytes+0 <= TotalSize holds for any live stream).
	// Without a count bound, a peer advertises TotalSize = 1 with
	// TotalChunks = MaxStreamChunks and parks 2^20-1 zero-length chunks —
	// measured at 80.1 B/entry, i.e. 80 MiB of live heap for a stream that
	// advertised one byte, times MaxConcurrentStreams.
	//
	// The value is derived as MaxStreamChunks / MaxConcurrentStreams, the bound
	// that makes the *aggregate* parked-entry count across all in-flight streams
	// no larger than the chunk count of a single maximal stream: 2^20 / 2^10 =
	// 1024 entries per stream (~80 KiB) and ~80 MiB across 1024 streams. It
	// leaves two orders of magnitude of headroom over any legitimate sender —
	// StreamSender parks at most maxInFlight-1 chunks, and the library default
	// in-flight depth is 1.
	//
	// The C++ reassembler carries the same bound with the same default
	// (include/shm/StreamReassembler.h: StreamReassemblerConfig::
	// maxParkedChunks). It is part of the §3.3.4 accept/reject parity contract,
	// not a local tuning knob: changing it on one side alone makes a stream that
	// one peer accepts get rejected by the other.
	MaxParkedChunks = MaxStreamChunks / MaxConcurrentStreams
)

// DefaultStreamTimeout bounds how long a partially-reassembled stream may sit
// in memory before it is pruned regardless of the concurrent-stream count.
// It matches the C++ StreamReassembler default (StreamReassemblerConfig::
// streamTimeoutMs = 10000; see include/shm/StreamReassembler.h) so both peers
// reclaim abandoned stream memory on the same timescale. The count-LRU bound
// (MaxConcurrentStreams) only reclaims once 1024 streams pile up; the timeout
// reclaims a lone stalled stream — and the ooo bytes it parks — on its own.
// SPEC §3.3.4 leaves the reclaim mechanism implementation-defined, so adding
// time-prune alongside LRU stays within the wire contract.
const DefaultStreamTimeout = 10 * time.Second

// StreamHandler is a function type for processing assembled streams.
type StreamHandler func(streamID uint64, data []byte)

// TypedStreamHandler is StreamHandler plus the routing tag the stream's own
// STREAM_START carried in StreamHeader.AppMsgType (SPEC §3.3.1). shm neither
// interprets nor validates the tag: 0 arrives as 0, every other value verbatim.
//
// appMsgType is the tag captured when THIS stream's context was created, not the
// most recently observed one — with several streams in flight each completion
// reports its own.
type TypedStreamHandler func(streamID uint64, appMsgType uint32, data []byte)

// streamContext holds the state of a stream being reassembled.
//
// 2026-07-03 direct-into-destination: the final buffer is preallocated at
// StreamStart and in-order chunks are copied straight into it at a running
// offset — the previous layout (per-chunk allocation into a [][]byte plus a
// second full-size copy at completion) copied every stream byte twice and
// allocated once per chunk. Chunks that arrive ahead of the in-order cursor
// (possible when the sender pipelines chunks across multiple slots) are
// buffered in a lazily-allocated side map and drained as the cursor reaches
// them; with the default in-flight depth of 1 the map is never allocated.
//
// 2026-07-25 per-stream locking: the reassembly state below is guarded by this
// context's own mutex, not by the reassembler-wide map mutex. Chunk payload
// copies (up to a whole slot buffer each) therefore run fully concurrently
// across streams; the shared mutex only serializes map lookup/insert/delete
// and the LRU activity counter. See newStreamReassembler for the lock
// discipline that keeps the SPEC §3.3.4 invariants intact.
type streamContext struct {
	// mu guards buf, next, offset, ooo and done — everything touched while
	// consuming a chunk. Never held while the reassembler's map mutex is held
	// (and vice versa), so the two locks cannot invert.
	mu sync.Mutex

	totalSize   uint64 // immutable after construction
	totalChunks uint32 // immutable after construction
	// appMsgType is the SPEC §3.3.1 routing tag from the STREAM_START that
	// created THIS context. Immutable after construction, which is what makes
	// the per-stream delivery obligation hold under interleaved streams: a
	// reassembler-wide "last tag seen" would report the wrong stream's tag.
	// Opaque to every guard below — nothing in reassembly reads it.
	appMsgType uint32
	buf        []byte // preallocated destination, len == totalSize
	next       uint32 // next in-order chunk index to consume into buf
	offset     uint64 // bytes written into buf so far (running Σ payloadSize)
	// ooo records chunks placed AHEAD of next, keyed by chunk index. Presence in
	// the map is the dedup marker, so zero-length chunks are counted exactly
	// once. nil until first needed.
	//
	// Since SHM_VERSION 0x00080000 it holds NO PAYLOAD — every chunk is written
	// straight to buf[dstOffset] on arrival, so an out-of-order chunk costs a
	// 12-byte record instead of a copy of itself, and the drain copies nothing.
	//
	// The map REMAINS, deliberately: with offset placement and NO dedup, two
	// copies of chunk 0 (len 5) of a totalSize=10 / totalChunks=2 stream give
	// receivedChunks=2 and receivedBytes==totalSize, so the stream COMPLETES with
	// its second half never written. Dedup is what makes "count the chunks and
	// count the bytes" a sound completion test.
	ooo map[uint32]placedChunk
	// oooBytes is Σ ooo[i].size — the bytes ALREADY PLACED in buf ahead of the
	// cursor but not yet accounted in order (the map values carry no payload and
	// therefore no length of their own).
	// offset+oooBytes is the running *received* length of the stream,
	// which is what the SPEC §3.3.4 per-chunk overflow guard bounds by
	// totalSize; without it, a peer that only ever sends ahead-of-cursor
	// indices parks unbounded memory and is caught only once the gap fills.
	// Decremented exactly when a parked chunk is drained into buf.
	oooBytes uint64
	// done is set when this context is retired from within the chunk path:
	// either the stream completed (buf handed to onStream) or it was dropped as
	// corrupt. It makes the "onStream fires exactly once" and "no bytes are
	// consumed after a drop" invariants local to the context, instead of
	// resting on the map entry having already been removed — with per-stream
	// locking, retiring the map entry necessarily happens after the context
	// lock is released.
	done bool

	// dead is set by the map-side reclaim paths — the age prune, the
	// count-LRU eviction, and a StreamStart that replaces the same stream ID —
	// at the moment the context leaves the map. It is atomic because those
	// paths run under the map mutex and must not acquire a context lock (that
	// would nest the two locks). A chunk handler that snapshotted this context
	// just before it was reclaimed observes the flag after taking mu and
	// rejects the chunk, so a reclaimed context can never absorb further bytes
	// or fire onStream. This preserves the observable behavior of the previous
	// single-mutex reassembler.
	dead atomic.Bool

	// lastSeq is the reassembler-local activity counter value at the most
	// recent StreamStart/StreamChunk for this stream. It defines
	// least-recently-active order for eviction; it is read/written only under
	// the reassembler map mutex.
	lastSeq uint64
	// started is the wall/monotonic time the stream's StreamStart was accepted.
	// It drives the C++-parity age-based timeout prune and, mirroring the C++
	// StreamContext::startTime, is set once at StreamStart and NOT refreshed on
	// chunk arrival — expiry is measured from stream start, not last activity.
	// Written once before the context is published into the map, then read only
	// under the reassembler map mutex.
	started time.Time
}

// fits reports whether accepting n more bytes keeps the running received
// length (assembled + parked) within totalSize — the SPEC §3.3.4 per-chunk
// overflow guard. Parked bytes count because every parked index is eventually
// drained into buf, so they are already part of Σ payloadSize. Must be called
// under c.mu.
func (c *streamContext) fits(n int) bool {
	return c.offset+c.oooBytes+uint64(n) <= c.totalSize
}

// placedChunk records a chunk already written to its destination but not yet
// accounted in order. It carries no payload: the bytes are in buf already.
type placedChunk struct {
	dstOffset uint64
	size      uint32
}

// inBounds reports whether [dstOffset, dstOffset+n) lies inside buf. dstOffset is
// PEER-CONTROLLED, so this subtracts rather than adds — dstOffset+n could wrap.
func (c *streamContext) inBounds(dstOffset uint64, n int) bool {
	if dstOffset > c.totalSize {
		return false
	}
	return uint64(n) <= c.totalSize-dstOffset
}

// write puts a chunk at its absolute destination. Bounds-checked by the caller.
func (c *streamContext) write(dstOffset uint64, data []byte) {
	if len(data) > 0 {
		copy(c.buf[dstOffset:], data)
	}
}

// accountInOrder advances the cursor over a chunk already written.
func (c *streamContext) accountInOrder(n int) {
	c.offset += uint64(n)
	c.next++
}

// notePlaced records an ahead-of-cursor chunk that has already been written to
// buf. Returns false if the count bound trips; the caller must then drop the
// stream. Must be called under c.mu.
//
// Count bound (SPEC §3.3.4), independent of fits(): a record costs memory at zero
// payload, and a zero-length chunk can never trip the byte guard. It is now the
// ONLY thing this bounds — the payload is no longer duplicated here — but that
// makes it no less necessary. See MaxParkedChunks.
func (c *streamContext) notePlaced(index uint32, dstOffset uint64, n int) bool {
	if len(c.ooo) >= MaxParkedChunks {
		return false
	}
	if c.ooo == nil {
		c.ooo = make(map[uint32]placedChunk)
	}
	c.ooo[index] = placedChunk{dstOffset: dstOffset, size: uint32(n)}
	c.oooBytes += uint64(n)
	return true
}

// NewStreamReassembler creates a Handler that reassembles streams and calls the provided StreamHandler.
// It handles normal messages by passing them through (if a fallback handler is provided).
//
// onStream: Called when a stream is fully reassembled.
// fallback: Called for non-stream messages. Can be nil.
func NewStreamReassembler(onStream StreamHandler, fallback func(req []byte, resp []byte, msgType MsgType) (int32, MsgType)) func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
	return newStreamReassembler(onStream, fallback, DefaultStreamTimeout, time.Now)
}

// NewStreamReassemblerTyped is NewStreamReassembler with a completion callback
// that also receives StreamHeader.AppMsgType (SPEC §3.3.1 "Receive-side
// delivery"). It is the PARALLEL entry point required there: NewStreamReassembler
// keeps its signature and its behavior, and is defined as this one with the tag
// discarded, so the two cannot diverge in reassembly behavior.
//
// The tag reaches onStream on both completion paths — the empty stream
// (TotalChunks == 0, completed inside STREAM_START) and ordinary chunk
// completion. A dropped or evicted stream delivers nothing at all.
//
// This costs no version bump: StreamHeader.AppMsgType@20 has been on the wire
// since SHM_VERSION 0x00080000, written by both senders and read by nobody. Only
// local API surface changes here.
func NewStreamReassemblerTyped(onStream TypedStreamHandler, fallback func(req []byte, resp []byte, msgType MsgType) (int32, MsgType)) func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
	handler, _ := newStreamReassemblerTypedPeek(onStream, fallback, DefaultStreamTimeout, time.Now, nil)
	return handler
}

// newStreamReassembler is the clock- and timeout-injectable core of
// NewStreamReassembler. timeout <= 0 disables the age-based prune (LRU only).
// now supplies the current time; production passes time.Now, tests pass a
// controllable clock so the timeout can be driven deterministically without
// real sleeps. now() is expected to be monotonic within a process — time.Time
// subtraction uses Go's monotonic reading, so an NTP wall-clock step does not
// perturb elapsed measurement.
//
// Lock discipline (2026-07-25). Two levels:
//
//  1. mu — the map mutex — guards the streams map and the seq activity
//     counter. Critical sections are O(1) map operations (the count-LRU scan is
//     the one exception, and it only runs at the MaxConcurrentStreams bound).
//     No allocation of stream-sized memory and no payload copy happens under it.
//  2. streamContext.mu — one per in-flight stream — guards that stream's
//     reassembly state.
//
// The two are NEVER held simultaneously: every path takes mu, snapshots or
// publishes the map entry, releases mu, and only then works under the context
// lock; retiring a completed/dropped entry re-takes mu afterwards. Lock-order
// inversion is therefore impossible by construction.
//
// Because retirement is no longer atomic with completion, two invariants are
// carried explicitly instead of implicitly:
//
//   - streamContext.done / streamContext.dead make "delivered exactly once" and
//     "a reclaimed stream absorbs nothing further" properties of the context
//     rather than of the map.
//   - the map entry is removed with a compare-and-delete (delete only if the
//     entry still points at this context), so a chunk finishing on a context
//     that a concurrent StreamStart already replaced cannot evict the fresh
//     one.
func newStreamReassembler(onStream StreamHandler, fallback func(req []byte, resp []byte, msgType MsgType) (int32, MsgType), timeout time.Duration, now func() time.Time) func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
	handler, _ := newStreamReassemblerPeek(onStream, fallback, timeout, now)
	return handler
}

// newStreamReassemblerPeek is newStreamReassembler plus peek, which returns the
// live streamContext for a stream ID (nil if there is none).
//
// It exists for ONE property that is unobservable from outside: whether an
// ahead-of-cursor chunk's payload is already sitting at buf[dstOffset] while the
// stream is still INCOMPLETE. Every other observable — delivered bytes, digests,
// accept/reject — is identical between a dstOffset-honouring receiver and a
// cursor-appending one, because the in-order invariant forces dstOffset to equal
// the cursor for every index at the moment it becomes in-order. buf is handed out
// only at completion, by which point both shapes have filled it, so the
// distinguishing observation has to be taken mid-stream. See
// go/stream_dstoffset_placement_test.go.
//
// peek takes the map mutex, so it is safe to call concurrently with the handler,
// but the *contents* of the returned context must be read under ctx.mu. It is a
// test seam in the same sense newStreamReassembler itself is (clock injection):
// production goes through NewStreamReassembler and never sees it.
func newStreamReassemblerPeek(onStream StreamHandler, fallback func(req []byte, resp []byte, msgType MsgType) (int32, MsgType), timeout time.Duration, now func() time.Time) (handler func(req []byte, resp []byte, msgType MsgType) (int32, MsgType), peek func(streamID uint64) *streamContext) {
	return newStreamReassemblerTypedPeek(func(streamID uint64, _ uint32, data []byte) {
		onStream(streamID, data)
	}, fallback, timeout, now, nil)
}

// newStreamReassemblerTypedPeek is the single implementation; every other
// constructor in this file adapts onto it. The untyped forms pass a shim that
// drops appMsgType, so "the untyped entry point behaves exactly like the typed
// one minus the tag" is true by construction rather than by two code paths kept
// in sync (SPEC §3.3.1 obligation 1).
//
// newBuf is a BENCHMARK-ONLY seam, not a buffer supplier feature. EVERY public
// constructor passes nil, so production allocates the destination with
// make([]byte, totalSize) exactly as it always has. It exists so the cost of
// that zero-fill can be measured against a receive path that is otherwise
// byte-identical (go/stream_zerofill_bench_test.go); the backlog item proposing
// an opt-in supplier had been picked up and abandoned twice for want of that
// number. The measurement was taken on 2026-08-03 and the opt-in was DECLINED —
// see EXPERIMENTS.md "2026-08-03 stream destination zero-fill". Do not export
// this: a public supplier hands the caller a lifetime obligation shm cannot
// enforce IN GO — StreamHandler passes the destination out as a []byte, and
// retaining that slice past the callback is legal, undetectable Go, so a
// recycled buffer silently corrupts data the previous receiver still holds. The
// C++ reassembler has no equivalent exposure (its callback takes a
// const std::vector<uint8_t>& to a vector destroyed right after the call), so do
// not carry this reason across; what the C++ side does owe is a matching hook
// per AGENTS.md feature parity, which is ADDITIVE there — StreamReassembler.h
// moves ctx.buf into a local it still owns across onStream, so an acquire/
// release pair needs no signature change. That is a cost in duplicated work and
// tests, not a breaking API change.
//
// A seam implementation MUST return a slice with len == n; the caller installs
// it as ctx.buf verbatim and every bounds guard in reassembly is expressed
// against it.
func newStreamReassemblerTypedPeek(onStream TypedStreamHandler, fallback func(req []byte, resp []byte, msgType MsgType) (int32, MsgType), timeout time.Duration, now func() time.Time, newBuf func(int) []byte) (handler func(req []byte, resp []byte, msgType MsgType) (int32, MsgType), peek func(streamID uint64) *streamContext) {
	if newBuf == nil {
		newBuf = func(n int) []byte { return make([]byte, n) }
	}
	// Map to store partial streams
	streams := make(map[uint64]*streamContext)
	var mu sync.Mutex
	// seq is a monotonic activity counter; the highest value marks the most
	// recently touched stream. Guarded by mu.
	var seq uint64

	// retire removes ctx's map entry once ctx has been delivered or dropped.
	// The identity check matters: a StreamStart for the same ID may already
	// have installed a fresh context while this chunk was being consumed, and
	// that replacement must survive.
	retire := func(streamID uint64, ctx *streamContext) {
		mu.Lock()
		if cur, ok := streams[streamID]; ok && cur == ctx {
			delete(streams, streamID)
		}
		mu.Unlock()
	}

	peek = func(streamID uint64) *streamContext {
		mu.Lock()
		defer mu.Unlock()
		return streams[streamID]
	}

	handler = func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
		if msgType == MsgTypeStreamStart {
			if len(req) < streamHeaderSize {
				return 0, MsgTypeSystemError
			}
			// Manual little-endian decode. Equivalent to the previous
			// reflection-based binary.Read (same wire layout per SPEC §3.3.1)
			// without the bytes.Reader allocation and reflect walk, and
			// identical in shape to the chunk parser below. The length guard
			// above plus the compile-time size assert pin every index.
			streamID := binary.LittleEndian.Uint64(req[0:8])
			totalSize := binary.LittleEndian.Uint64(req[8:16])
			totalChunks := binary.LittleEndian.Uint32(req[16:20])
			// SPEC §3.3.1 routing tag. Parsed here and NEVER consulted below —
			// it is carried alongside the reassembled bytes, not used while
			// producing them, which is what keeps the inertness MUST true.
			appMsgType := binary.LittleEndian.Uint32(req[20:24])

			if totalChunks > MaxStreamChunks || totalSize > MaxStreamSize {
				return 0, MsgTypeSystemError
			}
			if totalChunks == 0 {
				// Empty stream: no chunks will follow, so complete it now
				// instead of leaving a context in the map forever.
				if totalSize != 0 {
					return 0, MsgTypeSystemError
				}
				onStream(streamID, appMsgType, nil)
				return 0, MsgTypeNormal // ACK
			}

			// Allocate the destination BEFORE taking the map mutex. TotalSize
			// reaches MaxStreamSize (1 GiB), and the runtime zeroes the whole
			// allocation; holding the reassembler-wide lock across that memclr
			// stalled every other stream's chunk traffic.
			//
			// The zero-fill itself is NOT wasted work that can simply be dropped,
			// even though delivery requires every byte to have been overwritten
			// (next == totalChunks AND offset == totalSize): Go has no allocation
			// that skips it, so removing it means not allocating at all — i.e.
			// recycling a buffer the application handed back. newBuf is the
			// benchmark seam for that; production is make([]byte, totalSize).
			ctx := &streamContext{
				totalSize:   totalSize,
				totalChunks: totalChunks,
				appMsgType:  appMsgType,
				buf:         newBuf(int(totalSize)),
			}

			mu.Lock()
			seq++
			tnow := now()

			// Age-based prune (C++ StreamReassembler::PruneInternal parity):
			// drop every in-flight stream older than the timeout, freeing its
			// preallocated buf and any parked ooo bytes. Unlike the count-LRU
			// bound below, this reclaims a lone stalled stream without waiting
			// for MaxConcurrentStreams distinct streams to pile up. Runs at
			// StreamStart — the point new memory is about to be committed.
			// A repeated StreamStart for the same ID resets the start time
			// via the fresh streamContext overwrite below (C++ does the
			// same: streams[id] is replaced with a fresh startTime).
			if timeout > 0 {
				for id, c := range streams {
					if tnow.Sub(c.started) > timeout {
						c.dead.Store(true)
						delete(streams, id)
					}
				}
			}

			// Bound the number of in-flight reassemblies. Adding a genuinely
			// new stream beyond the cap evicts the least-recently-active one,
			// so a peer that starts streams but never finishes them cannot grow
			// the map without limit. A repeated StreamStart for an existing ID
			// just overwrites and does not count as growth.
			prev, dup := streams[streamID]
			if !dup && len(streams) >= MaxConcurrentStreams {
				oldestID := streamID
				oldestSeq := ^uint64(0)
				var oldest *streamContext
				for id, c := range streams {
					if c.lastSeq < oldestSeq {
						oldestSeq = c.lastSeq
						oldestID = id
						oldest = c
					}
				}
				if oldest != nil {
					oldest.dead.Store(true)
				}
				delete(streams, oldestID)
			}
			if dup {
				// The replaced context may still be mid-chunk in another
				// worker; mark it so it stops consuming and never delivers.
				prev.dead.Store(true)
			}
			ctx.lastSeq = seq
			ctx.started = tnow
			streams[streamID] = ctx
			mu.Unlock()
			return 0, MsgTypeNormal // ACK
		}

		if msgType == MsgTypeStreamChunk {
			if len(req) < chunkHeaderSize {
				return 0, MsgTypeSystemError
			}

			streamID := binary.LittleEndian.Uint64(req[0:8])
			chunkIndex := binary.LittleEndian.Uint32(req[8:12])
			payloadSize := binary.LittleEndian.Uint32(req[12:16])
			// Absolute destination (SPEC §3.3.2, SHM_VERSION 0x00080000). Widened
			// to u64 for the bounds arithmetic below; the wire field is u32, which
			// is sufficient only because MaxStreamSize is 1 GiB.
			dstOffset := uint64(binary.LittleEndian.Uint32(req[16:20]))

			// Unsigned arithmetic: on 32-bit builds int(payloadSize) of a
			// hostile value >= 2^31 would go negative and slip past a signed
			// comparison, panicking at the slice expression below.
			if uint64(len(req)) < uint64(chunkHeaderSize)+uint64(payloadSize) {
				return 0, MsgTypeSystemError
			}

			data := req[chunkHeaderSize : chunkHeaderSize+int(payloadSize)]

			// Map-mutex section: locate the stream and stamp activity. Both
			// totalChunks and the identity of the context are stable once read,
			// so everything below runs outside this lock.
			mu.Lock()
			ctx, exists := streams[streamID]
			if !exists {
				mu.Unlock()
				return 0, MsgTypeSystemError
			}
			if chunkIndex >= ctx.totalChunks {
				mu.Unlock()
				return 0, MsgTypeSystemError
			}
			// Mark activity so a stream actively receiving chunks is not the
			// eviction target when other streams open under memory pressure.
			seq++
			ctx.lastSeq = seq
			mu.Unlock()

			ctx.mu.Lock()
			// Retired between the map lookup and this lock: either delivered /
			// dropped by a concurrent chunk, or reclaimed by a prune, an LRU
			// eviction, or a StreamStart that replaced this ID. Same answer the
			// single-mutex reassembler gave when the map entry was already gone.
			if ctx.done || ctx.dead.Load() {
				ctx.mu.Unlock()
				return 0, MsgTypeSystemError
			}

			// Duplicate delivery: already consumed into buf, or already parked
			// in the out-of-order buffer. ACK without consuming (each index is
			// filled exactly once, per SPEC §3.3.4).
			if chunkIndex < ctx.next {
				ctx.mu.Unlock()
				return 0, MsgTypeNormal
			}
			if _, dup := ctx.ooo[chunkIndex]; dup {
				ctx.mu.Unlock()
				return 0, MsgTypeNormal
			}

			corrupt := false
			// Guards BEFORE the copy, in the order SPEC §3.3.4 fixes: the
			// running-length bound, then the destination bounds. dstOffset is
			// peer-controlled, so inBounds is what keeps the copy inside buf.
			if !ctx.fits(len(data)) || !ctx.inBounds(dstOffset, len(data)) {
				corrupt = true
			} else {
				// EVERY chunk goes straight to its final home, whatever the
				// arrival order. That is what dstOffset bought: an out-of-order
				// chunk is no longer copied into a parked slice and then copied
				// a second time when the gap fills.
				ctx.write(dstOffset, data)

				if chunkIndex == ctx.next {
					// In-order. A conforming sender puts exactly the running
					// cursor on chunk `next`, so this equality IS the
					// misplacement check — and it is exact rather than
					// heuristic: it runs for every index once that index becomes
					// in-order, so a sender that misplaces ANY chunk is caught
					// before the stream can complete. That is what lets this keep
					// index dedup instead of tracking covered intervals.
					if dstOffset != ctx.offset {
						corrupt = true
					} else {
						ctx.accountInOrder(len(data))
						// Drain the indices the cursor has caught up to. No
						// payload moves here any more — the bytes are already in
						// place; only the bookkeeping advances.
						for !corrupt {
							placed, present := ctx.ooo[ctx.next]
							if !present {
								break
							}
							delete(ctx.ooo, ctx.next)
							// Move these bytes from "placed" to "assembled"
							// before re-accounting, so offset+oooBytes stays
							// invariant and a legitimately-sized stream cannot be
							// rejected for bytes it counted twice.
							ctx.oooBytes -= uint64(placed.size)
							if placed.dstOffset != ctx.offset {
								corrupt = true // misplaced, caught on drain
								break
							}
							ctx.accountInOrder(int(placed.size))
						}
					}
				} else if !ctx.notePlaced(chunkIndex, dstOffset, len(data)) {
					// Ahead of the cursor (pipelined sender). The payload is
					// already written; only the 12-byte record is bounded.
					corrupt = true
				}
			}

			ready := false
			var fullData []byte
			if !corrupt && ctx.next == ctx.totalChunks {
				// Every chunk index consumed exactly once. The byte-sum must
				// match the advertised TotalSize exactly; otherwise the stream
				// is corrupt and is dropped rather than delivered truncated.
				if ctx.offset == ctx.totalSize {
					ready = true
					fullData = ctx.buf
				} else {
					corrupt = true
				}
			}
			if corrupt || ready {
				// No further chunk may touch this context: buf is about to be
				// handed to onStream (sole owner from here) or discarded.
				ctx.done = true
			}
			ctx.mu.Unlock()

			if corrupt || ready {
				retire(streamID, ctx)
			}
			if corrupt {
				return 0, MsgTypeSystemError
			}
			if ready {
				// ctx.appMsgType, not a reassembler-wide value: the tag belongs
				// to the STREAM_START that opened THIS context (SPEC §3.3.1
				// obligation 2). Immutable since construction, so reading it
				// after the context lock was dropped is safe.
				onStream(streamID, ctx.appMsgType, fullData)
			}

			return 0, MsgTypeNormal // ACK
		}

		if fallback != nil {
			return fallback(req, resp, msgType)
		}
		return 0, MsgTypeNormal
	}

	return handler, peek
}
