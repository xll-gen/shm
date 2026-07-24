package shm

import (
	"fmt"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

// TestGuestResponderFastPath exercises the v0.8.8 no-reclaim fast path of the
// guest responder (workerLoopInternal): when the host publishes
// ExchangeHeader.FastPathAllowed==1, the worker skips the per-RTT gen bump +
// REQ_READY→GUEST_BUSY consume-claim + lease refresh and processes the request
// while State stays REQ_READY, publishing RESP_READY as usual. The rest of the
// Go suite builds its ExchangeHeader with FastPathAllowed==0 (zero default), so
// it covers only the slow path; this test flips the flag and drives the Direct
// Exchange responder end-to-end via a mock host on slot 0, across two
// round-trips to confirm re-arm works without the claim.
func TestGuestResponderFastPath(t *testing.T) {
	shmName := "GuestResponderFastPathSHM"
	UnlinkShm(shmName)
	UnlinkEvent(fmt.Sprintf("%s_slot_0", shmName))
	UnlinkEvent(fmt.Sprintf("%s_slot_0_resp", shmName))

	const slotSize = 1024
	const reqOff = 0
	const respOff = 512
	totalSize := uint64(64 + 1*(128+slotSize)) // 1 host slot, 0 guest slots
	if totalSize < 4096 {
		totalSize = 4096
	}

	hShm, addr, err := CreateShm(shmName, totalSize)
	if err != nil {
		t.Fatalf("CreateShm: %v", err)
	}
	defer func() {
		CloseShm(hShm, addr, totalSize)
		UnlinkShm(shmName)
	}()

	ex := (*ExchangeHeader)(unsafe.Pointer(addr))
	ex.Magic = Magic
	ex.Version = Version
	ex.NumSlots = 1
	ex.NumGuestSlots = 0
	ex.SlotSize = slotSize
	ex.ReqOffset = reqOff
	ex.RespOffset = respOff
	atomic.StoreUint32(&ex.FastPathAllowed, 1) // <-- enable the fast path

	reqEvName := fmt.Sprintf("%s_slot_0", shmName)
	respEvName := fmt.Sprintf("%s_slot_0_resp", shmName)
	hReq, _ := CreateEvent(reqEvName)
	hResp, _ := CreateEvent(respEvName)
	defer func() {
		CloseEvent(hReq)
		UnlinkEvent(reqEvName)
		CloseEvent(hResp)
		UnlinkEvent(respEvName)
	}()

	client, err := ConnectDefault(shmName)
	if err != nil {
		t.Fatalf("Connect: %v", err)
	}
	defer client.Close()

	if !client.guest.fastPathAllowed {
		t.Fatalf("guest did not read FastPathAllowed==1 from the ExchangeHeader")
	}

	// Echo handler.
	client.Handle(func(req []byte, respBuf []byte, msgType MsgType) (int32, MsgType) {
		n := copy(respBuf, req)
		return int32(n), MsgTypeNormal
	})
	if err := client.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}

	// Slot 0 geometry (mirrors DirectHost::Init).
	hdr := (*SlotHeader)(unsafe.Pointer(addr + 64))
	reqBuf := unsafe.Slice((*byte)(unsafe.Pointer(addr+64+128+reqOff)), slotSize-reqOff)
	respBuf := unsafe.Slice((*byte)(unsafe.Pointer(addr+64+128+respOff)), slotSize-respOff)

	for round := 0; round < 2; round++ {
		payload := []byte(fmt.Sprintf("ping-%d", round))
		copy(reqBuf, payload)
		hdr.ReqSize = int32(len(payload))
		hdr.MsgType = MsgTypeNormal
		hdr.MsgSeq = uint32(round + 1)

		// Publish the request (mock host). seq_cst store pairs with the
		// worker's acquire load. Always signal — a mock host doesn't spin, and
		// an unconditional doorbell is always correct (the gate is only an
		// optimization on the real host side).
		atomic.StoreUint32(&hdr.State, SlotReqReady)
		SignalEvent(hReq)

		// Await response.
		start := time.Now()
		for atomic.LoadUint32(&hdr.State) != SlotRespReady {
			if time.Since(start) > 3*time.Second {
				t.Fatalf("round %d: timed out waiting for RESP_READY (fast path lost the request?)", round)
			}
			WaitForEvent(hResp, 20)
		}

		if hdr.RespSize != int32(len(payload)) {
			t.Fatalf("round %d: RespSize=%d want %d", round, hdr.RespSize, len(payload))
		}
		if got := string(respBuf[:hdr.RespSize]); got != string(payload) {
			t.Fatalf("round %d: echo mismatch: got %q want %q", round, got, payload)
		}
		if hdr.MsgSeq != uint32(round+1) {
			t.Fatalf("round %d: MsgSeq changed: %d", round, hdr.MsgSeq)
		}

		// Consume + re-arm for the next round (mock host).
		atomic.StoreUint32(&hdr.State, SlotFree)
	}
}

