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
	Reserved    uint32
}

// ChunkHeader represents the metadata for a Stream Chunk message.
type ChunkHeader struct {
	StreamID    uint64
	ChunkIndex  uint32
	PayloadSize uint32
	Reserved    uint32
	Padding     uint32 // Padding to match C++ alignment (24 bytes)
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
	buf         []byte // preallocated destination, len == totalSize
	next        uint32 // next in-order chunk index to consume into buf
	offset      uint64 // bytes written into buf so far (running Σ payloadSize)
	// ooo holds copies of chunks received ahead of next, keyed by chunk
	// index. Presence in the map (not slice nil-ness) is the dedup marker so
	// zero-length chunks are counted exactly once. nil until first needed.
	ooo map[uint32][]byte
	// oooBytes is Σ len(ooo[i]) — the bytes currently parked ahead of the
	// cursor. offset+oooBytes is the running *received* length of the stream,
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

// consume appends data at the in-order cursor, enforcing the per-chunk
// running-length guard. Returns false if the guard trips; the caller must then
// drop the stream and answer MSG_TYPE_SYSTEM_ERROR. Draining a parked chunk
// releases its oooBytes first (see the drain loop), so a chunk that fit when it
// was parked still fits when it is consumed. Must be called under c.mu.
func (c *streamContext) consume(data []byte) bool {
	if !c.fits(len(data)) {
		return false
	}
	copy(c.buf[c.offset:], data)
	c.offset += uint64(len(data))
	c.next++
	return true
}

// park buffers a chunk that arrived ahead of the in-order cursor. The overflow
// guard runs BEFORE the copy is allocated, so an over-advertised stream is
// rejected at arrival time instead of after all of its bytes are resident.
// Returns false if the guard trips. Must be called under c.mu.
func (c *streamContext) park(index uint32, data []byte) bool {
	if !c.fits(len(data)) {
		return false
	}
	if c.ooo == nil {
		c.ooo = make(map[uint32][]byte)
	}
	parked := make([]byte, len(data))
	copy(parked, data)
	c.ooo[index] = parked
	c.oooBytes += uint64(len(data))
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

	return func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
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

			if totalChunks > MaxStreamChunks || totalSize > MaxStreamSize {
				return 0, MsgTypeSystemError
			}
			if totalChunks == 0 {
				// Empty stream: no chunks will follow, so complete it now
				// instead of leaving a context in the map forever.
				if totalSize != 0 {
					return 0, MsgTypeSystemError
				}
				onStream(streamID, nil)
				return 0, MsgTypeNormal // ACK
			}

			// Allocate the destination BEFORE taking the map mutex. TotalSize
			// reaches MaxStreamSize (1 GiB), and the runtime zeroes the whole
			// allocation; holding the reassembler-wide lock across that memclr
			// stalled every other stream's chunk traffic.
			ctx := &streamContext{
				totalSize:   totalSize,
				totalChunks: totalChunks,
				buf:         make([]byte, totalSize),
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
			if chunkIndex == ctx.next {
				// In-order fast path: copy straight from the slot buffer into
				// the preallocated destination, then drain any parked chunks
				// the cursor has caught up to.
				ok := ctx.consume(data)
				for ok {
					buffered, present := ctx.ooo[ctx.next]
					if !present {
						break
					}
					delete(ctx.ooo, ctx.next)
					// These bytes move from "parked" to "assembled"; releasing
					// the count first keeps offset+oooBytes invariant across the
					// drain, so a legitimately-sized stream cannot be rejected
					// for bytes it counted twice.
					ctx.oooBytes -= uint64(len(buffered))
					ok = ctx.consume(buffered)
				}
				if !ok {
					// Running length exceeded totalSize: the stream is corrupt.
					// Drop it rather than deliver truncated/garbled data.
					corrupt = true
				}
			} else if !ctx.park(chunkIndex, data) {
				// Ahead of the cursor (pipelined sender): the byte offset is
				// unknown until the gap fills, so park a copy — but only if the
				// running received length still fits totalSize. Rejected here
				// the parked bytes are never allocated at all.
				corrupt = true
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
				onStream(streamID, fullData)
			}

			return 0, MsgTypeNormal // ACK
		}

		if fallback != nil {
			return fallback(req, resp, msgType)
		}
		return 0, MsgTypeNormal
	}
}
