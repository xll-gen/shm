package shm

import (
	"fmt"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

func TestGuestStream(t *testing.T) {
	shmName := "GuestStreamTestSHM"
	UnlinkShm(shmName)
	UnlinkEvent(fmt.Sprintf("%s_slot_0", shmName))
	UnlinkEvent(fmt.Sprintf("%s_slot_0_resp", shmName))
	UnlinkEvent(fmt.Sprintf("%s_guest_call", shmName))
	UnlinkEvent(fmt.Sprintf("%s_slot_1_resp", shmName))
	UnlinkEvent(fmt.Sprintf("%s_slot_2_resp", shmName)) // 2 Guest slots for pipelining

	// 1. Create SHM (Act as Host)
	// 1 Host Slot, 2 Guest Slots
	totalSize := uint64(64 + 3*(128+1024)) // 1 host + 2 guest
	if totalSize < 4096 {
		totalSize = 4096
	}

	hShm, addr, err := CreateShm(shmName, totalSize)
	if err != nil {
		t.Fatalf("Failed to create SHM: %v", err)
	}
	defer func() {
		CloseShm(hShm, addr, totalSize)
		UnlinkShm(shmName)
	}()

	exHeader := (*ExchangeHeader)(unsafe.Pointer(addr))
	exHeader.Magic = Magic
	exHeader.Version = Version
	exHeader.NumSlots = 1
	exHeader.NumGuestSlots = 2
	exHeader.SlotSize = 1024
	exHeader.ReqOffset = 0
	exHeader.RespOffset = 512

	// Create Events
	reqEvName0 := fmt.Sprintf("%s_slot_0", shmName)
	respEvName0 := fmt.Sprintf("%s_slot_0_resp", shmName)
	hReq0, _ := CreateEvent(reqEvName0)
	hResp0, _ := CreateEvent(respEvName0)
	defer func() {
		CloseEvent(hReq0)
		UnlinkEvent(reqEvName0)
		CloseEvent(hResp0)
		UnlinkEvent(respEvName0)
	}()

	// Guest Call Event
	reqEvName := fmt.Sprintf("%s_guest_call", shmName)
	hReq, err := CreateEvent(reqEvName)
	if err != nil {
		t.Fatalf("CreateEvent req failed: %v", err)
	}
	defer func() {
		CloseEvent(hReq)
		UnlinkEvent(reqEvName)
	}()

	// Guest Slot Response Events (Index 1 and 2)
	guestRespEvents := make([]EventHandle, 2)
	for i := 0; i < 2; i++ {
		rName := fmt.Sprintf("%s_slot_%d_resp", shmName, 1+i)
		ev, err := CreateEvent(rName)
		if err != nil {
			t.Fatalf("CreateEvent failed: %v", err)
		}
		guestRespEvents[i] = ev
		defer func(name string, h EventHandle) {
			CloseEvent(h)
			UnlinkEvent(name)
		}(rName, ev)
	}

	// Mock Host Logic
	stopHost := make(chan struct{})
	hostDone := make(chan struct{})

	go func() {
		defer close(hostDone)
		// Poll Guest Slots (1 and 2)
		slot1Ptr := addr + uintptr(64+1*(128+1024)) // Slot 1
		slot2Ptr := addr + uintptr(64+2*(128+1024)) // Slot 2

		slots := []*SlotHeader{
			(*SlotHeader)(unsafe.Pointer(slot1Ptr)),
			(*SlotHeader)(unsafe.Pointer(slot2Ptr)),
		}

		start := time.Now()
		for {
			select {
			case <-stopHost:
				return
			default:
			}

			if time.Since(start) > 5*time.Second {
				fmt.Printf("MockHost: Timed out\n")
				return
			}

			for i, slotHeader := range slots {
				state := atomic.LoadUint32(&slotHeader.State)
				if state == SlotReqReady {
					// Just ACK
					slotHeader.RespSize = 0
					slotHeader.MsgType = MsgTypeNormal
					atomic.StoreUint32(&slotHeader.State, SlotRespReady)
					SignalEvent(guestRespEvents[i])
				}
			}
			time.Sleep(1 * time.Millisecond)
		}
	}()

	// 2. Guest Logic
	client, err := ConnectDefault(shmName)
	if err != nil {
		t.Fatalf("ConnectDefault failed: %v", err)
	}

	sender := NewStreamSender(client, 2)

	// Create large data
	dataSize := 5000 // > 1024, so multiple chunks
	data := make([]byte, dataSize)
	for i := range data {
		data[i] = byte(i % 255)
	}

	streamID := uint64(12345)

	startT := time.Now()
	err = sender.Send(data, streamID)
	if err != nil {
		t.Fatalf("Sender.Send failed: %v", err)
	}
	elapsed := time.Since(startT)
	t.Logf("Sent %d bytes in %v", dataSize, elapsed)

	// Clean up
	client.Close()
	close(stopHost)
	<-hostDone
}

func TestGuestStreamEmpty(t *testing.T) {
	shmName := "GuestStreamEmptySHM"
	UnlinkShm(shmName)
	UnlinkEvent(fmt.Sprintf("%s_slot_0", shmName))
	UnlinkEvent(fmt.Sprintf("%s_slot_0_resp", shmName))
	UnlinkEvent(fmt.Sprintf("%s_guest_call", shmName))
	UnlinkEvent(fmt.Sprintf("%s_slot_1_resp", shmName))

	// 1 Host, 1 Guest
	totalSize := uint64(64 + 2*(128+1024))
	if totalSize < 4096 { totalSize = 4096 }

	hShm, addr, err := CreateShm(shmName, totalSize)
	if err != nil { t.Fatalf("CreateShm failed: %v", err) }
	defer func() {
		CloseShm(hShm, addr, totalSize)
		UnlinkShm(shmName)
	}()

	exHeader := (*ExchangeHeader)(unsafe.Pointer(addr))
	exHeader.Magic = Magic
	exHeader.Version = Version
	exHeader.NumSlots = 1
	exHeader.NumGuestSlots = 1
	exHeader.SlotSize = 1024
	exHeader.ReqOffset = 0
	exHeader.RespOffset = 512

	// Events
	reqEvName0 := fmt.Sprintf("%s_slot_0", shmName)
	respEvName0 := fmt.Sprintf("%s_slot_0_resp", shmName)
	hReq0, _ := CreateEvent(reqEvName0)
	hResp0, _ := CreateEvent(respEvName0)
	defer func() { CloseEvent(hReq0); UnlinkEvent(reqEvName0); CloseEvent(hResp0); UnlinkEvent(respEvName0) }()

	reqEvName := fmt.Sprintf("%s_guest_call", shmName)
	hReq, _ := CreateEvent(reqEvName)
	defer func() { CloseEvent(hReq); UnlinkEvent(reqEvName) }()

	rName := fmt.Sprintf("%s_slot_1_resp", shmName)
	ev, _ := CreateEvent(rName)
	defer func() { CloseEvent(ev); UnlinkEvent(rName) }()

	// Mock Host
	stopHost := make(chan struct{})
	hostDone := make(chan struct{})

	go func() {
		defer close(hostDone)
		slot1Ptr := addr + uintptr(64+1*(128+1024))
		slotHeader := (*SlotHeader)(unsafe.Pointer(slot1Ptr))

		for {
			select {
			case <-stopHost: return
			default:
			}

			if atomic.LoadUint32(&slotHeader.State) == SlotReqReady {
				 slotHeader.RespSize = 0
				 slotHeader.MsgType = MsgTypeNormal
				 atomic.StoreUint32(&slotHeader.State, SlotRespReady)
				 SignalEvent(ev)
			}
			time.Sleep(1 * time.Millisecond)
		}
	}()

	client, err := ConnectDefault(shmName)
	if err != nil { t.Fatalf("Connect: %v", err) }
	defer client.Close()

	sender := NewStreamSender(client, 1)
	err = sender.Send([]byte{}, 999)
	if err != nil { t.Fatalf("Send empty failed: %v", err) }

	close(stopHost)
	<-hostDone
}
