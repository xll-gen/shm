package shm

import (
	"encoding/binary"
	"fmt"
	"math"
	"unsafe"
)

// StreamSender allows the Guest to send large data streams to the Host.
type StreamSender struct {
	guest *DirectGuest
}

// NewStreamSender creates a new StreamSender for the given DirectGuest.
func NewStreamSender(g *DirectGuest) *StreamSender {
	return &StreamSender{
		guest: g,
	}
}

// NewStreamSenderFromClient creates a new StreamSender using the Client's underlying DirectGuest.
func NewStreamSenderFromClient(c *Client) *StreamSender {
	return &StreamSender{
		guest: c.guest,
	}
}

// Send sends data to the Host as a stream.
// It splits the data into chunks and sends them sequentially using Guest Calls.
// This function blocks until the entire stream is sent or an error occurs.
func (s *StreamSender) Send(data []byte, streamID uint64) error {
	if s.guest == nil {
		return fmt.Errorf("guest is nil")
	}
	if len(data) == 0 {
		return nil
	}

	// Calculate Max Payload Size per chunk
	maxReq := int(s.guest.respOffset - s.guest.reqOffset)
	chunkHeaderSize := int(unsafe.Sizeof(ChunkHeader{}))

	if maxReq <= chunkHeaderSize {
		return fmt.Errorf("slot size too small for stream chunk header")
	}

	maxPayload := maxReq - chunkHeaderSize
	totalSize := uint64(len(data))
	totalChunks := uint32(math.Ceil(float64(totalSize) / float64(maxPayload)))

	// 1. Send Stream Start
	// StreamHeader: StreamID(8) + TotalSize(8) + TotalChunks(4) + Reserved(4) = 24 bytes
	headerBuf := make([]byte, 24)
	binary.LittleEndian.PutUint64(headerBuf[0:8], streamID)
	binary.LittleEndian.PutUint64(headerBuf[8:16], totalSize)
	binary.LittleEndian.PutUint32(headerBuf[16:20], totalChunks)
	// Reserved is 0

	_, err := s.guest.SendGuestCall(headerBuf, MsgTypeStreamStart)
	if err != nil {
		return fmt.Errorf("failed to send stream start: %v", err)
	}

	// 2. Send Chunks
	offset := 0
	for i := uint32(0); i < totalChunks; i++ {
		end := offset + maxPayload
		if end > len(data) {
			end = len(data)
		}
		chunkPayload := data[offset:end]
		payloadSize := uint32(len(chunkPayload))

		// ChunkHeader: StreamID(8) + ChunkIndex(4) + PayloadSize(4) + Reserved(4) + Padding(4) = 24 bytes?
		// Wait, ChunkHeader in Go is defined in stream.go
		// type ChunkHeader struct { ... Padding uint32 }
		// Manual serialization:

		msgBuf := make([]byte, chunkHeaderSize+int(payloadSize))

		binary.LittleEndian.PutUint64(msgBuf[0:8], streamID)
		binary.LittleEndian.PutUint32(msgBuf[8:12], i)
		binary.LittleEndian.PutUint32(msgBuf[12:16], payloadSize)
		// Reserved (16-20) is 0
		// Padding (20-24) is 0

		copy(msgBuf[chunkHeaderSize:], chunkPayload)

		_, err := s.guest.SendGuestCall(msgBuf, MsgTypeStreamChunk)
		if err != nil {
			return fmt.Errorf("failed to send stream chunk %d: %v", i, err)
		}

		offset += int(payloadSize)
	}

	return nil
}