// TestGuestCallFastConsume exercises the v0.8.9 sender-side fast consume:
// with FastPathAllowed==1 (host reclaim off) and the guest's own reclaim off,
// sendGuestCallInternal retains ownership of the responded slot via
// ActiveWait alone (no gen bump / consume CAS / lease refresh) and releases it
// straight from SlotRespReady. Two back-to-back calls verify the slot recycles
// cleanly through the fast release; run under -race in CI.
func TestGuestCallFastConsume(t *testing.T) {
	shmName := "GuestCallFastConsumeSHM"
	UnlinkShm(shmName)
	UnlinkEvent(fmt.Sprintf("%s_slot_0", shmName))
	UnlinkEvent(fmt.Sprintf("%s_slot_0_resp", shmName))
	UnlinkEvent(fmt.Sprintf("%s_guest_call", shmName))
	UnlinkEvent(fmt.Sprintf("%s_slot_1_resp", shmName))

	totalSize := uint64(64 + 2*(128+1024)) // 1 host + 1 guest slot
	if totalSize < 4096 {
		totalSize = 4096
	}
	hShm, addr, err := CreateShm(shmName, totalSize)
	if err != nil {
		t.Fatalf("CreateShm: %v", err)
	}
	defer func() {
		CloseShm(hShm, addr, totalSize)
		UnlinkShm(shmName)
	}()

	ex := (*ExchangeHeader)(unsafe.Pointer(addr))
	ex.Magic = Magic
	ex.Version = Version
	ex.NumSlots = 1
	ex.NumGuestSlots = 1
	ex.SlotSize = 1024
	ex.ReqOffset = 0
	ex.RespOffset = 512
	atomic.StoreUint32(&ex.FastPathAllowed, 1) // <-- enable fast consume

	for _, n := range []string{"_slot_0", "_slot_0_resp", "_guest_call", "_slot_1_resp"} {
		ev, err := CreateEvent(shmName + n)
		if err != nil {
			t.Fatalf("CreateEvent %s: %v", n, err)
		}
		name := shmName + n
		defer func() { CloseEvent(ev); UnlinkEvent(name) }()
	}
	hResp, _ := OpenEvent(shmName + "_slot_1_resp")
	defer CloseEvent(hResp)

	// Mock host: echo two guest calls on guest slot (index 1).
	stop := make(chan struct{})
	done := make(chan struct{})
	go func() {
		defer close(done)
		gh := (*SlotHeader)(unsafe.Pointer(addr + uintptr(64+1*(128+1024))))
		for served := 0; served < 2; {
			select {
			case <-stop:
				return
			default:
			}
			if atomic.LoadUint32(&gh.State) == SlotReqReady {
				// mimic ProcessGuestCalls: claim, echo, publish
				if atomic.CompareAndSwapUint32(&gh.State, SlotReqReady, SlotBusy) {
					gh.RespSize = gh.ReqSize
					reqB := unsafe.Slice((*byte)(unsafe.Pointer(addr+uintptr(64+1*(128+1024))+128)), 512)
					respB := unsafe.Slice((*byte)(unsafe.Pointer(addr+uintptr(64+1*(128+1024))+128+512)), 512)
					copy(respB, reqB[:gh.ReqSize])
					atomic.StoreUint32(&gh.State, SlotRespReady)
					SignalEvent(hResp)
					served++
				}
			}
			time.Sleep(100 * time.Microsecond)
		}
	}()
	defer func() { close(stop); <-done }()

	guest, err := NewDirectGuest(shmName)
	if err != nil {
		t.Fatalf("NewDirectGuest: %v", err)
	}
	defer guest.Close()

	if !guest.fastPathAllowed {
		t.Fatalf("guest did not read FastPathAllowed==1")
	}

	for round := 0; round < 2; round++ {
		payload := []byte(fmt.Sprintf("fast-%d", round))
		resp, err := guest.SendGuestCall(payload, MsgTypeGuestCall)
		if err != nil {
			t.Fatalf("round %d: SendGuestCall: %v", round, err)
		}
		if string(resp) != string(payload) {
			t.Fatalf("round %d: echo mismatch: got %q want %q", round, resp, payload)
		}
		// Fast release must leave the slot claimable again (SlotFree).
		gh := (*SlotHeader)(unsafe.Pointer(addr + uintptr(64+1*(128+1024))))
		if st := atomic.LoadUint32(&gh.State); st != SlotFree {
			t.Fatalf("round %d: slot not released, state=%d", round, st)
		}
		if aw := atomic.LoadInt32(&guest.slots[1].ActiveWait); aw != 0 {
			t.Fatalf("round %d: ActiveWait leaked: %d", round, aw)
		}
	}
}

