package shm

import (
	"fmt"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

// TestGuestCallNegativeRespSize reproduces the bug where negative RespSize (end aligned data)
// is ignored by DirectGuest.SendGuestCall.
func TestGuestCallNegativeRespSize(t *testing.T) {
	shmName := "BugReproSHM"
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

    // Mock Host Logic
    go func() {
        slotOffset := 64 + 128 + 1024
        slotHeaderPtr := addr + uintptr(slotOffset)
        slotHeader := (*SlotHeader)(unsafe.Pointer(slotHeaderPtr))

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

        respData := []byte("Success")
        respLen := len(respData)
        dataBase := slotHeaderPtr + 128
        respBase := dataBase + 512
        writeOffset := 512 - respLen
        destPtr := respBase + uintptr(writeOffset)
        destSlice := unsafe.Slice((*byte)(unsafe.Pointer(destPtr)), respLen)
        copy(destSlice, respData)

        slotHeader.RespSize = -int32(respLen)
        slotHeader.MsgType = MsgTypeNormal
        atomic.StoreUint32(&slotHeader.State, SlotRespReady)
        SignalEvent(hResp)
    }()

    // 2. Guest Logic
    guest, err := NewDirectGuest(shmName, 0, 0)
    if err != nil {
        t.Fatalf("NewDirectGuest failed: %v", err)
    }
    defer guest.Close()

    resp, err := guest.SendGuestCall([]byte("Test"), MsgTypeGuestCall)
    if err != nil {
        t.Fatalf("SendGuestCall failed: %v", err)
    }

    expected := "Success"
    if string(resp) != expected {
        t.Fatalf("Mismatch! Expected '%s', got '%s'", expected, string(resp))
    }
}

// TestSpuriousWakeup verifies that SendGuestCall correctly handles
// a spurious wakeup (semaphore signaled before we wait).
func TestSpuriousWakeup(t *testing.T) {
	shmName := "BugReproSHM_Race"
    UnlinkShm(shmName)
    UnlinkEvent(fmt.Sprintf("%s_slot_1", shmName))
    UnlinkEvent(fmt.Sprintf("%s_slot_1_resp", shmName))

    totalSize := uint64(64 + 2 * (128 + 1024))

    hShm, addr, err := CreateShm(shmName, totalSize)
    if err != nil {
        t.Fatalf("Failed to create SHM: %v", err)
    }
    defer func() {
        CloseShm(hShm, addr, totalSize)
        UnlinkShm(shmName)
    }()

    exHeader := (*ExchangeHeader)(unsafe.Pointer(addr))
    exHeader.NumSlots = 1
    exHeader.NumGuestSlots = 1
    exHeader.SlotSize = 1024
    exHeader.ReqOffset = 0
    exHeader.RespOffset = 512

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

    // Poison the Semaphore!
    SignalEvent(hResp)

    go func() {
        slotOffset := 64 + 128 + 1024
        slotHeaderPtr := addr + uintptr(slotOffset)
        slotHeader := (*SlotHeader)(unsafe.Pointer(slotHeaderPtr))

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

        time.Sleep(100 * time.Millisecond)

        slotHeader.RespSize = 0
        slotHeader.MsgType = MsgTypeNormal
        atomic.StoreUint32(&slotHeader.State, SlotRespReady)
        SignalEvent(hResp)
    }()

    guest, err := NewDirectGuest(shmName, 0, 0)
    if err != nil {
        t.Fatalf("NewDirectGuest failed: %v", err)
    }
    defer guest.Close()

    _, err = guest.SendGuestCall([]byte("Test"), MsgTypeGuestCall)
    if err != nil {
        t.Fatalf("SendGuestCall failed (reproduced bug): %v", err)
    }
}
