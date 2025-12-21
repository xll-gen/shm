package shm

import (
	"math"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

// TestGuestPanicOnIntMin verifies that the Guest handles INT_MIN ReqSize gracefully
// without panicking, and returns MsgTypeSystemError.
func TestGuestPanicOnIntMin(t *testing.T) {
	shmName := "TestGuestPanicOnIntMin"
	numSlots := uint32(1)
	slotSize := uint32(1024)

	// 1. Manually Initialize SHM (Simulate Host)
	// We use the internal createShm helper since we are in package shm.
	// Check cleanup
	UnlinkShm(shmName)
	defer UnlinkShm(shmName)

	headerSize := uint64(unsafe.Sizeof(ExchangeHeader{}))
	if headerSize < 64 { headerSize = 64 }
	slotHeaderSize := uint64(unsafe.Sizeof(SlotHeader{}))
	if slotHeaderSize < 128 { slotHeaderSize = 128 }

	totalSize := headerSize + slotHeaderSize + uint64(slotSize)

	// Note: createShm returns (ShmHandle, uintptr, error)
	h, addr, err := createShm(shmName, totalSize)
	if err != nil {
		t.Fatalf("Failed to create SHM: %v", err)
	}
	defer closeShm(h, addr, totalSize)

	// Initialize ExchangeHeader
	exHeader := (*ExchangeHeader)(unsafe.Pointer(addr))
	exHeader.Magic = Magic
	exHeader.Version = Version
	exHeader.NumSlots = numSlots
	exHeader.SlotSize = slotSize
	exHeader.ReqOffset = 0
	exHeader.RespOffset = slotSize / 2

	// Initialize SlotHeader
	slotHeaderAddr := addr + uintptr(headerSize)
	slotHeader := (*SlotHeader)(unsafe.Pointer(slotHeaderAddr))
	atomic.StoreUint32(&slotHeader.State, SlotFree)
	atomic.StoreUint32(&slotHeader.HostState, HostStateActive)
	atomic.StoreUint32(&slotHeader.GuestState, GuestStateActive)

	// Create Events
	reqName := shmName + "_slot_0"
	respName := shmName + "_slot_0_resp"

	// Unlink previous just in case
	unlinkEvent(reqName)
	unlinkEvent(respName)
	defer unlinkEvent(reqName)
	defer unlinkEvent(respName)

	hReq, err := createEvent(reqName)
	if err != nil {
		t.Fatalf("Failed to create Req event: %v", err)
	}
	defer closeEvent(hReq)

	hResp, err := createEvent(respName)
	if err != nil {
		t.Fatalf("Failed to create Resp event: %v", err)
	}
	defer closeEvent(hResp)

	// 2. Start Guest
	guest, err := NewDirectGuest(shmName)
	if err != nil {
		t.Fatalf("Failed to start Guest: %v", err)
	}
	defer guest.Close()

	// Dummy Handler
	handler := func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
		// Should not be called for invalid size
		return 0, MsgTypeNormal
	}
	guest.Start(handler)

	// 3. Simulate Malicious Host
	// Set ReqSize to INT_MIN
	slotHeader.ReqSize = math.MinInt32
	slotHeader.MsgType = MsgTypeNormal

	// Signal Guest
	atomic.StoreUint32(&slotHeader.State, SlotReqReady)
	signalEvent(hReq)

	// 4. Wait for Response
	// We wait on hResp. Guest should signal it after handling error.

	// Simple wait loop (or use waitForEvent if exported? It is not exported, lowercase)
	// But we are in package shm, so we can use it.
	// Wait up to 2 seconds
	start := time.Now()
	signaled := false
	for time.Since(start) < 2*time.Second {
		// We can't easily check if event is signaled without consuming it.
		// waitForEvent blocks.
		// Since we expect a signal, we can just block with timeout.
		// Wait 100ms at a time
		// NOTE: waitForEvent implementation varies by OS.
		// Linux: sem_timedwait.
		waitForEvent(hResp, 100)

		// Check if State changed to SlotRespReady
		state := atomic.LoadUint32(&slotHeader.State)
		if state == SlotRespReady {
			signaled = true
			break
		}
	}

	if !signaled {
		t.Fatalf("Timeout waiting for Guest response")
	}

	// 5. Verify Response
	respType := slotHeader.MsgType
	respSize := slotHeader.RespSize

	t.Logf("Got Response: Type=%d, Size=%d", respType, respSize)

	if respType != MsgTypeSystemError {
		t.Errorf("Expected MsgTypeSystemError (%d), got %d. (Implies Panic Recovery occurred instead of clean rejection)", MsgTypeSystemError, respType)
	}
}
