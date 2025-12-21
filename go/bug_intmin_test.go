package shm

import (
	"fmt"
	"math"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

// TestWorkerPanicOnIntMin verifies that the Guest worker handles
// INT_MIN ReqSize from Host without panicking, and returns SystemError.
func TestWorkerPanicOnIntMin(t *testing.T) {
	shmName := "BugIntMinSHM"
    UnlinkShm(shmName)
    UnlinkEvent(fmt.Sprintf("%s_slot_0", shmName))
    UnlinkEvent(fmt.Sprintf("%s_slot_0_resp", shmName))
    UnlinkEvent(fmt.Sprintf("%s_guest_call", shmName))

    // 1. Create SHM (Act as Host)
    totalSize := uint64(64 + 1 * (128 + 1024)) // 1 Slot
    if totalSize < 4096 { totalSize = 4096 }

    hShm, addr, err := CreateShm(shmName, totalSize)
    if err != nil {
        t.Fatalf("Failed to create SHM: %v", err)
    }
    defer func() {
        CloseShm(hShm, addr, totalSize)
        UnlinkShm(shmName)
    }()

    // Init Exchange Header
    exHeader := (*ExchangeHeader)(unsafe.Pointer(addr))
    exHeader.Magic = Magic
    exHeader.Version = Version
    exHeader.NumSlots = 1
    exHeader.NumGuestSlots = 0
    exHeader.SlotSize = 1024
    exHeader.ReqOffset = 0
    exHeader.RespOffset = 512

    // Create Events
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

    // 2. Start Guest
	guest, err := NewDirectGuest(shmName)
    if err != nil {
        t.Fatalf("NewDirectGuest failed: %v", err)
    }
    defer guest.Close()

    handlerCalled := int32(0)
    guest.Start(func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
        atomic.StoreInt32(&handlerCalled, 1)
        return 0, MsgTypeNormal
    })

    // 3. Mock Malicious Host
    slotHeaderPtr := addr + 64 // ExchangeHeader is 64 bytes
    slotHeader := (*SlotHeader)(unsafe.Pointer(slotHeaderPtr))

    // Send INT_MIN ReqSize
    slotHeader.ReqSize = math.MinInt32
    slotHeader.MsgType = MsgTypeNormal
    // Use a unique sequence to verify
    slotHeader.MsgSeq = 123
    slotHeader.HostState = HostStateWaiting

    atomic.StoreUint32(&slotHeader.State, SlotReqReady)
    SignalEvent(hReq)

    // 4. Wait for Response
    // If Guest panics (and recovers), it sets RespSize=0.
    // If Guest handles gracefully (Fix), it sets MsgType=SystemError.
    // If Guest crashes hard, we time out.

    start := time.Now()
    success := false
    for time.Since(start) < 2*time.Second {
        state := atomic.LoadUint32(&slotHeader.State)
        if state == SlotRespReady {
            success = true
            break
        }
        time.Sleep(10 * time.Millisecond)
    }

    if !success {
        t.Fatalf("Timed out waiting for Guest response. Guest likely hung or crashed.")
    }

    // Verify
    if atomic.LoadInt32(&handlerCalled) == 1 {
        t.Fatalf("Handler was called! It should have been skipped due to invalid size.")
    }

    respType := slotHeader.MsgType
    if respType != MsgTypeSystemError {
        t.Errorf("Expected MsgTypeSystemError (%d), got %d. (Did panic recovery kick in instead of graceful handling?)", MsgTypeSystemError, respType)
    }
}
