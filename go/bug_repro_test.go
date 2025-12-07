package shm

import (
	"fmt"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

// This test reproduces the bug where negative RespSize (end aligned data)
// is ignored by DirectGuest.SendGuestCall.
func TestGuestCallNegativeRespSize(t *testing.T) {
	shmName := "BugReproSHM"
    // Cleanup previous runs (best effort)
    UnlinkShm(shmName)
    UnlinkEvent(fmt.Sprintf("%s_slot_1", shmName))
    UnlinkEvent(fmt.Sprintf("%s_slot_1_resp", shmName))

    // 1. Create SHM (Act as Host)
    // 64 header + 1 Host Slot (header+data) + 1 Guest Slot (header+data)
    // SlotHeader = 128
    // SlotSize = 1024
    // PerSlot = 128 + 1024 = 1152
    // Total = 64 + 2 * 1152 = 2368
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
    // Slot 1 (Guest Slot)
    reqEvName := fmt.Sprintf("%s_slot_1", shmName)
    respEvName := fmt.Sprintf("%s_slot_1_resp", shmName)

    // We create them so Guest can open them
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

    // Mock Host Logic
    go func() {
        // Calculate Slot 1 Header Address
        // Offset = 64 (ExHeader) + 1 * (128 + 1024) = 64 + 1152 = 1216
        slotOffset := 64 + 128 + 1024
        slotHeaderPtr := addr + uintptr(slotOffset)
        slotHeader := (*SlotHeader)(unsafe.Pointer(slotHeaderPtr))

        // Wait for SLOT_REQ_READY
        // Simple spin with timeout
        start := time.Now()
        for {
            state := atomic.LoadUint32(&slotHeader.State)
            if state == SlotReqReady {
                break
            }
            if time.Since(start) > 5*time.Second {
                fmt.Printf("MockHost: Timed out waiting for REQ\n")
                return
            }
            time.Sleep(1 * time.Millisecond)
        }

        // Process Request (Ignored)
        // Write Response "Success" at END of buffer
        // Buffer starts at slotHeaderPtr + 128
        // SlotSize = 1024
        // RespOffset = 512
        // Actually, RespBuffer is from 512 to 1024 (size 512).

        respData := []byte("Success")
        respLen := len(respData)

        // Calculate End address
        // Base of slot data = slotHeaderPtr + 128
        // Resp starts at Base + 512
        // Resp end at Base + 1024

        dataBase := slotHeaderPtr + 128
        respBase := dataBase + 512

        // We write to end of resp buffer.
        // Size of resp buffer = 512.

        writeOffset := 512 - respLen
        destPtr := respBase + uintptr(writeOffset)

        // Write data
        destSlice := unsafe.Slice((*byte)(unsafe.Pointer(destPtr)), respLen)
        copy(destSlice, respData)

        // Set Header
        slotHeader.RespSize = -int32(respLen)
        slotHeader.MsgType = MsgTypeNormal // Use Normal or whatever

        // Signal Ready
        atomic.StoreUint32(&slotHeader.State, SlotRespReady)
        SignalEvent(hResp)
    }()

    // 2. Guest Logic
    guest, err := NewDirectGuest(shmName, 0, 0)
    if err != nil {
        t.Fatalf("NewDirectGuest failed: %v", err)
    }
    defer guest.Close()

    // Send Guest Call
    resp, err := guest.SendGuestCall([]byte("Test"), MsgTypeGuestCall)
    if err != nil {
        t.Fatalf("SendGuestCall failed: %v", err)
    }

    // Verify
    expected := "Success"
    if string(resp) != expected {
        t.Fatalf("Mismatch! Expected '%s', got '%s'. This proves the bug.", expected, string(resp))
    }
}
