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

// TestGuestResponderFastPath_SlowHandlerNoReclaimIsSafe pins the no-false-
// positive side of the v0.8.14 publish CAS: a handler that runs long enough to
// exceed the host's response timeout, on a slot NOBODY steals, must still
// complete its ORIGINAL transaction. The CAS from the responder's owned
// SlotGuestBusy succeeds (no competing owner ever moved State), msgSeq is
// intact and the response is correct. Together with
// TestGuestResponderFastPath_HostReclaimDuringHandlerIsRejected — which pins
// the opposite direction — this fixes the publish CAS as exactly discriminating
// "slot still mine" from "slot lost".
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
		t.Fatalf("MsgSeq=%d want 1 (the publish must land on the original transaction)", env.hdr.MsgSeq)
	}
	if env.hdr.RespSize != int32(len(payload)) {
		t.Fatalf("RespSize=%d want %d", env.hdr.RespSize, len(payload))
	}
	if got := string(env.respBuf[:env.hdr.RespSize]); got != string(payload) {
		t.Fatalf("echo mismatch: got %q want %q", got, payload)
	}
}

// TestGuestResponderFastPath_HostReclaimDuringHandlerIsRejected is the
// redefinition (v0.8.14) of the former ..._HostReclaimDuringHandlerCorrupts.
//
// The old test DOCUMENTED corruption: a host that abandons a slow handler and
// starts a new transaction on the slot used to receive the late responder's
// blind RESP_READY store, publishing txn#1's bytes under txn#2's msgSeq — a
// silent wrong value the msgSeq guard cannot catch. The 2026-07-24 verdict
// ("unreachable by a conforming host") was wrong: the reclaim modelled here is
// also what SlotAllocator::tryClaimSlot's zombie steal does after an ordinary
// response timeout, and that steal is NOT gated on auto-reclaim (see
// TestGuestResponderFastPath_HostTimeoutStealDoesNotCorrupt).
//
// Now that the responder publishes with CAS(SlotGuestBusy -> SlotRespReady),
// the assertion flips: the late publish must be REJECTED and the response
// dropped, and the new owner's transaction must survive untouched and be served
// correctly by the next loop iteration.
func TestGuestResponderFastPath_HostReclaimDuringHandlerIsRejected(t *testing.T) {
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

	// Host abandons the slow handler, reclaims the slot (State->FREE) and starts
	// a NEW transaction #2 (msgSeq 2, "BBBB") on it. The blind FREE store also
	// clobbers the responder's SlotGuestBusy claim — the harshest form of the
	// race, and exactly what the publish CAS has to survive.
	req2 := []byte("BBBB")
	atomic.StoreUint32(&env.hdr.State, SlotFree)
	copy(env.reqBuf, req2)
	env.hdr.ReqSize = int32(len(req2))
	env.hdr.MsgType = MsgTypeNormal
	env.hdr.MsgSeq = 2
	atomic.StoreUint32(&env.hdr.State, SlotReqReady)

	close(release) // handler#1 returns and attempts its (now guarded) publish

	if !waitForStateEq(env.hdr, SlotRespReady, 3*time.Second) {
		t.Fatal("txn#2 never received a response")
	}

	// The late publish CAS(SlotGuestBusy -> SlotRespReady) must have FAILED
	// (State was SlotReqReady, owned by txn#2), so txn#1's response is dropped.
	// The worker then picks txn#2 up normally and answers it.
	if env.hdr.MsgSeq != 2 {
		t.Fatalf("MsgSeq=%d want 2 (responder does not rewrite msgSeq)", env.hdr.MsgSeq)
	}
	got := string(env.respBuf[:env.hdr.RespSize])
	if got == string(req1) {
		t.Fatalf("CORRUPTION: stale txn#1 payload %q published under txn#2 msgSeq=2", req1)
	}
	if got != string(req2) {
		t.Fatalf("txn#2 response mismatch: got %q want %q", got, req2)
	}
}

