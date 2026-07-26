package shm

import (
	"encoding/binary"
	"fmt"
	"sync"
	"time"
)

// Guest-slot acquisition policy for the streaming sender, shared by the
// StreamStart and chunk phases (unified 2026-07-25). Before that the two
// disagreed: StreamStart retried 1000 × 1 ms and discarded every intermediate
// error, while the chunk loop retried forever at 100 µs — so a permanently
// exhausted guest-slot pool failed one phase and hung the other.
//
// The bound is DefaultStreamTimeout because that is the peer's reassembly
// deadline: a stream whose chunks stall longer than that is pruned on the
// receiving side (SPEC §3.3.4), so waiting beyond it can only produce chunks
// the peer will reject.
const (
	streamSlotAcquireTimeout  = DefaultStreamTimeout
	streamSlotAcquireInterval = 100 * time.Microsecond
)

// putStreamHeader writes a StreamHeader at the start of buf, which the caller
// must have verified holds at least streamHeaderSize bytes. Field offsets are
// SPEC §3.3.1.
func putStreamHeader(buf []byte, streamID, totalSize uint64, totalChunks uint32) {
	binary.LittleEndian.PutUint64(buf[0:], streamID)
	binary.LittleEndian.PutUint64(buf[8:], totalSize)
	binary.LittleEndian.PutUint32(buf[16:], totalChunks)
	binary.LittleEndian.PutUint32(buf[20:], 0) // Reserved
}

// putChunkHeader writes a ChunkHeader at the start of buf, which the caller must
// have verified holds at least chunkHeaderSize bytes. Field offsets are SPEC
// §3.3.2.
func putChunkHeader(buf []byte, streamID uint64, chunkIndex, payloadSize uint32) {
	binary.LittleEndian.PutUint64(buf[0:], streamID)
	binary.LittleEndian.PutUint32(buf[8:], chunkIndex)
	binary.LittleEndian.PutUint32(buf[12:], payloadSize)
	binary.LittleEndian.PutUint32(buf[16:], 0) // Reserved
	binary.LittleEndian.PutUint32(buf[20:], 0) // Padding
}

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

	maxPayload, err := s.sendStart(data, streamID)
	if err != nil {
		return err
	}
	if len(data) == 0 {
		return nil
	}

	if s.maxInFlight == 1 {
		return s.sendChunksInline(data, streamID, maxPayload)
	}
	return s.sendChunksPipelined(data, streamID, maxPayload)
}

// acquireSlot acquires a free guest slot, retrying while the pool is exhausted.
//
// Bounded and cancellable: the wait ends at streamSlotAcquireTimeout with the
// last acquisition error wrapped (never silently dropped), or as soon as abort
// yields a value. abort is the pipelined path's error channel — a chunk that
// already failed makes further slot hunting pointless, and the received error
// is returned verbatim so the caller reports the original failure instead of a
// slot-exhaustion one. Pass nil when there is nothing to cancel on.
func (s *StreamSender) acquireSlot(abort <-chan error) (*GuestSlot, error) {
	deadline := time.Now().Add(streamSlotAcquireTimeout)
	for {
		slot, err := s.client.AcquireGuestSlot()
		if err == nil {
			return slot, nil
		}

		select {
		case e := <-abort:
			return nil, e
		default:
		}

		if !time.Now().Before(deadline) {
			return nil, fmt.Errorf("no free guest slot after %v: %w", streamSlotAcquireTimeout, err)
		}
		time.Sleep(streamSlotAcquireInterval)
	}
}

// sendStart sends MSG_TYPE_STREAM_START synchronously and returns the per-chunk
// payload capacity that the chunk phase must use — the same value totalChunks
// was derived from, so the number of chunks actually sent always matches the
// number advertised.
func (s *StreamSender) sendStart(data []byte, streamID uint64) (maxPayload int, err error) {
	slot, err := s.acquireSlot(nil)
	if err != nil {
		return 0, fmt.Errorf("failed to acquire slot for Stream Start: %w", err)
	}
	defer slot.Release()

	reqBuf := slot.RequestBuffer()

	// Wire-boundary size guards on the exported StreamStart path: the slot
	// buffer is sized by the peer, so both header shapes must be checked here
	// before anything is written. (The former duplicate `len(reqBuf) < 24`
	// check on this same un-resliced buffer was the literal-constant twin of
	// the first guard below — AGENTS.md's collapse rule — and is gone with the
	// hardcoded 24; the guard itself remains.)
	if len(reqBuf) < streamHeaderSize {
		return 0, fmt.Errorf("slot buffer (%d bytes) too small for StreamHeader (%d bytes)", len(reqBuf), streamHeaderSize)
	}
	maxPayload = len(reqBuf) - chunkHeaderSize
	if maxPayload <= 0 {
		return 0, fmt.Errorf("slot buffer (%d bytes) too small for ChunkHeader (%d bytes)", len(reqBuf), chunkHeaderSize)
	}

	totalChunks := 0
	if len(data) > 0 {
		totalChunks = (len(data) + maxPayload - 1) / maxPayload
	}

	putStreamHeader(reqBuf, streamID, uint64(len(data)), uint32(totalChunks))

	// The reassembler signals rejection (unknown/duplicate stream, size
	// overflow, OOM, eviction pressure) not via a transport error but via
	// respType == MsgTypeSystemError with err == nil — mirror SendGuestCall and
	// surface it as an error. Returning here keeps the chunk phase from firing
	// against a stream the host refused to start (which would waste slots on
	// chunks it will also reject).
	_, respType, serr := slot.Send(int32(streamHeaderSize), MsgTypeStreamStart)
	if serr != nil {
		return 0, fmt.Errorf("failed to send Stream Start: %v", serr)
	}
	if respType == MsgTypeSystemError {
		return 0, fmt.Errorf("stream start rejected by host (streamID %d): system error", streamID)
	}
	return maxPayload, nil
}

