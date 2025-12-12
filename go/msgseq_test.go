package shm

import (
	"fmt"
	"testing"
	"time"
	"unsafe"
    "sync/atomic"
)

// MockHostForMsgSeq simulates a malicious/buggy host that corrupts MsgSeq
func MockHostForMsgSeq(t *testing.T, name string, numSlots int) {
	// Connect to SHM (assume already created by Guest)
	// We need to wait for Guest to create it.
	// But in NewDirectGuest, it expects SHM to exist.
	// So we must create SHM first.

    // Actually, NewDirectGuest expects *Host* to have created SHM.
    // So we need to create SHM here.

    shmName := name
    totalSize := 1024*1024 // arbitrary large enough
    hMap, addr, err := CreateShm(shmName, uint64(totalSize))
    if err != nil {
        t.Errorf("Failed to create SHM: %v", err)
        return
    }
    defer CloseShm(hMap, addr, uint64(totalSize))

    // Init Header
    header := (*ExchangeHeader)(unsafe.Pointer(addr))
    header.Magic = Magic
    header.Version = Version
    header.NumSlots = uint32(numSlots)
    header.NumGuestSlots = 1
    header.SlotSize = 1024
    header.ReqOffset = 0
    header.RespOffset = 512

    // Init Slot 0 (Host Slot) - Not used for Guest Call
    // Init Slot 1 (Guest Slot)
    // Slot 0 at offset 64 + 128 = 192
    // Slot 1 at offset 192 + 128 + 1024 = 1344

    // Calculate offsets correctly
    headerSize := uint64(unsafe.Sizeof(ExchangeHeader{}))
    if headerSize < 64 { headerSize = 64 }
    slotHeaderSize := uint64(unsafe.Sizeof(SlotHeader{}))
    if slotHeaderSize < 128 { slotHeaderSize = 128 }
    perSlot := slotHeaderSize + uint64(header.SlotSize)

    // Guest Slot is at index numSlots
    guestSlotOffset := headerSize + (perSlot * uint64(numSlots))
    guestSlotHeader := (*SlotHeader)(unsafe.Pointer(addr + uintptr(guestSlotOffset)))

    // Create Events
    reqName := fmt.Sprintf("%s_guest_call", name)
    respName := fmt.Sprintf("%s_slot_%d_resp", name, numSlots)

    hReq, _ := CreateEvent(reqName)
    hResp, _ := CreateEvent(respName)
    defer CloseEvent(hReq)
    defer CloseEvent(hResp)

    // Loop to process one request
    for {
        // Wait for REQ_READY
        if atomic.LoadUint32(&guestSlotHeader.State) == SlotReqReady {
             // Read MsgSeq
             // Corrupt it
             guestSlotHeader.MsgSeq = 999999

             // Signal RESP_READY
             atomic.StoreUint32(&guestSlotHeader.State, SlotRespReady)
             SignalEvent(hResp)
             return // Done
        }
        time.Sleep(1 * time.Millisecond)
    }
}

func TestGuestCallMsgSeqValidation(t *testing.T) {
    name := "MsgSeqTest"

    go MockHostForMsgSeq(t, name, 1)

    time.Sleep(100 * time.Millisecond) // Wait for Host Init

    g, err := NewDirectGuest(name)
    if err != nil {
        t.Fatalf("Failed to connect: %v", err)
    }
    defer g.Close()

    data := []byte("test")
    _, err = g.SendGuestCall(data, MsgTypeNormal)

    if err == nil {
        t.Fatal("Expected error due to MsgSeq mismatch, got nil")
    }

    expected := "msgSeq mismatch"
    if len(err.Error()) < len(expected) || err.Error()[:len(expected)] != expected {
        t.Errorf("Expected error starting with %q, got %q", expected, err.Error())
    }
}