// TestGuestResponderFastPath_HostTimeoutStealDoesNotCorrupt drives the REAL
// trigger for the blind-RESP_READY defect — no contract violation required.
//
// SPEC §3.4: on a response timeout the host disowns the slot WITHOUT storing
// any state (the transaction is still in flight). The slot is then recovered by
// SlotAllocator::tryClaimSlot's zombie steal, which is gated only on
// `!activeWait` — NOT on autoReclaimTimeoutNs — so it runs even under
// FastPathAllowed==1. This test replays that steal verbatim (snapshot Gen,
// observe State, CAS Gen, CAS State->SLOT_BUSY, republish) and then lets the
// late handler try to publish.
//
// Before v0.8.14 the responder blind-stored SlotRespReady and republished
// txn#1's bytes under txn#2's msgSeq. Note that a publish CAS expecting
// SlotReqReady would NOT have helped: the stolen slot is republished at
// SlotReqReady, so such a CAS still succeeds. Only the SlotGuestBusy claim —
// a state a stolen slot can never be back in — makes the publish airtight.
//
// SCOPE LIMIT (do not widen this test — add cases next to it instead): this
// case CANNOT observe msgType corruption. txn#1's handler return type and
// txn#2's request type are both MsgTypeNormal, and the assertions only look at
// RespSize and the response bytes, so a responder that writes header.MsgType
// before proving ownership passes here unnoticed. The header-field writes that
// precede the publish CAS are covered by
// TestGuestResponderSteal_NoHeaderWriteBeforeOwnership (msgType/respSize) and
// TestGuestResponderPanic_DoesNotStampErrorOnStolenSlot (panic path).
func TestGuestResponderFastPath_HostTimeoutStealDoesNotCorrupt(t *testing.T) {
	env := newFPHostSlotEnv(t, "FastPathTimeoutStealSHM")
	defer env.cleanup()

	release := make(chan struct{})
	entered := make(chan struct{})
	var firstCall int32
	var firstReq []byte
	env.client.Handle(func(req []byte, respBuf []byte, mt MsgType) (int32, MsgType) {
		if atomic.CompareAndSwapInt32(&firstCall, 0, 1) {
			firstReq = append([]byte(nil), req...)
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

	// Host publishes transaction #1 the way SlotAllocator does: gen bump, lease
	// stamp, then the REQ_READY release.
	req1 := []byte("AAAA")
	copy(env.reqBuf, req1)
	env.hdr.ReqSize = int32(len(req1))
	env.hdr.MsgType = MsgTypeNormal
	env.hdr.MsgSeq = 1
	atomic.AddUint64(&env.hdr.Gen, 1)
	atomic.StoreUint64(&env.hdr.Lease, MonotonicNanos())
	atomic.StoreUint32(&env.hdr.State, SlotReqReady)
	SignalEvent(env.hReq)

	<-entered // worker is inside handler(txn#1)

	// responseTimeout elapses: FinishWait(slot,false) drops activeWait and
	// leaves State untouched. Nothing is written to shared memory here.

	// The host now needs a slot and steals this abandoned one — tryClaimSlot's
	// zombie branch, replayed exactly.
	genObserved := atomic.LoadUint64(&env.hdr.Gen)
	observed := atomic.LoadUint32(&env.hdr.State)
	if observed == SlotFree || observed == SlotBusy {
		t.Fatalf("precondition: slot should be responder-owned, state=%d", observed)
	}
	if !atomic.CompareAndSwapUint64(&env.hdr.Gen, genObserved, genObserved+1) {
		t.Fatal("host steal: Gen CAS lost")
	}
	if !atomic.CompareAndSwapUint32(&env.hdr.State, observed, SlotBusy) {
		t.Fatalf("host steal: State CAS lost (observed=%d)", observed)
	}
	atomic.StoreUint64(&env.hdr.Lease, MonotonicNanos())

	// Transaction #2 on the recycled slot.
	req2 := []byte("BBBB")
	copy(env.reqBuf, req2)
	env.hdr.ReqSize = int32(len(req2))
	env.hdr.MsgType = MsgTypeNormal
	env.hdr.MsgSeq = 2
	atomic.StoreUint32(&env.hdr.State, SlotReqReady)
	SignalEvent(env.hReq)

	close(release) // handler#1 finally returns and attempts to publish

	if !waitForStateEq(env.hdr, SlotRespReady, 3*time.Second) {
		t.Fatal("txn#2 never received a response")
	}
	if env.hdr.MsgSeq != 2 {
		t.Fatalf("MsgSeq=%d want 2", env.hdr.MsgSeq)
	}
	got := string(env.respBuf[:env.hdr.RespSize])
	if got == string(req1) {
		t.Fatalf("CORRUPTION: txn#1 bytes %q published under txn#2 (msgSeq=2) after a timeout steal", req1)
	}
	if got != string(req2) {
		t.Fatalf("txn#2 response mismatch: got %q want %q", got, req2)
	}
}

// replayHostTimeoutSteal reproduces SlotAllocator::tryClaimSlot's zombie-steal
// branch verbatim on the test's slot: the host's response timeout fired (
// FinishWait(slot,false) dropped activeWait and left `state` untouched), so the
// host's next acquisition recycles the abandoned slot. Snapshot Gen, observe
// State, win the §3.6.1 gen CAS, then CAS State -> SlotBusy and restamp Lease.
// The slot is left claimed at SlotBusy, ready for the caller to publish a NEW
// transaction on it. The responder that is still running its handler has now
// definitively lost the slot.
func replayHostTimeoutSteal(t *testing.T, hdr *SlotHeader) {
	t.Helper()
	genObserved := atomic.LoadUint64(&hdr.Gen)
	observed := atomic.LoadUint32(&hdr.State)
	if observed != SlotGuestBusy {
		t.Fatalf("precondition: responder should own the slot at SlotGuestBusy, state=%d", observed)
	}
	if !atomic.CompareAndSwapUint64(&hdr.Gen, genObserved, genObserved+1) {
		t.Fatal("host steal: Gen CAS lost")
	}
	if !atomic.CompareAndSwapUint32(&hdr.State, observed, SlotBusy) {
		t.Fatalf("host steal: State CAS lost (observed=%d)", observed)
	}
	atomic.StoreUint64(&hdr.Lease, MonotonicNanos())
}

// TestGuestResponderSteal_NoHeaderWriteBeforeOwnership closes the hole
// TestGuestResponderFastPath_HostTimeoutStealDoesNotCorrupt structurally cannot
// see (see the SCOPE LIMIT note there).
//
// v0.8.14 sealed the *publish transition* (CAS SlotGuestBusy -> SlotRespReady)
// but left the header-field writes that precede it unconditional:
//
//	respSize, respType := handler(...)
//	header.RespSize = respSize   // <-- executed even after the slot was stolen
//	header.MsgType  = respType   // <-- ditto
//	publishResponse(...)         // CAS fails, response "dropped"
//
// After a host response timeout + zombie steal, those two stores land on the
// STEALER's brand-new transaction. The publish CAS then fails and the response
// bytes are correctly dropped — but the header is already poisoned. Because a
// host slot is serviced by exactly one worker goroutine, the very same goroutine
// then reads the poisoned header.MsgType in the next loop iteration and
// dispatches txn#2 to the WRONG handler branch. msgSeq is untouched (the
// responder never writes it), so the host's msgSeq guard validates and the
// caller silently gets a wrong value.
//
// Per SPEC §3.5's disown-terminal lesson, assert the individual acts, not just
// the final cell value: the request txn#2 is dispatched with must carry txn#2's
// own msgType/reqSize/msgSeq, and RespSize must still hold the sentinel the
// stealer left, proving the late responder wrote NOTHING.
func TestGuestResponderSteal_NoHeaderWriteBeforeOwnership(t *testing.T) {
	env := newFPHostSlotEnv(t, "FastPathStealHeaderWriteSHM")
	defer env.cleanup()

	const (
		reqType1  = MsgTypeAppStart + 1 // txn#1 request type
		respType1 = MsgTypeAppStart + 7 // txn#1 handler's RESPONSE type — the poison
		reqType2  = MsgTypeAppStart + 2 // txn#2 request type
	)
	// A value no participant would ever legitimately write, so seeing it proves
	// the late responder did not touch RespSize.
	const respSizeSentinel = int32(-31337)

	type dispatch struct {
		msgType  MsgType
		respSize int32
		msgSeq   uint32
		reqSize  int32
		req      string
	}
	second := make(chan dispatch, 4)

	release := make(chan struct{})
	entered := make(chan struct{})
	var firstCall int32
	var firstReq []byte
	env.client.Handle(func(req []byte, respBuf []byte, mt MsgType) (int32, MsgType) {
		if atomic.CompareAndSwapInt32(&firstCall, 0, 1) {
			firstReq = append([]byte(nil), req...)
			close(entered)
			<-release
			copy(respBuf, firstReq)
			return int32(len(firstReq)), respType1
		}
		// Snapshot the header EXACTLY as the worker handed this request over,
		// before anything writes a response for it.
		second <- dispatch{
			msgType:  mt,
			respSize: env.hdr.RespSize,
			msgSeq:   env.hdr.MsgSeq,
			reqSize:  env.hdr.ReqSize,
			req:      string(req),
		}
		n := copy(respBuf, req)
		return int32(n), mt
	})
	if err := env.client.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}

	// Host publishes transaction #1 the way SlotAllocator does.
	req1 := []byte("AAAA")
	copy(env.reqBuf, req1)
	env.hdr.ReqSize = int32(len(req1))
	env.hdr.MsgType = reqType1
	env.hdr.MsgSeq = 1
	atomic.AddUint64(&env.hdr.Gen, 1)
	atomic.StoreUint64(&env.hdr.Lease, MonotonicNanos())
	atomic.StoreUint32(&env.hdr.State, SlotReqReady)
	SignalEvent(env.hReq)

	<-entered // worker is inside handler(txn#1), blocked

	// responseTimeout elapses, then the host steals the abandoned slot.
	replayHostTimeoutSteal(t, env.hdr)

	// Transaction #2 on the recycled slot, with a DIFFERENT msgType so header
	// poisoning is observable at all.
	req2 := []byte("BBBB")
	copy(env.reqBuf, req2)
	env.hdr.ReqSize = int32(len(req2))
	env.hdr.MsgType = reqType2
	env.hdr.MsgSeq = 2
	env.hdr.RespSize = respSizeSentinel
	atomic.StoreUint32(&env.hdr.State, SlotReqReady)
	SignalEvent(env.hReq)

	close(release) // handler#1 returns; its late publish must write nothing

	var d dispatch
	select {
	case d = <-second:
	case <-time.After(3 * time.Second):
		t.Fatal("txn#2 was never dispatched to the handler")
	}

	if d.msgType != reqType2 {
		t.Errorf("txn#2 dispatched with msgType=%d, want %d: the late responder overwrote header.MsgType before proving ownership%s",
			d.msgType, reqType2, msgTypeHint(d.msgType, respType1))
	}
	if d.respSize != respSizeSentinel {
		t.Errorf("header.RespSize=%d at txn#2 dispatch, want sentinel %d: the late responder overwrote header.RespSize before proving ownership",
			d.respSize, respSizeSentinel)
	}
	if d.msgSeq != 2 {
		t.Errorf("header.MsgSeq=%d at txn#2 dispatch, want 2", d.msgSeq)
	}
	if d.reqSize != int32(len(req2)) {
		t.Errorf("header.ReqSize=%d at txn#2 dispatch, want %d", d.reqSize, len(req2))
	}
	if d.req != string(req2) {
		t.Errorf("txn#2 dispatched with req=%q, want %q", d.req, req2)
	}

	if !waitForStateEq(env.hdr, SlotRespReady, 3*time.Second) {
		t.Fatal("txn#2 never received a response")
	}
	if env.hdr.MsgSeq != 2 {
		t.Fatalf("MsgSeq=%d want 2 (responder does not rewrite msgSeq)", env.hdr.MsgSeq)
	}
	if env.hdr.MsgType != reqType2 {
		t.Fatalf("response MsgType=%d want %d (echo of txn#2's own type)", env.hdr.MsgType, reqType2)
	}
	if env.hdr.RespSize != int32(len(req2)) {
		t.Fatalf("response RespSize=%d want %d", env.hdr.RespSize, len(req2))
	}
	if got := string(env.respBuf[:env.hdr.RespSize]); got != string(req2) {
		t.Fatalf("txn#2 response mismatch: got %q want %q", got, req2)
	}
}

func msgTypeHint(got, poison MsgType) string {
	if got == poison {
		return " (it is txn#1's response type verbatim)"
	}
	return ""
}

// TestGuestResponderPanic_DoesNotStampErrorOnStolenSlot covers the panic-
// recovery mirror of the same defect.
//
// workerLoop's recover() used to accept `state == SlotReqReady` as "we still
// hold the slot" and stamp MsgType=SYSTEM_ERROR + RespSize=0 on it, publishing
// with CAS(observed -> SlotRespReady). But the worker CAS-claims
// SlotReqReady -> SlotGuestBusy BEFORE it touches anything, so observing
// SlotReqReady at panic time means the slot was stolen after a host response
// timeout and REPUBLISHED under a different transaction. The CAS then SUCCEEDS
// and completes, as a system error, a request that was never executed — and the
// handler is never invoked for it at all.
//
// Only SlotGuestBusy proves ownership, so that is the sole state the recovery
// may publish from.
func TestGuestResponderPanic_DoesNotStampErrorOnStolenSlot(t *testing.T) {
	env := newFPHostSlotEnv(t, "FastPathPanicStealSHM")
	defer env.cleanup()

	const reqType2 = MsgTypeAppStart + 2

	second := make(chan MsgType, 4)
	release := make(chan struct{})
	entered := make(chan struct{})
	var firstCall int32
	env.client.Handle(func(req []byte, respBuf []byte, mt MsgType) (int32, MsgType) {
		if atomic.CompareAndSwapInt32(&firstCall, 0, 1) {
			close(entered)
			<-release
			panic("simulated handler panic, raised after the slot was stolen")
		}
		second <- mt
		n := copy(respBuf, req)
		return int32(n), mt
	})
	if err := env.client.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}

	req1 := []byte("AAAA")
	copy(env.reqBuf, req1)
	env.hdr.ReqSize = int32(len(req1))
	env.hdr.MsgType = MsgTypeNormal
	env.hdr.MsgSeq = 1
	atomic.AddUint64(&env.hdr.Gen, 1)
	atomic.StoreUint64(&env.hdr.Lease, MonotonicNanos())
	atomic.StoreUint32(&env.hdr.State, SlotReqReady)
	SignalEvent(env.hReq)

	<-entered // worker is inside handler(txn#1), about to panic

	replayHostTimeoutSteal(t, env.hdr)

	req2 := []byte("BBBB")
	copy(env.reqBuf, req2)
	env.hdr.ReqSize = int32(len(req2))
	env.hdr.MsgType = reqType2
	env.hdr.MsgSeq = 2
	env.hdr.RespSize = -31337
	atomic.StoreUint32(&env.hdr.HostState, HostStateWaiting)
	atomic.StoreUint32(&env.hdr.State, SlotReqReady)
	SignalEvent(env.hReq)

	close(release) // handler#1 panics now

	// The panic recovery must leave txn#2 alone, so the restarted worker picks
	// it up and the handler runs for it. This is the load-bearing assertion:
	// a recovery that stamps SYSTEM_ERROR completes txn#2 without ever calling
	// the handler, so this channel stays empty.
	select {
	case mt := <-second:
		if mt != reqType2 {
			t.Fatalf("txn#2 dispatched with msgType=%d want %d", mt, reqType2)
		}
	case <-time.After(3 * time.Second):
		t.Fatalf("txn#2 was never dispatched: the panic recovery published a response (state=%d, MsgType=%d, RespSize=%d) for a request that never ran",
			atomic.LoadUint32(&env.hdr.State), env.hdr.MsgType, env.hdr.RespSize)
	}

	if !waitForStateEq(env.hdr, SlotRespReady, 3*time.Second) {
		t.Fatal("txn#2 never received a response")
	}
	if env.hdr.MsgType == MsgTypeSystemError {
		t.Fatalf("SYSTEM_ERROR was stamped on txn#2, which the panicking handler never processed")
	}
	if env.hdr.MsgSeq != 2 {
		t.Fatalf("MsgSeq=%d want 2", env.hdr.MsgSeq)
	}
	if env.hdr.RespSize != int32(len(req2)) {
		t.Fatalf("RespSize=%d want %d", env.hdr.RespSize, len(req2))
	}
	if got := string(env.respBuf[:env.hdr.RespSize]); got != string(req2) {
		t.Fatalf("txn#2 response mismatch: got %q want %q", got, req2)
	}
}

// TestGuestResponderTwoPhasePublish_RingsDoorbell is the no-regression guard for
// the two-phase publish (SlotGuestBusy -> SlotDone -> SlotRespReady): the
// transient SlotDone lock must not swallow the §4.2 doorbell. The mock host
// publishes hostState=WAITING and then parks on the response event ONLY — no
// state polling — so the test hangs if the publish stops signalling. It also
// pins that the host never has to know about SlotDone: it observes exactly
// SlotRespReady, as before.
func TestGuestResponderTwoPhasePublish_RingsDoorbell(t *testing.T) {
	env := newFPHostSlotEnv(t, "FastPathTwoPhaseDoorbellSHM")
	defer env.cleanup()

	env.client.Handle(func(req []byte, respBuf []byte, mt MsgType) (int32, MsgType) {
		n := copy(respBuf, req)
		return int32(n), mt
	})
	if err := env.client.Start(); err != nil {
		t.Fatalf("Start: %v", err)
	}

	for round := 0; round < 3; round++ {
		payload := []byte(fmt.Sprintf("ding-%d", round))
		copy(env.reqBuf, payload)
		env.hdr.ReqSize = int32(len(payload))
		env.hdr.MsgType = MsgTypeNormal
		env.hdr.MsgSeq = uint32(round + 1)
		env.hdr.RespSize = -1

		// Drain any latched signal so this round's waiter can only be woken by
		// this round's publish (the response event is auto-reset).
		WaitForEvent(env.hResp, 0)
		woke := make(chan struct{})
		go func() {
			WaitForEvent(env.hResp, 3000)
			close(woke)
		}()

		// Publish hostState=WAITING before the request so the responder is
		// guaranteed to observe it and take the signalling branch.
		atomic.StoreUint32(&env.hdr.HostState, HostStateWaiting)
		atomic.AddUint64(&env.hdr.Gen, 1)
		atomic.StoreUint64(&env.hdr.Lease, MonotonicNanos())
		atomic.StoreUint32(&env.hdr.State, SlotReqReady)
		SignalEvent(env.hReq)

		if !waitForStateEq(env.hdr, SlotRespReady, 3*time.Second) {
			t.Fatalf("round %d: no response (state=%d) — the two-phase publish never reached SlotRespReady",
				round, atomic.LoadUint32(&env.hdr.State))
		}
		select {
		case <-woke:
		case <-time.After(1500 * time.Millisecond):
			t.Fatalf("round %d: response published but the doorbell was never rung — the two-phase publish dropped the SetEvent", round)
		}
		atomic.StoreUint32(&env.hdr.HostState, HostStateActive)

		if env.hdr.RespSize != int32(len(payload)) {
			t.Fatalf("round %d: RespSize=%d want %d", round, env.hdr.RespSize, len(payload))
		}
		if got := string(env.respBuf[:env.hdr.RespSize]); got != string(payload) {
			t.Fatalf("round %d: echo mismatch: got %q want %q", round, got, payload)
		}
		atomic.StoreUint32(&env.hdr.State, SlotFree)
	}
}