// fpHostSlotEnv is a minimal 1-host-slot fast-path environment: a real shm
// region with FastPathAllowed==1, a connected Client (worker not yet started),
// and direct handles to the slot header/buffers so the test can act as the
// C++ host by poking shared memory (same pattern as the tests above).
type fpHostSlotEnv struct {
	client  *Client
	hdr     *SlotHeader
	reqBuf  []byte
	respBuf []byte
	hReq    EventHandle
	hResp   EventHandle
	cleanup func()
}

func newFPHostSlotEnv(t *testing.T, shmName string) *fpHostSlotEnv {
	t.Helper()

	UnlinkShm(shmName)
	UnlinkEvent(fmt.Sprintf("%s_slot_0", shmName))
	UnlinkEvent(fmt.Sprintf("%s_slot_0_resp", shmName))

	const slotSize = 1024
	const reqOff = 0
	const respOff = 512
	totalSize := uint64(64 + 1*(128+slotSize))
	if totalSize < 4096 {
		totalSize = 4096
	}

	hShm, addr, err := CreateShm(shmName, totalSize)
	if err != nil {
		t.Fatalf("CreateShm: %v", err)
	}

	ex := (*ExchangeHeader)(unsafe.Pointer(addr))
	ex.Magic = Magic
	ex.Version = Version
	ex.NumSlots = 1
	ex.NumGuestSlots = 0
	ex.SlotSize = slotSize
	ex.ReqOffset = reqOff
	ex.RespOffset = respOff
	atomic.StoreUint32(&ex.FastPathAllowed, 1)

	reqEvName := fmt.Sprintf("%s_slot_0", shmName)
	respEvName := fmt.Sprintf("%s_slot_0_resp", shmName)
	hReq, _ := CreateEvent(reqEvName)
	hResp, _ := CreateEvent(respEvName)

	client, err := ConnectDefault(shmName)
	if err != nil {
		t.Fatalf("Connect: %v", err)
	}
	if !client.guest.fastPathAllowed {
		t.Fatalf("guest did not read FastPathAllowed==1")
	}

	hdr := (*SlotHeader)(unsafe.Pointer(addr + 64))
	reqBuf := unsafe.Slice((*byte)(unsafe.Pointer(addr+64+128+reqOff)), slotSize-reqOff)
	respBuf := unsafe.Slice((*byte)(unsafe.Pointer(addr+64+128+respOff)), slotSize-respOff)

	env := &fpHostSlotEnv{
		client: client, hdr: hdr, reqBuf: reqBuf, respBuf: respBuf, hReq: hReq, hResp: hResp,
	}
	env.cleanup = func() {
		client.Close()
		CloseEvent(hReq)
		UnlinkEvent(reqEvName)
		CloseEvent(hResp)
		UnlinkEvent(respEvName)
		CloseShm(hShm, addr, totalSize)
		UnlinkShm(shmName)
	}
	return env
}

