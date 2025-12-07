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

// TestWorkerNegativeResp verifies that DirectGuest worker loop correctly handles
// negative RespSize returned by the handler by moving the data to the end of the buffer.
func TestWorkerNegativeResp(t *testing.T) {
	shmName := "WorkerBugSHM"
    UnlinkShm(shmName)
    // Clean up potentially leaked events
    UnlinkEvent(fmt.Sprintf("%s_slot_0", shmName))
    UnlinkEvent(fmt.Sprintf("%s_slot_0_resp", shmName))

    // 1. Create SHM (Act as Host)
    // Header (64) + 1 Slot (128 + 1024)
    totalSize := uint64(64 + 1 * (128 + 1024))

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
    exHeader.NumGuestSlots = 0
    exHeader.SlotSize = 1024
    exHeader.ReqOffset = 0
    exHeader.RespOffset = 512

    // Create Events
    reqEvName := fmt.Sprintf("%s_slot_0", shmName)
    respEvName := fmt.Sprintf("%s_slot_0_resp", shmName)

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

    // 2. Start Guest
    guest, err := NewDirectGuest(shmName, 0, 0)
    if err != nil {
        t.Fatalf("NewDirectGuest failed: %v", err)
    }
    defer guest.Close()

    // Handler that returns negative size (Zero Copy request)
    // It writes to the START of the buffer, expecting the framework to move it.
    handler := func(req []byte, resp []byte, msgType MsgType) (int32, MsgType) {
        payload := []byte("ZeroCopyData")
        copy(resp, payload)
        return -int32(len(payload)), MsgTypeFlatbuffer
    }

    guest.Start(handler)

    // 3. Host Logic (Send Request)
    slotOffset := 64 // Header is 64
    slotHeaderPtr := addr + uintptr(slotOffset)
    slotHeader := (*SlotHeader)(unsafe.Pointer(slotHeaderPtr))

    // Set BUSY
    atomic.StoreUint32(&slotHeader.State, SlotBusy)

    // Write Request (Empty is fine)
    slotHeader.ReqSize = 0
    slotHeader.MsgType = MsgTypeNormal
    slotHeader.MsgSeq = 1

    // Signal Ready
    atomic.StoreUint32(&slotHeader.State, SlotReqReady)
    SignalEvent(hReq)

    // Wait for Response
    start := time.Now()
    for {
        state := atomic.LoadUint32(&slotHeader.State)
        if state == SlotRespReady {
            break
        }
        if time.Since(start) > 2*time.Second {
            t.Fatalf("Timed out waiting for Guest Response")
        }
        time.Sleep(1 * time.Millisecond)
    }

    // 4. Verification
    respSize := slotHeader.RespSize
    if respSize >= 0 {
        t.Fatalf("Expected negative respSize, got %d", respSize)
    }

    absRespSize := -respSize
    expectedData := []byte("ZeroCopyData")
    if int(absRespSize) != len(expectedData) {
        t.Fatalf("Expected size %d, got %d", len(expectedData), absRespSize)
    }

    // Check where the data is
    // It SHOULD be at the end of the response buffer (Offset 1024)
    // RespBuffer starts at 512. MaxRespSize = 512.
    // End is at 512 + 512 = 1024 (relative to slot start)
    // Data should be at [1024 - len]

    // Calculate pointer
    dataBase := slotHeaderPtr + 128
    respBase := dataBase + 512
    maxRespSize := 512

    offset := maxRespSize - int(absRespSize)
    destPtr := respBase + uintptr(offset)
    destSlice := unsafe.Slice((*byte)(unsafe.Pointer(destPtr)), absRespSize)

    destString := string(destSlice)
    if destString != string(expectedData) {
        // Debug: check start of buffer
        startSlice := unsafe.Slice((*byte)(unsafe.Pointer(respBase)), absRespSize)
        startString := string(startSlice)
        t.Fatalf("Data mismatch at End-Aligned position.\nExpected: '%s'\nGot:      '%s'\n(At Start: '%s')", expectedData, destString, startString)
    }

    // Cleanup done by defers
}
