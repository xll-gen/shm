package shm

import (
	"fmt"
	"sync/atomic"
	"time"
)

// GuestSlot represents an acquired slot for Zero-Copy Guest Calls.
// It allows the user to write directly to shared memory and read the response directly,
// avoiding extra allocations and copies.
type GuestSlot struct {
	guest   *DirectGuest
	slotIdx int
	slot    *slotContext
}

// RequestBuffer returns the shared memory buffer for the request.
// The user should write their data into this buffer.
// For standard messages, write at the beginning.
// For Zero-Copy (FlatBuffers), write at the end and use a negative size in Send().
func (s *GuestSlot) RequestBuffer() []byte {
	return s.slot.reqBuffer
}

// ResponseBuffer returns the shared memory buffer for the response.
// This buffer contains the valid response data after Send() returns successfully.
// Note: The valid data range depends on the size returned by the Host (available in the header),
// but for raw access, this returns the full buffer.
func (s *GuestSlot) ResponseBuffer() []byte {
	return s.slot.respBuffer
}

// Send signals the Host to process the request currently in the RequestBuffer.
//
// size: The size of the data written.
//       Positive: Data starts at index 0.
//       Negative: Data ends at the end of the buffer (End-Aligned).
// msgType: The message type.
//
// Returns:
//   - int32: The size of the response (Positive=Start, Negative=End).
//   - MsgType: The type of the response.
//   - error: Error if transaction fails or times out.
func (s *GuestSlot) Send(size int32, msgType MsgType) (int32, MsgType, error) {
	return s.SendWithTimeout(size, msgType, s.guest.responseTimeout)
}

// SendWithTimeout is the same as Send but with a custom timeout.
func (s *GuestSlot) SendWithTimeout(size int32, msgType MsgType, timeout time.Duration) (int32, MsgType, error) {
	header := s.slot.header

	// Validate Size
	absSize := size
	if absSize < 0 {
		absSize = -absSize
	}
	if int(absSize) > len(s.slot.reqBuffer) {
		return 0, 0, fmt.Errorf("request size %d exceeds buffer size %d", absSize, len(s.slot.reqBuffer))
	}

	header.ReqSize = size
	header.MsgType = msgType

	// Signal Ready
	atomic.StoreUint32(&header.State, SlotReqReady)
	SignalEvent(s.slot.reqEvent)

	// Wait for Response
	checkReady := func() bool {
		return atomic.LoadUint32(&header.State) == SlotRespReady
	}

	sleepAction := func() {
		atomic.StoreUint32(&header.GuestState, GuestStateWaiting)
		if checkReady() {
			atomic.StoreUint32(&header.GuestState, GuestStateActive)
			return
		}

		// Loop to handle spurious wakeups
		start := time.Now()
		for {
			if checkReady() {
				break
			}

			elapsed := time.Since(start)
			if elapsed >= timeout {
				break
			}

			remaining := timeout - elapsed
			waitMs := uint32(remaining.Milliseconds())
			if waitMs == 0 && remaining > 0 {
				waitMs = 1
			}
			if waitMs > 100 {
				waitMs = 100
			}

			WaitForEvent(s.slot.respEvent, waitMs)
		}
		atomic.StoreUint32(&header.GuestState, GuestStateActive)
	}

	ready := s.slot.waitStrategy.Wait(checkReady, sleepAction)

	if !ready {
		// Timeout
		return 0, 0, fmt.Errorf("timeout waiting for host")
	}

	return header.RespSize, header.MsgType, nil
}

// Release marks the slot as free for other workers to use.
// MUST be called after processing the response.
func (s *GuestSlot) Release() {
	if s.slot != nil {
		atomic.StoreUint32(&s.slot.header.State, SlotFree)
		s.slot = nil
	}
}

// AcquireGuestSlot acquires a free guest slot for Zero-Copy operations.
// Returns a GuestSlot object or an error if no slots are available.
func (g *DirectGuest) AcquireGuestSlot() (*GuestSlot, error) {
	if g.numGuestSlots == 0 {
		return nil, fmt.Errorf("no guest slots available")
	}

	startIdx := int(g.numSlots)
	endIdx := int(g.numSlots + g.numGuestSlots)

	for i := startIdx; i < endIdx; i++ {
		s := &g.slots[i]
		if atomic.CompareAndSwapUint32(&s.header.State, SlotFree, SlotBusy) {
			return &GuestSlot{
				guest:   g,
				slotIdx: i,
				slot:    s,
			}, nil
		}
	}

	return nil, fmt.Errorf("all guest slots busy")
}