func waitForStateEq(hdr *SlotHeader, want uint32, d time.Duration) bool {
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if atomic.LoadUint32(&hdr.State) == want {
			return true
		}
		time.Sleep(200 * time.Microsecond)
	}
	return atomic.LoadUint32(&hdr.State) == want
}

// TestGuestResponderFastPath_SlowHandlerNoReclaimIsSafe pins the safety of the
// v0.8.8 fast-path blind RESP_READY store (workerLoopInternal) when a handler
// runs long enough to exceed the host's response timeout. Because
// FastPathAllowed==1 requires host auto-reclaim to be OFF (§3.4 safe-by-default
// polarity), no reclaimer ever creates a competing owner, so there is no
// zombie-steal for the late blind store to land on. The store completes the
// ORIGINAL transaction: msgSeq intact, response correct. This is the
// "defense already exists" pin for backlog item 3 — the guarding invariant is
// the no-reclaim contract, not a msgSeq re-check.
func TestGuestResponderFastPath_SlowHandlerNoReclaimIsSafe(t *testing.T) {
	env := newFPHostSlotEnv(t, "FastPathSlowHandlerSafeSHM")
	defer env.cleanup()

	release := make(chan struct{})
	entered := make(chan struct{})
	var firstCall int32
	env.client.Handle(func(req []byte, respBuf []byte, mt MsgType) (int32, MsgType) {
		if atomic.CompareAndSwapInt32(&firstCall, 0, 1) {
			close(entered)
			<-release // block well past a would-be response timeout
		}
		n := copy(respBuf, req)
		return int32(n), mt
	})
	if err := env.client.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}

	payload := []byte("ping")
	copy(env.reqBuf, payload)
	env.hdr.ReqSize = int32(len(payload))
	env.hdr.MsgType = MsgTypeNormal
	env.hdr.MsgSeq = 1
	atomic.StoreUint32(&env.hdr.State, SlotReqReady)
	SignalEvent(env.hReq)

	<-entered // handler is running and blocked

	// Simulate the host's responseTimeout elapsing while the handler works:
	// the host must not see RESP_READY yet.
	if waitForStateEq(env.hdr, SlotRespReady, 150*time.Millisecond) {
		t.Fatal("RESP_READY appeared while the handler was still blocked")
	}
	// A conforming fast-path host (auto-reclaim off) does NOT reclaim/reuse the
	// slot on timeout; it just keeps waiting. Nothing else touches the slot.

	close(release)

	if !waitForStateEq(env.hdr, SlotRespReady, 3*time.Second) {
		t.Fatal("timed out waiting for RESP_READY after the handler completed")
	}
	if env.hdr.MsgSeq != 1 {
		t.Fatalf("MsgSeq=%d want 1 (blind store must land on the original transaction)", env.hdr.MsgSeq)
	}
	if env.hdr.RespSize != int32(len(payload)) {
		t.Fatalf("RespSize=%d want %d", env.hdr.RespSize, len(payload))
	}
	if got := string(env.respBuf[:env.hdr.RespSize]); got != string(payload) {
		t.Fatalf("echo mismatch: got %q want %q", got, payload)
	}
}

