package shm

import (
	"fmt"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

// busyWaitSetup creates a 1-host / 1-guest SHM segment with all the events a
// DirectGuest expects, connects a Client, and returns the Client plus the
// address of the single guest slot's header (slot index 1). The returned
// cleanup func tears everything down. No mock host is started; callers that
// need one start their own goroutine.
func busyWaitSetup(t *testing.T, shmName string) (client *Client, guestHeader *SlotHeader, addr uintptr, cleanup func()) {
	t.Helper()

	UnlinkShm(shmName)
	UnlinkEvent(fmt.Sprintf("%s_slot_0", shmName))
	UnlinkEvent(fmt.Sprintf("%s_slot_0_resp", shmName))
	UnlinkEvent(fmt.Sprintf("%s_guest_call", shmName))
	UnlinkEvent(fmt.Sprintf("%s_slot_1_resp", shmName))

	totalSize := uint64(64 + 2*(128+1024)) // 1 host + 1 guest
	if totalSize < 4096 {
		totalSize = 4096
	}

	hShm, baseAddr, err := CreateShm(shmName, totalSize)
	if err != nil {
		t.Fatalf("CreateShm failed: %v", err)
	}

	exHeader := (*ExchangeHeader)(unsafe.Pointer(baseAddr))
	exHeader.Magic = Magic
	exHeader.Version = Version
	exHeader.NumSlots = 1
	exHeader.NumGuestSlots = 1
	exHeader.SlotSize = 1024
	exHeader.ReqOffset = 0
	exHeader.RespOffset = 512

	reqEvName0 := fmt.Sprintf("%s_slot_0", shmName)
	respEvName0 := fmt.Sprintf("%s_slot_0_resp", shmName)
	hReq0, _ := CreateEvent(reqEvName0)
	hResp0, _ := CreateEvent(respEvName0)

	reqEvName := fmt.Sprintf("%s_guest_call", shmName)
	hReq, _ := CreateEvent(reqEvName)

	rName := fmt.Sprintf("%s_slot_1_resp", shmName)
	ev, _ := CreateEvent(rName)

	client, err = ConnectDefault(shmName)
	if err != nil {
		t.Fatalf("Connect: %v", err)
	}

	guestSlotPtr := baseAddr + uintptr(64+1*(128+1024)) // guest slot lives at slot index 1
	guestHeader = (*SlotHeader)(unsafe.Pointer(guestSlotPtr))

	cleanup = func() {
		client.Close()
		CloseEvent(ev)
		UnlinkEvent(rName)
		CloseEvent(hReq)
		UnlinkEvent(reqEvName)
		CloseEvent(hResp0)
		UnlinkEvent(respEvName0)
		CloseEvent(hReq0)
		UnlinkEvent(reqEvName0)
		CloseShm(hShm, baseAddr, totalSize)
		UnlinkShm(shmName)
	}
	return client, guestHeader, baseAddr, cleanup
}

// TestSendGuestCallWaitsForBusySlot asserts that when every guest slot is busy,
// SendGuestCallWithTimeout does NOT fail instantly: it waits up to the supplied
// timeout for a slot to free before giving up. Before the fix, slot acquisition
// was a single non-blocking pass that returned "all guest slots busy" in
// microseconds, which permanently stranded one-shot RTD pushes whenever the
// (tiny) guest-slot pool was momentarily saturated on a connect storm.
func TestSendGuestCallWaitsForBusySlot(t *testing.T) {
	client, guestHeader, _, cleanup := busyWaitSetup(t, "GuestBusyWaitSHM")
	defer cleanup()

	// Occupy the only guest slot and never free it.
	atomic.StoreUint32(&guestHeader.State, SlotGuestBusy)

	timeout := 200 * time.Millisecond
	start := time.Now()
	_, err := client.SendGuestCallWithTimeout([]byte{1}, MsgTypeNormal, timeout)
	elapsed := time.Since(start)

	if err == nil {
		t.Fatalf("expected an error while the only guest slot stays busy, got nil")
	}
	if elapsed < timeout*3/4 {
		t.Fatalf("expected SendGuestCall to wait ~%v for a free slot before failing, but it returned after %v (err=%v)", timeout, elapsed, err)
	}
}

// TestSendGuestCallSucceedsWhenSlotFreesMidWait asserts the payoff of the wait:
// a call that starts with all slots busy still succeeds if a slot frees before
// the timeout elapses, instead of having failed instantly at t=0.
func TestSendGuestCallSucceedsWhenSlotFreesMidWait(t *testing.T) {
	client, guestHeader, _, cleanup := busyWaitSetup(t, "GuestBusyFreeMidWaitSHM")
	defer cleanup()

	respEv, _ := CreateEvent(fmt.Sprintf("%s_slot_1_resp", "GuestBusyFreeMidWaitSHM"))
	// (already created in setup; reopening is fine on Windows named events)

	// Mock host: once the guest posts a request on slot 1, ack it immediately.
	stopHost := make(chan struct{})
	hostDone := make(chan struct{})
	go func() {
		defer close(hostDone)
		for {
			select {
			case <-stopHost:
				return
			default:
			}
			if atomic.LoadUint32(&guestHeader.State) == SlotReqReady {
				guestHeader.RespSize = 0
				guestHeader.MsgType = MsgTypeNormal
				atomic.StoreUint32(&guestHeader.State, SlotRespReady)
				SignalEvent(respEv)
			}
			time.Sleep(1 * time.Millisecond)
		}
	}()
	defer func() {
		close(stopHost)
		<-hostDone
		CloseEvent(respEv)
	}()

	// Start with the slot busy, free it after a delay shorter than the timeout.
	atomic.StoreUint32(&guestHeader.State, SlotGuestBusy)
	freeAfter := 80 * time.Millisecond
	go func() {
		time.Sleep(freeAfter)
		atomic.StoreUint32(&guestHeader.State, SlotFree)
	}()

	start := time.Now()
	_, err := client.SendGuestCallWithTimeout([]byte{1}, MsgTypeNormal, 1*time.Second)
	elapsed := time.Since(start)

	if err != nil {
		t.Fatalf("expected success once the slot freed mid-wait, got error: %v", err)
	}
	if elapsed < freeAfter/2 {
		t.Fatalf("call returned suspiciously early (%v) — it should have waited for the slot to free at ~%v", elapsed, freeAfter)
	}
}
