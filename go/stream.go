package shm

import (
	"bytes"
	"encoding/binary"
	"sync"
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
type streamContext struct {
	totalSize   uint64
	totalChunks uint32
	buf         []byte // preallocated destination, len == totalSize
	next        uint32 // next in-order chunk index to consume into buf
	offset      uint64 // bytes written into buf so far (running Σ payloadSize)
	// ooo holds copies of chunks received ahead of next, keyed by chunk
	// index. Presence in the map (not slice nil-ness) is the dedup marker so
	// zero-length chunks are counted exactly once. nil until first needed.
	ooo map[uint32][]byte
	// lastSeq is the closure-local activity counter value at the most recent
	// StreamStart/StreamChunk for this stream. It defines least-recently-active
	// order for eviction; it is read/written only under the reassembler mutex.
	lastSeq uint64
}

// consume appends data at the in-order cursor, enforcing the SPEC §3.3.4
// per-chunk running-length guard (assembled length must never exceed
// totalSize). Returns false if the guard trips; the caller must then drop the
// stream and answer MSG_TYPE_SYSTEM_ERROR. Must be called under the
// reassembler mutex.
func (c *streamContext) consume(data []byte) bool {
	if c.offset+uint64(len(data)) > c.totalSize {
		return false
	}
	copy(c.buf[c.offset:], data)
	c.offset += uint64(len(data))
	c.next++
	return true
}

// NewStreamReassembler creates a Handler that reassembles streams and calls the provided StreamHandler.
// It handles normal messages by passing them through (if a fallback handler is provided).
//
// onStream: Called when a stream is fully reassembled.
// fallback: Called for non-stream messages. Can be nil.
func NewStreamReassembler(onStream StreamHandler, fallback func(req []byte, resp []byte, msgType MsgType) (int32, MsgType)) func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
	// Map to store partial streams
	streams := make(map[uint64]*streamContext)
	var mu sync.Mutex
	// seq is a monotonic activity counter; the highest value marks the most
	// recently touched stream. Guarded by mu.
	var seq uint64

	return func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
		if msgType == MsgTypeStreamStart {
			if len(req) < int(unsafe.Sizeof(StreamHeader{})) {
				return 0, MsgTypeSystemError
			}
			var header StreamHeader
			if err := binary.Read(bytes.NewReader(req), binary.LittleEndian, &header); err != nil {
				return 0, MsgTypeSystemError
			}
			if header.TotalChunks > MaxStreamChunks || header.TotalSize > MaxStreamSize {
				return 0, MsgTypeSystemError
			}
			if header.TotalChunks == 0 {
				// Empty stream: no chunks will follow, so complete it now
				// instead of leaving a context in the map forever.
				if header.TotalSize != 0 {
					return 0, MsgTypeSystemError
				}
				onStream(header.StreamID, nil)
				return 0, MsgTypeNormal // ACK
			}

			mu.Lock()
			seq++
			// Bound the number of in-flight reassemblies. Adding a genuinely
			// new stream beyond the cap evicts the least-recently-active one,
			// so a peer that starts streams but never finishes them cannot grow
			// the map without limit. A repeated StreamStart for an existing ID
			// just overwrites and does not count as growth.
			if _, dup := streams[header.StreamID]; !dup && len(streams) >= MaxConcurrentStreams {
				oldestID := header.StreamID
				oldestSeq := ^uint64(0)
				for id, c := range streams {
					if c.lastSeq < oldestSeq {
						oldestSeq = c.lastSeq
						oldestID = id
					}
				}
				delete(streams, oldestID)
			}
			streams[header.StreamID] = &streamContext{
				totalSize:   header.TotalSize,
				totalChunks: header.TotalChunks,
				buf:         make([]byte, header.TotalSize),
				lastSeq:     seq,
			}
			mu.Unlock()
			return 0, MsgTypeNormal // ACK
		}

		if msgType == MsgTypeStreamChunk {
			if len(req) < int(unsafe.Sizeof(ChunkHeader{})) {
				return 0, MsgTypeSystemError
			}

			streamID := binary.LittleEndian.Uint64(req[0:8])
			chunkIndex := binary.LittleEndian.Uint32(req[8:12])
			payloadSize := binary.LittleEndian.Uint32(req[12:16])

			headerLen := int(unsafe.Sizeof(ChunkHeader{}))
			if len(req) < headerLen+int(payloadSize) {
				return 0, MsgTypeSystemError
			}

			data := req[headerLen : headerLen+int(payloadSize)]

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

			// Duplicate delivery: already consumed into buf, or already parked
			// in the out-of-order buffer. ACK without consuming (each index is
			// filled exactly once, per SPEC §3.3.4).
			if chunkIndex < ctx.next {
				mu.Unlock()
				return 0, MsgTypeNormal
			}
			if _, dup := ctx.ooo[chunkIndex]; dup {
				mu.Unlock()
				return 0, MsgTypeNormal
			}

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
					ok = ctx.consume(buffered)
				}
				if !ok {
					// Running length exceeded totalSize: the stream is corrupt.
					// Drop it rather than deliver truncated/garbled data.
					delete(streams, streamID)
					mu.Unlock()
					return 0, MsgTypeSystemError
				}
			} else {
				// Ahead of the cursor (pipelined sender): the byte offset is
				// unknown until the gap fills, so park a copy. The slot buffer
				// is reused after this handler returns, hence the copy.
				if ctx.ooo == nil {
					ctx.ooo = make(map[uint32][]byte)
				}
				parked := make([]byte, len(data))
				copy(parked, data)
				ctx.ooo[chunkIndex] = parked
			}

			ready := ctx.next == ctx.totalChunks
			var fullData []byte
			if ready {
				// Every chunk index consumed exactly once. The byte-sum must
				// match the advertised TotalSize exactly; otherwise the stream
				// is corrupt and is dropped rather than delivered truncated.
				delete(streams, streamID)
				if ctx.offset != ctx.totalSize {
					mu.Unlock()
					return 0, MsgTypeSystemError
				}
				fullData = ctx.buf
			}
			mu.Unlock()

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
