package shm

import (
	"fmt"
	"sync"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

// Regression tests for the zombie-slot steal bug:
//
// A guest-call waiter used to clear ActiveWait immediately after WaitState
// observed SlotRespReady, and only later read the response (for GuestSlot
// the caller may hold ResponseBuffer() arbitrarily long before Release()).
// During that window the slot carried the zombie signature
// (SlotRespReady && ActiveWait==0), so AcquireGuestSlot /
// sendGuestCallInternal Case-2 reclaim could CAS-steal it and start a new
// transaction over the buffer the original owner was still reading. The
// original owner's Release() then blindly stored SlotFree, clobbering the
// thief's state.
//
// The fix consume-claims SlotRespReady -> SlotGuestBusy before clearing
// ActiveWait, releases via CAS from the owned state, and forfeits
// ownership (no SlotFree store) after a timeout.

const (
	zsSlotSize   = 1024
	zsRespOffset = 512
	zsPerSlot    = 128 + zsSlotSize
)

// zombieStealEnv creates a real shared-memory region with 1 host slot and
// numGuest guest slots plus the named events NewDirectGuest expects. The
// test acts as the Host by poking SlotHeaders directly (same pattern as
// regression_test.go's mock hosts).
type zombieStealEnv struct {
	name     string
	addr     uintptr
	numGuest int
	// respEvents[k] is the response event handle for ABSOLUTE slot index k.
	respEvents map[int]EventHandle
	cleanup    []func()
}

func newZombieStealEnv(t *testing.T, name string, numGuest int) *zombieStealEnv {
	t.Helper()

	env := &zombieStealEnv{
		name:       name,
		numGuest:   numGuest,
		respEvents: make(map[int]EventHandle),
	}

	// Best-effort cleanup of leftovers from a previous crashed run.
	UnlinkShm(name)
	UnlinkEvent(fmt.Sprintf("%s_slot_0", name))
	UnlinkEvent(fmt.Sprintf("%s_guest_call", name))
	for k := 0; k <= numGuest; k++ {
		UnlinkEvent(fmt.Sprintf("%s_slot_%d_resp", name, k))
	}

	totalSlots := 1 + numGuest
	totalSize := uint64(64 + totalSlots*zsPerSlot)
	if totalSize < 4096 {
		totalSize = 4096
	}

	hShm, addr, err := CreateShm(name, totalSize)
	if err != nil {
		t.Fatalf("CreateShm failed: %v", err)
	}
	env.addr = addr
	env.cleanup = append(env.cleanup, func() {
		CloseShm(hShm, addr, totalSize)
		UnlinkShm(name)
	})

	exHeader := (*ExchangeHeader)(unsafe.Pointer(addr))
	exHeader.Magic = Magic
	exHeader.Version = Version
	exHeader.NumSlots = 1
	exHeader.NumGuestSlots = uint32(numGuest)
	exHeader.SlotSize = zsSlotSize
	exHeader.ReqOffset = 0
	exHeader.RespOffset = zsRespOffset

	mkEvent := func(evName string) EventHandle {
		h, err := CreateEvent(evName)
		if err != nil {
			t.Fatalf("CreateEvent %s failed: %v", evName, err)
		}
		env.cleanup = append(env.cleanup, func() {
			CloseEvent(h)
			UnlinkEvent(evName)
		})
		return h
	}

	mkEvent(fmt.Sprintf("%s_slot_0", name))
	env.respEvents[0] = mkEvent(fmt.Sprintf("%s_slot_0_resp", name))
	mkEvent(fmt.Sprintf("%s_guest_call", name))
	for k := 1; k <= numGuest; k++ {
		env.respEvents[k] = mkEvent(fmt.Sprintf("%s_slot_%d_resp", name, k))
	}

	return env
}

func (e *zombieStealEnv) Close() {
	for i := len(e.cleanup) - 1; i >= 0; i-- {
		e.cleanup[i]()
	}
}

// slotHeader returns the SlotHeader for ABSOLUTE slot index k.
func (e *zombieStealEnv) slotHeader(k int) *SlotHeader {
	return (*SlotHeader)(unsafe.Pointer(e.addr + 64 + uintptr(k)*zsPerSlot))
}

func (e *zombieStealEnv) reqBuf(k int) []byte {
	base := e.addr + 64 + uintptr(k)*zsPerSlot + 128
	return unsafe.Slice((*byte)(unsafe.Pointer(base)), zsRespOffset)
}

func (e *zombieStealEnv) respBuf(k int) []byte {
	base := e.addr + 64 + uintptr(k)*zsPerSlot + 128 + zsRespOffset
	return unsafe.Slice((*byte)(unsafe.Pointer(base)), zsSlotSize-zsRespOffset)
}

// respondOnce mimics the C++ host's ProcessGuestCalls for one request on
// absolute slot k: wait for SLOT_REQ_READY, CAS to SLOT_BUSY, write resp,
// publish SLOT_RESP_READY, signal.
func (e *zombieStealEnv) respondOnce(t *testing.T, k int, resp []byte) bool {
	hdr := e.slotHeader(k)
	deadline := time.Now().Add(5 * time.Second)
	for {
		if atomic.LoadUint32(&hdr.State) == SlotReqReady {
			if atomic.CompareAndSwapUint32(&hdr.State, SlotReqReady, SlotBusy) {
				break
			}
		}
		if time.Now().After(deadline) {
			t.Errorf("mock host: timed out waiting for SlotReqReady on slot %d", k)
			return false
		}
		time.Sleep(100 * time.Microsecond)
	}
	copy(e.respBuf(k), resp)
	hdr.RespSize = int32(len(resp))
	hdr.MsgType = MsgTypeNormal
	atomic.StoreUint32(&hdr.State, SlotRespReady)
	SignalEvent(e.respEvents[k])
	return true
}

// TestGuestSlot_NotStolenWhileConsumingResponse pins the core invariant:
// between a successful Send() and Release(), the slot is owned by the
// caller and must not be acquirable by anyone else.
//
// Before the fix this failed deterministically: after Send() returned, the
// slot sat in SlotRespReady with ActiveWait==0 — exactly the zombie
// signature — so the second AcquireGuestSlot CAS-stole it.
func TestGuestSlot_NotStolenWhileConsumingResponse(t *testing.T) {
	env := newZombieStealEnv(t, "ZombieStealConsumeSHM", 1)
	defer env.Close()

	guest, err := NewDirectGuest(env.name)
	if err != nil {
		t.Fatalf("NewDirectGuest failed: %v", err)
	}
	defer guest.Close()

	hostDone := make(chan struct{})
	go func() {
		defer close(hostDone)
		env.respondOnce(t, 1, []byte("PONG"))
	}()

	slot, err := guest.AcquireGuestSlot()
	if err != nil {
		t.Fatalf("AcquireGuestSlot failed: %v", err)
	}
	copy(slot.RequestBuffer(), "ping")
	respSize, _, err := slot.Send(4, MsgTypeGuestCall)
	<-hostDone
	if err != nil {
		t.Fatalf("Send failed: %v", err)
	}

	// The owner is still consuming ResponseBuffer(): the slot must NOT be
	// acquirable. Before the fix this acquire succeeded via the Case-2
	// zombie reclaim and started a new transaction over our response.
	if thief, terr := guest.AcquireGuestSlot(); terr == nil {
		thief.Release()
		t.Fatal("AcquireGuestSlot stole the slot while its owner was still consuming the response")
	}

	if got := string(slot.ResponseBuffer()[:respSize]); got != "PONG" {
		t.Fatalf("response corrupted: %q", got)
	}
	slot.Release()

	// After Release the slot must be available again.
	s2, err := guest.AcquireGuestSlot()
	if err != nil {
		t.Fatalf("slot not acquirable after Release: %v", err)
	}
	s2.Release()

	if st := atomic.LoadUint32(&env.slotHeader(1).State); st != SlotFree {
		t.Fatalf("slot state after final Release = %d, want SlotFree", st)
	}
}

// TestGuestSlot_ReleaseAfterTimeoutDoesNotFreeHostOwnedSlot pins the
// Release()-after-timeout contract: while the host still owns the slot
// (SlotReqReady), Release must NOT store SlotFree — that would hand the
// slot to a new owner whose transaction the host's late response would
// corrupt. The slot is recovered later by the Case-2 zombie reclaim once
// the host posts its (late) response.
func TestGuestSlot_ReleaseAfterTimeoutDoesNotFreeHostOwnedSlot(t *testing.T) {
	env := newZombieStealEnv(t, "ZombieStealTimeoutSHM", 1)
	defer env.Close()

	guest, err := NewDirectGuest(env.name)
	if err != nil {
		t.Fatalf("NewDirectGuest failed: %v", err)
	}
	defer guest.Close()

	slot, err := guest.AcquireGuestSlot()
	if err != nil {
		t.Fatalf("AcquireGuestSlot failed: %v", err)
	}
	copy(slot.RequestBuffer(), "ping")

	// No host is responding: the send must time out.
	if _, _, err := slot.SendWithTimeout(4, MsgTypeGuestCall, 50*time.Millisecond); err == nil {
		t.Fatal("SendWithTimeout should have timed out")
	}
	slot.Release()

	hdr := env.slotHeader(1)
	switch st := atomic.LoadUint32(&hdr.State); st {
	case SlotFree:
		t.Fatal("Release after timeout stored SlotFree while the host still owned the slot (SlotReqReady)")
	case SlotReqReady:
		// expected: host still owns the slot
	default:
		t.Fatalf("unexpected slot state after timed-out Release: %d", st)
	}

	// Late host response arrives: the slot is now a true zombie
	// (SlotRespReady, no active waiter) ...
	hdr.RespSize = 0
	hdr.MsgType = MsgTypeNormal
	atomic.StoreUint32(&hdr.State, SlotRespReady)

	// ... and the Case-2 zombie reclaim must recover it.
	s2, err := guest.AcquireGuestSlot()
	if err != nil {
		t.Fatalf("Case-2 zombie reclaim failed to recover the slot: %v", err)
	}
	s2.Release()
}

// TestGuestSlot_ConcurrentConsumersNoCorruption stress-drives the steal
// window: workers hold ResponseBuffer() for a while before Release while
// other workers continuously try to acquire slots. With an echo host, any
// steal manifests as a corrupted response (the thief's transaction
// overwrites the buffer the holder is reading) or a msgSeq mismatch.
func TestGuestSlot_ConcurrentConsumersNoCorruption(t *testing.T) {
	const numGuest = 2
	const numWorkers = 4
	const iters = 50

	env := newZombieStealEnv(t, "ZombieStealStressSHM", numGuest)
	defer env.Close()

	guest, err := NewDirectGuest(env.name)
	if err != nil {
		t.Fatalf("NewDirectGuest failed: %v", err)
	}
	defer guest.Close()

	// Echo host: polls all guest slots, echoes requests back.
	stop := make(chan struct{})
	var hostWg sync.WaitGroup
	hostWg.Add(1)
	go func() {
		defer hostWg.Done()
		for {
			select {
			case <-stop:
				return
			default:
			}
			progressed := false
			for k := 1; k <= numGuest; k++ {
				hdr := env.slotHeader(k)
				if atomic.LoadUint32(&hdr.State) == SlotReqReady {
					if atomic.CompareAndSwapUint32(&hdr.State, SlotReqReady, SlotBusy) {
						n := hdr.ReqSize
						if n > 0 && int(n) <= len(env.reqBuf(k)) {
							copy(env.respBuf(k), env.reqBuf(k)[:n])
							hdr.RespSize = n
						} else {
							hdr.RespSize = 0
						}
						hdr.MsgType = MsgTypeNormal
						atomic.StoreUint32(&hdr.State, SlotRespReady)
						SignalEvent(env.respEvents[k])
						progressed = true
					}
				}
			}
			if !progressed {
				time.Sleep(20 * time.Microsecond)
			}
		}
	}()
	defer func() {
		close(stop)
		hostWg.Wait()
	}()

	// raceHB mirrors the per-slot ownership handoff for the benefit of the
	// Go race detector. The REAL synchronization is the State atomic in the
	// SlotHeader, but that lives in file-mapped shared memory, and the race
	// detector cannot model happens-before edges through atomics located
	// outside the Go heap (verified empirically: a plain heap handoff
	// synchronized only by an atomic in CreateShm memory is reported as a
	// race). Without this mirror, every legitimate slot handoff between
	// workers is misreported as a race on slotContext.nextMsgSeq.
	//
	// Each worker acquire-loads the mirror right after winning the slot and
	// release-stores it right before Release(). This cannot mask the steal
	// bug this test targets: a thief acquires while the holder has NOT yet
	// stored the mirror, and corruption is asserted functionally (payload
	// equality below), not via the race detector.
	var raceHB [numGuest]uint32

	var workerWg sync.WaitGroup
	errCh := make(chan error, numWorkers*iters)
	for w := 0; w < numWorkers; w++ {
		workerWg.Add(1)
		go func(w int) {
			defer workerWg.Done()
			for i := 0; i < iters; i++ {
				payload := fmt.Sprintf("worker-%d-iter-%04d", w, i)

				var slot *GuestSlot
				deadline := time.Now().Add(5 * time.Second)
				for {
					var aerr error
					slot, aerr = guest.AcquireGuestSlot()
					if aerr == nil {
						break
					}
					if time.Now().After(deadline) {
						errCh <- fmt.Errorf("worker %d iter %d: acquire timed out: %v", w, i, aerr)
						return
					}
					time.Sleep(20 * time.Microsecond)
				}
				atomic.LoadUint32(&raceHB[slot.slotIdx-1])

				copy(slot.RequestBuffer(), payload)
				n, _, serr := slot.Send(int32(len(payload)), MsgTypeGuestCall)
				if serr != nil {
					errCh <- fmt.Errorf("worker %d iter %d: send failed: %v", w, i, serr)
					atomic.StoreUint32(&raceHB[slot.slotIdx-1], 1)
					slot.Release()
					return
				}

				// Hold the response while other workers hammer
				// AcquireGuestSlot — this is the steal window.
				time.Sleep(200 * time.Microsecond)

				if got := string(slot.ResponseBuffer()[:n]); got != payload {
					errCh <- fmt.Errorf("worker %d iter %d: response corrupted: want %q, got %q", w, i, payload, got)
					atomic.StoreUint32(&raceHB[slot.slotIdx-1], 1)
					slot.Release()
					return
				}
				atomic.StoreUint32(&raceHB[slot.slotIdx-1], 1)
				slot.Release()
			}
		}(w)
	}
	workerWg.Wait()
	close(errCh)
	for err := range errCh {
		t.Error(err)
	}
}
