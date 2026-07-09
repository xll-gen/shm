package shm

import (
	"encoding/binary"
	"fmt"
	"sync"
	"time"
	"unsafe"
)

// StreamSender helps sending large data streams to the Host (Guest Stream).
type StreamSender struct {
	client      *Client
	maxInFlight int
}

// NewStreamSender creates a new StreamSender.
// c: The Client instance.
// maxInFlight: Max number of concurrent chunks (pipelining). Default 1
// (v0.8.9, was 2) — depth 2 measured strictly slower than depth 1 on every
// stream cell on the reference host (memory-controller-bound plateau; overlap
// buys nothing and the extra slot doubles the working set). Pass 2+ explicitly
// for topologies where overlap helps. Mirrors the C++ StreamSender default.
func NewStreamSender(c *Client, maxInFlight int) *StreamSender {
	if maxInFlight <= 0 {
		maxInFlight = 1
	}
	return &StreamSender{
		client:      c,
		maxInFlight: maxInFlight,
	}
}

// Send sends a large payload as a stream.
// It blocks until the stream is fully sent.
func (s *StreamSender) Send(data []byte, streamID uint64) error {
	if s.client == nil {
		return fmt.Errorf("client is nil")
	}

	// 1. Send Stream Start (Synchronous)
	{
		// Acquire a slot
		var slot *GuestSlot
		var err error
		for i := 0; i < 1000; i++ {
			slot, err = s.client.AcquireGuestSlot()
			if err == nil {
				break
			}
			time.Sleep(1 * time.Millisecond)
		}
		if slot == nil {
			return fmt.Errorf("failed to acquire slot for Stream Start: %v", err)
		}

		reqBuf := slot.RequestBuffer()

		headerSize := int(unsafe.Sizeof(StreamHeader{}))
		if len(reqBuf) < headerSize {
			slot.Release()
			return fmt.Errorf("slot buffer too small for StreamHeader")
		}

		// Calculate chunks
		chunkHeaderSize := int(unsafe.Sizeof(ChunkHeader{}))
		maxPayload := len(reqBuf) - chunkHeaderSize
		if maxPayload <= 0 {
			slot.Release()
			return fmt.Errorf("slot buffer too small for ChunkHeader")
		}

		totalChunks := (len(data) + maxPayload - 1) / maxPayload
		if len(data) == 0 {
			totalChunks = 0
		}

		// Write Header manually
		if len(reqBuf) < 24 { // StreamHeader is 24 bytes
			slot.Release()
			return fmt.Errorf("buffer too small")
		}

		binary.LittleEndian.PutUint64(reqBuf[0:], streamID)
		binary.LittleEndian.PutUint64(reqBuf[8:], uint64(len(data)))
		binary.LittleEndian.PutUint32(reqBuf[16:], uint32(totalChunks))
		binary.LittleEndian.PutUint32(reqBuf[20:], 0) // Reserved

		// Send. The reassembler signals rejection (unknown/duplicate stream,
		// size overflow, OOM, eviction pressure) not via a transport error but
		// via respType == MsgTypeSystemError with err == nil — mirror
		// SendGuestCall and surface it as an error. Return immediately so the
		// chunk goroutines below never fire against a stream the host refused
		// to start (which would waste slots on chunks it will also reject).
		_, respType, serr := slot.Send(int32(headerSize), MsgTypeStreamStart)
		slot.Release()
		if serr != nil {
			return fmt.Errorf("failed to send Stream Start: %v", serr)
		}
		if respType == MsgTypeSystemError {
			return fmt.Errorf("stream start rejected by host (streamID %d): system error", streamID)
		}
	}

	// 2. Send Chunks
	if len(data) == 0 {
		return nil
	}

	// Use a semaphore to limit concurrent chunks
	sem := make(chan struct{}, s.maxInFlight)
	errChan := make(chan error, 1)
	var wg sync.WaitGroup

	offset := 0
	chunkIndex := uint32(0)

	for offset < len(data) {
		// Check for errors
		select {
		case err := <-errChan:
			wg.Wait()
			return err
		default:
		}

		sem <- struct{}{} // Acquire token
		wg.Add(1)

		// Acquire Slot Loop
		var slot *GuestSlot
		var err error
		for {
			slot, err = s.client.AcquireGuestSlot()
			if err == nil {
				break
			}

			// Check if any error occurred while waiting
			select {
			case e := <-errChan:
				<-sem // Release token
				wg.Done()
				// Drain already-launched chunk goroutines before returning,
				// mirroring the line-110 error path. Otherwise they keep
				// writing to their slots / calling Release after Send has
				// returned, which becomes a use-after-free if the caller then
				// closes the client and unmaps the region.
				wg.Wait()
				return e
			default:
			}

			time.Sleep(100 * time.Microsecond)
		}

		reqBuf := slot.RequestBuffer()
		chunkHeaderSize := int(unsafe.Sizeof(ChunkHeader{}))
		maxPayload := len(reqBuf) - chunkHeaderSize

		end := offset + maxPayload
		if end > len(data) {
			end = len(data)
		}

		chunkData := data[offset:end]
		chunkSize := len(chunkData)

		go func(slot *GuestSlot, chunkSlice []byte, idx uint32) {
			defer wg.Done()
			defer func() { <-sem }()
			defer slot.Release()

			reqBuf := slot.RequestBuffer()

			if len(reqBuf) < chunkHeaderSize+len(chunkSlice) {
				select {
				case errChan <- fmt.Errorf("buffer overflow"):
				default:
				}
				return
			}

			// Write Header manually. The line above already guarantees
			// len(reqBuf) >= chunkHeaderSize+len(chunkSlice) >= chunkHeaderSize
			// (== 24, pinned by the compile-time size assert in stream.go), so
			// the header writes below are in bounds without a separate <24 check.
			binary.LittleEndian.PutUint64(reqBuf[0:], streamID)
			binary.LittleEndian.PutUint32(reqBuf[8:], idx)
			binary.LittleEndian.PutUint32(reqBuf[12:], uint32(len(chunkSlice)))
			binary.LittleEndian.PutUint32(reqBuf[16:], 0) // Reserved
			binary.LittleEndian.PutUint32(reqBuf[20:], 0) // Padding

			// Write Data
			copy(reqBuf[chunkHeaderSize:], chunkSlice)

			// Send. A reassembler rejection arrives as respType ==
			// MsgTypeSystemError with a nil transport error (see StreamStart
			// above); fold it into err so the existing errChan path propagates
			// it and the whole Send fails instead of silently losing the chunk.
			_, respType, err := slot.Send(int32(chunkHeaderSize+len(chunkSlice)), MsgTypeStreamChunk)
			if err == nil && respType == MsgTypeSystemError {
				err = fmt.Errorf("stream chunk %d rejected by host (streamID %d): system error", idx, streamID)
			}
			if err != nil {
				select {
				case errChan <- err:
				default:
				}
			}
		}(slot, chunkData, chunkIndex)

		offset += chunkSize
		chunkIndex++
	}

	wg.Wait()

	select {
	case err := <-errChan:
		return err
	default:
	}

	return nil
}
