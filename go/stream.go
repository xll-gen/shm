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
type streamContext struct {
	totalSize   uint64
	totalChunks uint32
	chunks      [][]byte
	received    uint32
	// lastSeq is the closure-local activity counter value at the most recent
	// StreamStart/StreamChunk for this stream. It defines least-recently-active
	// order for eviction; it is read/written only under the reassembler mutex.
	lastSeq uint64
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
				chunks:      make([][]byte, header.TotalChunks),
				received:    0,
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

			// Copy data because the slot will be reused
			chunkData := make([]byte, len(data))
			copy(chunkData, data)

			mu.Lock()
			ctx, exists := streams[streamID]
			if !exists {
				mu.Unlock()
				return 0, MsgTypeSystemError
			}

			if chunkIndex >= uint32(len(ctx.chunks)) {
				mu.Unlock()
				return 0, MsgTypeSystemError
			}

			// Mark activity so a stream actively receiving chunks is not the
			// eviction target when other streams open under memory pressure.
			seq++
			ctx.lastSeq = seq

			if ctx.chunks[chunkIndex] == nil {
				ctx.chunks[chunkIndex] = chunkData
				ctx.received++
			}

			ready := (ctx.received == ctx.totalChunks)
			var fullData []byte
			if ready {
				// Reassemble. The chunk byte-sum must match the advertised
				// TotalSize exactly; otherwise the stream is corrupt and is
				// dropped rather than delivered truncated/garbled.
				delete(streams, streamID)
				fullData = make([]byte, ctx.totalSize)
				offset := 0
				for _, c := range ctx.chunks {
					if offset+len(c) > len(fullData) {
						mu.Unlock()
						return 0, MsgTypeSystemError
					}
					copy(fullData[offset:], c)
					offset += len(c)
				}
				if offset != len(fullData) {
					mu.Unlock()
					return 0, MsgTypeSystemError
				}
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
