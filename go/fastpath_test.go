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