// TestGuestResponderFastPath_HostReclaimDuringHandlerCorrupts documents the
// reproduction condition for backlog item 3: it is reachable ONLY by violating
// the fast-path no-reclaim contract. When the host (contrary to
// FastPathAllowed==1) abandons a slow handler, reclaims the slot, and starts a
// new transaction on it, the guest's late blind RESP_READY store lands on the
// new owner. Because the responder never rewrites MsgSeq, the new transaction's
// msgSeq survives — so the msgSeq guard on the requester side validates and
// accepts the STALE response bytes. This confirms the item-3 hypothesis (the
// msgSeq guard cannot catch it) while also showing the hazard cannot arise
// under a conforming host, so no code fix is applied (see report).
func TestGuestResponderFastPath_HostReclaimDuringHandlerCorrupts(t *testing.T) {
	env := newFPHostSlotEnv(t, "FastPathReclaimCorruptSHM")
	defer env.cleanup()

	release := make(chan struct{})
	entered := make(chan struct{})
	var firstCall int32
	var firstReq []byte
	env.client.Handle(func(req []byte, respBuf []byte, mt MsgType) (int32, MsgType) {
		if atomic.CompareAndSwapInt32(&firstCall, 0, 1) {
			firstReq = append([]byte(nil), req...) // snapshot txn#1 before the host overwrites the buffer
			close(entered)
			<-release
			copy(respBuf, firstReq)
			return int32(len(firstReq)), MsgTypeNormal
		}
		n := copy(respBuf, req)
		return int32(n), mt
	})
	if err := env.client.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}

	// Host publishes transaction #1 (msgSeq 1, "AAAA").
	req1 := []byte("AAAA")
	copy(env.reqBuf, req1)
	env.hdr.ReqSize = int32(len(req1))
	env.hdr.MsgType = MsgTypeNormal
	env.hdr.MsgSeq = 1
	atomic.StoreUint32(&env.hdr.State, SlotReqReady)
	SignalEvent(env.hReq)

	<-entered // worker is inside handler(txn#1), blocked

	// CONTRACT VIOLATION (reproduction only): abandon the slow handler, reclaim
	// the slot (State->FREE), and start a NEW transaction #2 (msgSeq 2, "BBBB")
	// on it. A conforming FastPathAllowed==1 host has auto-reclaim OFF and never
	// does this.
	req2 := []byte("BBBB")
	atomic.StoreUint32(&env.hdr.State, SlotFree)
	copy(env.reqBuf, req2)
	env.hdr.ReqSize = int32(len(req2))
	env.hdr.MsgType = MsgTypeGuestCall
	env.hdr.MsgSeq = 2
	atomic.StoreUint32(&env.hdr.State, SlotReqReady)

	close(release) // handler#1 finishes and blind-stores RESP_READY over txn#2

	if !waitForStateEq(env.hdr, SlotRespReady, 3*time.Second) {
		t.Fatal("timed out waiting for the late blind RESP_READY store")
	}

	// Current (unguarded) behavior: the blind store published RESP_READY over
	// txn#2's REQ_READY; msgSeq is still 2 (the responder never rewrites it),
	// but the payload is txn#1's "AAAA". A host waiting on txn#2 sees msgSeq==2
	// and accepts the stale bytes — the msgSeq guard cannot detect the mix-up.
	if got := atomic.LoadUint32(&env.hdr.State); got != SlotRespReady {
		t.Fatalf("State=%d want SlotRespReady", got)
	}
	if env.hdr.MsgSeq != 2 {
		t.Fatalf("MsgSeq=%d want 2 (responder does not rewrite msgSeq)", env.hdr.MsgSeq)
	}
	if got := string(env.respBuf[:env.hdr.RespSize]); got != string(req1) {
		t.Fatalf("expected stale txn#1 payload %q clobbering txn#2, got %q", req1, got)
	}
	t.Logf("REPRODUCED only via fast-path contract violation: blind RESP_READY published txn#1 bytes %q under txn#2 msgSeq=2; msgSeq guard cannot detect it. Unreachable by a conforming host (FastPathAllowed==1 ⇒ auto-reclaim off).", req1)
}
