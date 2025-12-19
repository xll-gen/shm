package shm

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"sync"
	"sync/atomic"
	"testing"
	"time"
	"unsafe"
)

func setupHost(t *testing.T, name string, numSlots, slotSize int) (int, func()) {
	headerSize := 64
	perSlotSize := 128 + slotSize
	totalSize := headerSize + numSlots*perSlotSize

	h, addr, err := CreateShm(name, uint64(totalSize))
	if err != nil {
		t.Fatalf("Failed to create shm: %v", err)
	}

	// Initialize ExchangeHeader
	header := (*ExchangeHeader)(unsafe.Pointer(addr))
	header.Magic = Magic
	header.Version = Version
	header.NumSlots = uint32(numSlots)
	header.SlotSize = uint32(slotSize)
	header.ReqOffset = 0
	header.RespOffset = uint32(slotSize / 2)

	// Initialize Slots
	ptr := addr + uintptr(headerSize)
	for i := 0; i < numSlots; i++ {
		slot := (*SlotHeader)(unsafe.Pointer(ptr))
		atomic.StoreUint32(&slot.State, SlotFree)

        hReq, _ := CreateEvent(fmt.Sprintf("%s_slot_%d", name, i))
        hResp, _ := CreateEvent(fmt.Sprintf("%s_slot_%d_resp", name, i))
        CloseEvent(hReq)
        CloseEvent(hResp)

		ptr += uintptr(perSlotSize)
	}

	return totalSize, func() {
		CloseShm(h, addr, uint64(totalSize))
		UnlinkShm(name)
        // Unlink events too
        for i := 0; i < numSlots; i++ {
             UnlinkEvent(fmt.Sprintf("%s_slot_%d", name, i))
             UnlinkEvent(fmt.Sprintf("%s_slot_%d_resp", name, i))
        }
	}
}

func TestStreaming(t *testing.T) {
	shmName := "TestStreamGo"
	numSlots := 4
	slotSize := 1024

	totalSize, cleanup := setupHost(t, shmName, numSlots, slotSize)
	defer cleanup()

	guest, err := NewDirectGuest(shmName)
	if err != nil {
		t.Fatalf("Failed to connect guest: %v", err)
	}
	defer guest.Close()

	var receivedData []byte
	var mu sync.Mutex
	done := make(chan struct{})

	streamHandler := func(streamID uint64, data []byte) {
		mu.Lock()
		receivedData = data
		mu.Unlock()
		close(done)
	}

	handler := NewStreamReassembler(streamHandler, nil)
	guest.Start(handler)

	// Open SHM again to act as Host
	_, addr, err := OpenShm(shmName, uint64(totalSize))
	if err != nil {
		t.Fatalf("Failed to open shm as host: %v", err)
	}

	headerSize := uint64(64)
	perSlotSize := uint64(128 + slotSize)

	writeSlot := func(slotIdx int, typeVal MsgType, payload []byte) {
		offset := headerSize + uint64(slotIdx)*perSlotSize
		ptr := addr + uintptr(offset)

		slotHeader := (*SlotHeader)(unsafe.Pointer(ptr))
		reqBuf := unsafe.Pointer(ptr + uintptr(128)) // 128 = SlotHeader size

		// Spin until FREE or RESP_READY (Ack)
		for {
			state := atomic.LoadUint32(&slotHeader.State)
			if state == SlotFree {
				break
			}
			if state == SlotRespReady {
				atomic.StoreUint32(&slotHeader.State, SlotFree)
				break
			}
			time.Sleep(time.Millisecond)
		}

		target := unsafe.Slice((*byte)(reqBuf), len(payload))
		copy(target, payload)

		slotHeader.ReqSize = int32(len(payload))
		slotHeader.MsgType = typeVal
		atomic.StoreUint32(&slotHeader.State, SlotReqReady)

        // Signal
        // Since we are in same process, guest.slots has events open.
        // Host signals ReqEvent.
        SignalEvent(guest.slots[slotIdx].reqEvent)
	}

	dataSize := 100 * 1024 // 100KB
	data := make([]byte, dataSize)
	for i := range data {
		data[i] = byte(i % 256)
	}
	streamID := uint64(999)

    // Calculate chunks
	chunkHeaderSize := int(unsafe.Sizeof(ChunkHeader{}))
	maxPayload := (slotSize / 2) - chunkHeaderSize
	totalChunks := (dataSize + maxPayload - 1) / maxPayload

	streamHead := StreamHeader{
		StreamID:    streamID,
		TotalSize:   uint64(dataSize),
		TotalChunks: uint32(totalChunks),
	}

	buf := new(bytes.Buffer)
	binary.Write(buf, binary.LittleEndian, streamHead)
	writeSlot(0, MsgTypeStreamStart, buf.Bytes())

	for i := 0; i < totalChunks; i++ {
		start := i * maxPayload
		end := start + maxPayload
		if end > dataSize {
			end = dataSize
		}
		payload := data[start:end]

		ch := ChunkHeader{
			StreamID:    streamID,
			ChunkIndex:  uint32(i),
			PayloadSize: uint32(len(payload)),
		}

		buf := new(bytes.Buffer)
		binary.Write(buf, binary.LittleEndian, ch)
		buf.Write(payload)

		writeSlot(i%numSlots, MsgTypeStreamChunk, buf.Bytes())
	}

	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("Timeout waiting for stream")
	}

    mu.Lock()
    defer mu.Unlock()
	if !bytes.Equal(receivedData, data) {
		t.Fatal("Data mismatch")
	}
}
