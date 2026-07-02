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
	// lost records that slot ownership was forfeited: the wait timed out
	// (the host may still own the slot in SlotReqReady/SlotBusy, or its
	// late SlotRespReady belongs to the zombie-reclaim path), or the
	// consume-claim CAS lost to a reclaimer. Release() must then leave
	// State untouched — storing SlotFree would clobber the host's or a
	// new owner's transaction. Recovery is handled by the Case-2 zombie
	// reclaim in AcquireGuestSlot or by lease-based reclamation.
	lost bool
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
//
//	Positive: Data starts at index 0.
//	Negative: Data ends at the end of the buffer (End-Aligned).
//
// msgType: The message type.
//
// Returns:
//   - int32: The size of the response (Positive=Start, Negative=End).
//   - MsgType: The type of the response.
//   - error: Error if transaction fails or times out.
func (s *GuestSlot) Send(size int32, msgType MsgType) (int32, MsgType, error) {
	return s.SendWithTimeout(size, msgType, s.guest.responseTimeout())
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

	// Set MsgSeq
	currentSeq := s.slot.nextMsgSeq
	header.MsgSeq = currentSeq
	s.slot.nextMsgSeq += uint32(len(s.guest.slots))

	// Mark this goroutine as an active waiter BEFORE publishing the request:
	// from the moment State can become SlotRespReady, the slot must never
	// exhibit the zombie signature (SlotRespReady && ActiveWait==0) while
	// still owned, or AcquireGuestSlot's Case-2 reclaim could steal it.
	atomic.StoreInt32(&s.slot.ActiveWait, 1)

	// Signal Ready
	atomic.StoreUint32(&header.State, SlotReqReady)
	SignalEvent(s.slot.reqEvent)

	// Wait for Response
	sleepAction := func() {
		atomic.StoreUint32(&header.GuestState, GuestStateWaiting)
		if atomic.LoadUint32(&header.State) == SlotRespReady {
			atomic.StoreUint32(&header.GuestState, GuestStateActive)
			return
		}

		// Loop to handle spurious wakeups
		start := time.Now()
		for {
			if atomic.LoadUint32(&header.State) == SlotRespReady {
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

	ready := s.slot.waitStrategy.WaitState(&header.State, SlotRespReady, sleepAction)
	claimed := false
	if ready {
		// Consume-claim: take the slot back to SlotGuestBusy BEFORE
		// clearing ActiveWait, so it never looks like a zombie while the
		// caller still reads ResponseBuffer(). Refresh the lease per
		// SPECIFICATION.md §3.6 (every claiming CAS heartbeats) and bump
		// Gen per §3.6.1 before re-claiming.
		claimSlotGen(header)
		claimed = atomic.CompareAndSwapUint32(&header.State, SlotRespReady, SlotGuestBusy)
		if claimed {
			atomic.StoreUint64(&header.Lease, MonotonicNanos())
		}
	}
	atomic.StoreInt32(&s.slot.ActiveWait, 0)

	if !ready {
		// Timeout: the host may still own the slot (SlotReqReady/SlotBusy).
		// Forfeit ownership so Release() leaves State untouched.
		s.lost = true
		return 0, 0, fmt.Errorf("timeout waiting for host")
	}
	if !claimed {
		// A reclaimer (lease-based crash recovery) took the slot between
		// observing SlotRespReady and the consume-claim CAS. The response
		// buffer can no longer be trusted; forfeit ownership.
		s.lost = true
		return 0, 0, fmt.Errorf("slot reclaimed while consuming response")
	}

	// Verify MsgSeq
	if header.MsgSeq != currentSeq {
		return 0, 0, fmt.Errorf("msgSeq mismatch: expected %d, got %d", currentSeq, header.MsgSeq)
	}

	return header.RespSize, header.MsgType, nil
}

// Release marks the slot as free for other workers to use.
// MUST be called after processing the response.
//
// If the preceding Send timed out (or lost its consume-claim), Release
// leaves State untouched: the host may still own the slot, and a blind
// SlotFree store would clobber the in-flight transaction of whoever owns
// it next. Such slots are recovered by zombie/lease reclamation instead.
func (s *GuestSlot) Release() {
	if s.slot != nil {
		if !s.lost {
			// CAS from the owned state (SlotGuestBusy) instead of a blind
			// store: if a lease-based reclaimer stole the slot, the new
			// owner's state must not be clobbered with SlotFree.
			atomic.CompareAndSwapUint32(&s.slot.header.State, SlotGuestBusy, SlotFree)
		}
		s.slot = nil
	}
}

// AcquireGuestSlot acquires a free guest slot for Zero-Copy operations.
// Returns a GuestSlot object or an error if no slots are available.
func (g *DirectGuest) AcquireGuestSlot() (*GuestSlot, error) {
	if g.numGuestSlots == 0 {
		return nil, fmt.Errorf("no guest slots available")
	}

	// Use Round-Robin to pick a start index
	offset := atomic.AddUint32(&g.nextGuestSlot, 1)
	startBase := int(g.numSlots)
	numGuest := int(g.numGuestSlots)

	for j := 0; j < numGuest; j++ {
		i := startBase + int((uint32(j)+offset)%uint32(numGuest))
		s := &g.slots[i]

		// Claim a Free slot, or steal a SlotRespReady zombie (late host
		// response whose waiter timed out, ActiveWait==0) via the Gen CAS
		// handshake that arbitrates against a concurrent legitimate re-use of
		// the same zombie (§3.6.1). See tryClaimGuestSlot in direct.go.
		if tryClaimGuestSlot(s) {
			return &GuestSlot{
				guest:   g,
				slotIdx: i,
				slot:    s,
			}, nil
		}
	}

	return nil, fmt.Errorf("all guest slots busy")
}
