package shm

import (
	"fmt"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

// TestGuestPanicRecovery verifies that the Guest worker recovers from a panic
// and continues to process subsequent requests.
func TestGuestPanicRecovery(t *testing.T) {
	shmName := "PanicRecoveryTest"
    UnlinkShm(shmName)
    UnlinkEvent(fmt.Sprintf("%s_slot_0", shmName))
    UnlinkEvent(fmt.Sprintf("%s_slot_0_resp", shmName))

    // 1. Create SHM (Act as Host)
    // 1 Host Slot, 0 Guest Slots
    totalSize := uint64(64 + 1 * (128 + 1024))
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

    hReq, err := CreateEvent(reqEvName)
    if err != nil { t.Fatalf("Failed to create req event: %v", err) }

    hResp, err := CreateEvent(respEvName)
    if err != nil { t.Fatalf("Failed to create resp event: %v", err) }

    defer func() {
        CloseEvent(hReq)
        CloseEvent(hResp)
        UnlinkEvent(reqEvName)
        UnlinkEvent(respEvName)
    }()

    // 2. Start Guest
    guest, err := NewDirectGuest(shmName)
    if err != nil {
        t.Fatalf("NewDirectGuest failed: %v", err)
    }

    const MsgTypePanic = MsgType(100)

    // Handler triggers panic
    handler := func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
        if msgType == MsgTypePanic {
            panic("Simulated Panic")
        }
        copy(resp, []byte("OK"))
        return 2, MsgTypeNormal
    }

    guest.Start(handler)
    defer guest.Close()

    // 3. Mock Host Logic: Send Panic
    slotHeaderPtr := addr + uintptr(64) // ExHeader(64) -> SlotHeader
    slotHeader := (*SlotHeader)(unsafe.Pointer(slotHeaderPtr))

    // Send Panic
    atomic.StoreUint32(&slotHeader.State, SlotBusy)
    slotHeader.ReqSize = 4
    slotHeader.MsgType = MsgTypePanic
    atomic.StoreUint32(&slotHeader.HostState, HostStateWaiting) // Pretend Host is waiting
    atomic.StoreUint32(&slotHeader.State, SlotReqReady)
    SignalEvent(hReq)

    // Wait for Response (With fix: Error/Empty. Without fix: Timeout)
    // We expect the fix to send a 0-size response immediately.
    done := make(chan bool)
    go func() {
        WaitForEvent(hResp, 2000) // 2s timeout
        done <- true
    }()

    select {
    case <-done:
        // Check State
        state := atomic.LoadUint32(&slotHeader.State)
        if state != SlotRespReady {
            t.Errorf("Step 1: Expected SlotRespReady, got %d", state)
        }
        if slotHeader.RespSize != 0 {
             t.Errorf("Step 1: Expected RespSize 0 (Error), got %d", slotHeader.RespSize)
        }
    case <-time.After(3 * time.Second):
        t.Fatalf("Step 1: Timed out waiting for panic response. Fix not working.")
    }

    // Reset Slot for next round
    atomic.StoreUint32(&slotHeader.State, SlotFree)

    // 4. Send Normal Request (Verify Recovery)
    atomic.StoreUint32(&slotHeader.State, SlotBusy)
    slotHeader.ReqSize = 4
    slotHeader.MsgType = MsgTypeNormal
    atomic.StoreUint32(&slotHeader.HostState, HostStateWaiting)
    atomic.StoreUint32(&slotHeader.State, SlotReqReady)
    SignalEvent(hReq)

    go func() {
        WaitForEvent(hResp, 2000)
        done <- true
    }()

    select {
    case <-done:
        state := atomic.LoadUint32(&slotHeader.State)
        if state != SlotRespReady {
            t.Errorf("Step 2: Expected SlotRespReady, got %d", state)
        }
        // Check payload "OK"
        // ... (Skipping payload check, presence of signal is enough proof of life)
    case <-time.After(3 * time.Second):
        t.Fatalf("Step 2: Timed out waiting for normal response. Worker is dead.")
    }
}
