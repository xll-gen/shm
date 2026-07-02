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

// streamRejectHost polls the guest slots of a zombieStealEnv and answers each
// request with the respType chosen by decide(reqMsgType). It emulates the C++
// host's ProcessGuestCalls + StreamReassembler, whose rejections (unknown
// streamID, size overflow, OOM, eviction) surface as respType ==
// MsgTypeSystemError with NO transport error. Returns the count of StreamChunk
// requests it observed (for asserting StreamStart rejection short-circuits).
func streamRejectHost(env *zombieStealEnv, decide func(MsgType) MsgType, stop <-chan struct{}, done chan<- struct{}, chunkSeen *int32) {
	defer close(done)
	for {
		select {
		case <-stop:
			return
		default:
		}
		for k := 1; k <= env.numGuest; k++ {
			hdr := env.slotHeader(k)
			if atomic.LoadUint32(&hdr.State) != SlotReqReady {
				continue
			}
			if !atomic.CompareAndSwapUint32(&hdr.State, SlotReqReady, SlotBusy) {
				continue
			}
			reqType := hdr.MsgType
			if reqType == MsgTypeStreamChunk {
				atomic.AddInt32(chunkSeen, 1)
			}
			hdr.RespSize = 0
			hdr.MsgType = decide(reqType)
			atomic.StoreUint32(&hdr.State, SlotRespReady)
			SignalEvent(env.respEvents[k])
		}
		time.Sleep(100 * time.Microsecond)
	}
}

// TestStreamSender_StreamStartRejection_ReturnsError is the regression for
// 2026-07-02 MED #2 (StreamStart leg): StreamSender discarded slot.Send's
// respType, so a reassembler rejection (respType=MsgTypeSystemError, err=nil)
// was invisible and Send returned nil — a silent data loss. Worse, a rejected
// StreamStart did not stop the chunk phase, wasting slots on chunks the host
// would also reject. Post-fix Send returns an error immediately and sends no
// chunks.
func TestStreamSender_StreamStartRejection_ReturnsError(t *testing.T) {
	env := newZombieStealEnv(t, "StreamStartRejectSHM", 2)
	defer env.Close()

	client, err := ConnectDefault(env.name)
	if err != nil {
		t.Fatalf("ConnectDefault failed: %v", err)
	}
	defer client.Close()

	stop := make(chan struct{})
	done := make(chan struct{})
	var chunkSeen int32
	// Reject everything, including the StreamStart.
	go streamRejectHost(env, func(MsgType) MsgType { return MsgTypeSystemError }, stop, done, &chunkSeen)
	defer func() { close(stop); <-done }()

	sender := NewStreamSender(client, 2)
	data := make([]byte, 3000) // multiple chunks if the phase were reached

	if err := sender.Send(data, 777); err == nil {
		t.Fatal("StreamSender.Send returned nil despite host rejecting StreamStart with MsgTypeSystemError")
	}
	if n := atomic.LoadInt32(&chunkSeen); n != 0 {
		t.Fatalf("host saw %d chunk requests after a rejected StreamStart; want 0 (Send must short-circuit)", n)
	}
}

// TestStreamSender_ChunkRejection_ReturnsError is the regression for the chunk
// leg: the StreamStart is ACKed but a chunk is rejected with
// MsgTypeSystemError. Pre-fix the chunk goroutine dropped the respType and
// Send returned nil (silent loss). Post-fix the rejection is folded into the
// chunk goroutine's error and propagated via errChan.
func TestStreamSender_ChunkRejection_ReturnsError(t *testing.T) {
	env := newZombieStealEnv(t, "StreamChunkRejectSHM", 2)
	defer env.Close()

	client, err := ConnectDefault(env.name)
	if err != nil {
		t.Fatalf("ConnectDefault failed: %v", err)
	}
	defer client.Close()

	stop := make(chan struct{})
	done := make(chan struct{})
	var chunkSeen int32
	// ACK the StreamStart, reject every chunk.
	go streamRejectHost(env, func(reqType MsgType) MsgType {
		if reqType == MsgTypeStreamStart {
			return MsgTypeNormal
		}
		return MsgTypeSystemError
	}, stop, done, &chunkSeen)
	defer func() { close(stop); <-done }()

	sender := NewStreamSender(client, 2)
	data := make([]byte, 3000)

	if err := sender.Send(data, 778); err == nil {
		t.Fatal("StreamSender.Send returned nil despite host rejecting a chunk with MsgTypeSystemError")
	}
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
	if totalSize < 4096 {
		totalSize = 4096
	}

	hShm, addr, err := CreateShm(shmName, totalSize)
	if err != nil {
		t.Fatalf("CreateShm failed: %v", err)
	}
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
			case <-stopHost:
				return
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
	if err != nil {
		t.Fatalf("Connect: %v", err)
	}
	defer client.Close()

	sender := NewStreamSender(client, 1)
	err = sender.Send([]byte{}, 999)
	if err != nil {
		t.Fatalf("Send empty failed: %v", err)
	}

	close(stopHost)
	<-hostDone
}