// sendChunk writes one MSG_TYPE_STREAM_CHUNK (header + payload) into the slot's
// request buffer and sends it. Slot acquisition and release belong to the
// caller, so this is shared verbatim by the inline and pipelined paths.
func sendChunk(slot *GuestSlot, streamID uint64, idx uint32, payload []byte) error {
	reqBuf := slot.RequestBuffer()

	if len(reqBuf) < chunkHeaderSize+len(payload) {
		return fmt.Errorf("buffer overflow")
	}

	// The guard above already gives len(reqBuf) >= chunkHeaderSize+len(payload)
	// >= chunkHeaderSize (pinned to 24 by the compile-time size assert in
	// stream.go), so the header write below is in bounds without a separate check.
	putChunkHeader(reqBuf, streamID, idx, uint32(len(payload)))

	copy(reqBuf[chunkHeaderSize:], payload)

	// A reassembler rejection arrives as respType == MsgTypeSystemError with a
	// nil transport error (see sendStart); fold it into err so callers fail the
	// whole Send instead of silently losing the chunk.
	_, respType, err := slot.Send(int32(chunkHeaderSize+len(payload)), MsgTypeStreamChunk)
	if err == nil && respType == MsgTypeSystemError {
		err = fmt.Errorf("stream chunk %d rejected by host (streamID %d): system error", idx, streamID)
	}
	return err
}

// sendChunksInline sends every chunk on the calling goroutine. Used when
// maxInFlight == 1, the library default since v0.8.9.
//
// At depth 1 the pipelined path below can overlap nothing: its semaphore token
// is taken *before* the slot is acquired and dropped only after the chunk
// goroutine's Send returns, so with cap 1 each per-chunk goroutine is spawned,
// immediately waited on, and torn down — a goroutine creation plus a parent
// park/unpark per chunk, for zero concurrency. This is *not* a revival of
// inFlight > 1 pipelining: the cap > 1 path keeps its previous behavior
// unchanged, this only removes the handoff where it provably buys nothing.
func (s *StreamSender) sendChunksInline(data []byte, streamID uint64, maxPayload int) error {
	offset := 0
	for idx := uint32(0); offset < len(data); idx++ {
		end := offset + maxPayload
		if end > len(data) {
			end = len(data)
		}

		slot, err := s.acquireSlot(nil)
		if err != nil {
			return err
		}
		err = sendChunk(slot, streamID, idx, data[offset:end])
		slot.Release()
		if err != nil {
			return err
		}

		offset = end
	}
	return nil
}

// sendChunksPipelined sends chunks with up to maxInFlight (> 1) in flight,
// one goroutine per chunk gated by a token semaphore.
func (s *StreamSender) sendChunksPipelined(data []byte, streamID uint64, maxPayload int) error {
	sem := make(chan struct{}, s.maxInFlight)
	errChan := make(chan error, 1)
	var wg sync.WaitGroup

	offset := 0
	for idx := uint32(0); offset < len(data); idx++ {
		select {
		case err := <-errChan:
			wg.Wait()
			return err
		default:
		}

		sem <- struct{}{} // Acquire token
		wg.Add(1)

		slot, err := s.acquireSlot(errChan)
		if err != nil {
			<-sem // Release token
			wg.Done()
			// Drain already-launched chunk goroutines before returning,
			// mirroring the loop-top error path. Otherwise they keep writing to
			// their slots / calling Release after Send has returned, which
			// becomes a use-after-free if the caller then closes the client and
			// unmaps the region.
			wg.Wait()
			return err
		}

		end := offset + maxPayload
		if end > len(data) {
			end = len(data)
		}

		go func(slot *GuestSlot, payload []byte, idx uint32) {
			defer wg.Done()
			defer func() { <-sem }()
			defer slot.Release()

			if err := sendChunk(slot, streamID, idx, payload); err != nil {
				select {
				case errChan <- err:
				default:
				}
			}
		}(slot, data[offset:end], idx)

		offset = end
	}

	wg.Wait()

	select {
	case err := <-errChan:
		return err
	default:
	}

	return nil
}
