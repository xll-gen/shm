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

// StreamHandler is a function type for processing assembled streams.
type StreamHandler func(streamID uint64, data []byte)

// streamContext holds the state of a stream being reassembled.
type streamContext struct {
	totalSize   uint64
	totalChunks uint32
	chunks      [][]byte
	received    uint32
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

	return func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
		if msgType == MsgTypeStreamStart {
			if len(req) < int(unsafe.Sizeof(StreamHeader{})) {
				return 0, MsgTypeSystemError
			}
			var header StreamHeader
            buf := bytes.NewReader(req)
            binary.Read(buf, binary.LittleEndian, &header)

			mu.Lock()
			streams[header.StreamID] = &streamContext{
				totalSize:   header.TotalSize,
				totalChunks: header.TotalChunks,
				chunks:      make([][]byte, header.TotalChunks),
				received:    0,
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
            if len(req) < headerLen + int(payloadSize) {
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

            if ctx.chunks[chunkIndex] == nil {
                ctx.chunks[chunkIndex] = chunkData
                ctx.received++
            }

            ready := (ctx.received == ctx.totalChunks)
            var fullData []byte
            if ready {
                // Reassemble
                fullData = make([]byte, ctx.totalSize)
                offset := 0
                for _, c := range ctx.chunks {
                    copy(fullData[offset:], c)
                    offset += len(c)
                }
                delete(streams, streamID)
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
