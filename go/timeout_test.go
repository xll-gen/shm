package shm

import (
	"fmt"
	"testing"
	"time"
	"unsafe"
)

func TestGuestCallTimeoutConfig(t *testing.T) {
	shmName := "TestTimeoutSHM"
    UnlinkShm(shmName)
    UnlinkEvent(fmt.Sprintf("%s_slot_1", shmName))
    UnlinkEvent(fmt.Sprintf("%s_slot_1_resp", shmName))

    // 1. Create SHM (Act as Host)
    totalSize := uint64(64 + 2 * (128 + 1024))

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
    exHeader.NumSlots = 1
    exHeader.NumGuestSlots = 1
    exHeader.SlotSize = 1024
    exHeader.ReqOffset = 0
    exHeader.RespOffset = 512

    // Create Events
    reqEvName := fmt.Sprintf("%s_slot_1", shmName)
    respEvName := fmt.Sprintf("%s_slot_1_resp", shmName)

    hReq, err := CreateEvent(reqEvName)
    if err != nil { t.Fatalf("CreateEvent req failed: %v", err) }
    defer func() {
        CloseEvent(hReq)
        UnlinkEvent(reqEvName)
    }()

    hResp, err := CreateEvent(respEvName)
    if err != nil { t.Fatalf("CreateEvent resp failed: %v", err) }
    defer func() {
        CloseEvent(hResp)
        UnlinkEvent(respEvName)
    }()

    // 2. Guest Logic
    guest, err := NewDirectGuest(shmName, 0, 0)
    if err != nil {
        t.Fatalf("NewDirectGuest failed: %v", err)
    }
    defer guest.Close()

    // Set short timeout (e.g., 500ms)
    timeout := 500 * time.Millisecond
    guest.SetTimeout(timeout)

    start := time.Now()
    _, err = guest.SendGuestCall([]byte("Test"), MsgTypeGuestCall)
    duration := time.Since(start)

    if err == nil {
        t.Fatalf("Expected timeout error, got nil")
    }

    // Expect duration approx 500ms. Allow margin.
    if duration < 400*time.Millisecond || duration > 1000*time.Millisecond {
        t.Errorf("Expected duration ~500ms, got %v", duration)
    }
}
