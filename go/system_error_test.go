package shm

import (
	"fmt"
	"testing"
	"time"
    "unsafe"
    "sync/atomic"
)

func TestSystemErrorReal(t *testing.T) {
    name := "TestSystemErrorGo"
    shmSize := uint64(4096)

    // 1. Create SHM file manually (simulate Host)
    h, addr, err := createShm(name, shmSize)
    if err != nil {
        t.Fatalf("Failed to create shm: %v", err)
    }
    defer closeShm(h, addr, shmSize)
    defer unlinkShm(name)

    // Initialize Header
    header := (*ExchangeHeader)(unsafe.Pointer(addr))
    header.Magic = Magic
    header.Version = Version
    header.NumSlots = 1
    header.NumGuestSlots = 1
    header.SlotSize = 1024
    header.ReqOffset = 0
    header.RespOffset = 512

    // Create Events for Slot 0 (Host Slot)
    reqName0 := fmt.Sprintf("%s_slot_0", name)
    respName0 := fmt.Sprintf("%s_slot_0_resp", name)
    reqEv0, _ := createEvent(reqName0)
    respEv0, _ := createEvent(respName0)
    defer closeEvent(reqEv0)
    defer closeEvent(respEv0)
    defer unlinkEvent(reqName0)
    defer unlinkEvent(respName0)

    // Create Events for Slot 1 (Guest Slot)
    reqName1 := fmt.Sprintf("%s_guest_call", name) // Shared
    respName1 := fmt.Sprintf("%s_slot_1_resp", name)
    reqEv1, _ := createEvent(reqName1)
    respEv1, _ := createEvent(respName1)
    defer closeEvent(reqEv1)
    defer closeEvent(respEv1)
    defer unlinkEvent(reqName1) // reqName1 is same as guest_call
    defer unlinkEvent(respName1)

    // 2. Connect Guest
    g, err := NewDirectGuest(name)
    if err != nil {
        t.Fatalf("NewDirectGuest failed: %v", err)
    }
    defer g.Close()

    // 3. Simulate Host Processing Loop
    // Host monitors Guest Slot (Index 1)
    // Address calc: Header (64) + Slot0(Header+Data) = 64 + 128 + 1024 = 1216.
    slot1Header := (*SlotHeader)(unsafe.Pointer(addr + 1216))

    go func() {
        // Wait for Request
        for {
            state := atomic.LoadUint32(&slot1Header.State)
            if state == SlotReqReady {
                 // Reject
                 slot1Header.RespSize = 0
                 slot1Header.MsgType = MsgTypeSystemError
                 atomic.StoreUint32(&slot1Header.State, SlotRespReady)
                 // Signal Response Event (Host would do this)
                 signalEvent(respEv1)
                 return
            }
            time.Sleep(1 * time.Millisecond)
        }
    }()

    // Call SendGuestCall
    data := []byte("Trigger Error")
    _, err = g.SendGuestCall(data, MsgTypeNormal)

    if err == nil {
        t.Fatalf("Expected error, got nil")
    }

    expected := "system error: host rejected request (likely buffer overflow)"
    if err.Error() != expected {
        t.Fatalf("Unexpected error message: got '%v', want '%v'", err, expected)
    }
}
