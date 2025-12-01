package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"
	"benchmark/ipc" // Using local generated code
	"xll-gen/shm/go" // Changed import path to match module
)

const (
	QUEUE_SIZE = 1024 * 1024 * 4 // 4MB
)

func main() {
	workers := flag.Int("w", 1, "Number of worker threads")
	flag.Parse()

	fmt.Printf("[Go] Starting Server with %d workers...\n", *workers)

	// 1. Create Events
	hReq, err := shm.CreateEvent("SimpleIPC_REQ")
	if err != nil {
		fmt.Printf("CreateEvent REQ error: %v\n", err)
	}
	hResp, err := shm.CreateEvent("SimpleIPC_RESP")
	if err != nil {
		fmt.Printf("CreateEvent RESP error: %v\n", err)
	}

	// 2. Open SHM
	qTotalSize := uint64(shm.QueueHeaderSize + QUEUE_SIZE) // Header + Data
	totalSize := uint64(qTotalSize * 2)

	hMap, addr, err := shm.CreateShm("SimpleIPC", totalSize)
	if err != nil {
		panic(err)
	}

	// 3. Init Queues
	reqQueue := shm.NewMPSCQueue(addr, QUEUE_SIZE, hReq)
	respQueue := shm.NewMPSCQueue(addr+uintptr(qTotalSize), QUEUE_SIZE, hResp)

	fmt.Println("[Go] Waiting for SHM initialization...")
	for atomic.LoadUint64(&reqQueue.Header.Capacity) == 0 {
		runtime.Gosched()
		time.Sleep(10 * time.Millisecond)
	}

	fmt.Println("[Go] Server Ready. Waiting for requests...")

	// 4. Start Workers
	workChan := make(chan []byte, 1024)

	var wg sync.WaitGroup
	for i := 0; i < *workers; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			builder := flatbuffers.NewBuilder(1024)
			for data := range workChan {
				handleRequest(data, respQueue, builder)
			}
		}(i)
	}

	// 5. Consumer Loop
	for {
		data, ok := reqQueue.Dequeue()
		if ok {
			workChan <- data
			continue
		}

		spinCount := 0
		for {
			if atomic.LoadUint64(&reqQueue.Header.WritePos) != atomic.LoadUint64(&reqQueue.Header.ReadPos) {
				break
			}
			spinCount++
			if spinCount > 4000 {
				shm.WaitForEvent(hReq, 100)
				break
			}
			runtime.Gosched()
		}
	}

	_ = hMap
}

func handleRequest(data []byte, respQ *shm.MPSCQueue, builder *flatbuffers.Builder) {
	defer func() {
		if r := recover(); r != nil {
			// Recover from potential FlatBuffers panics due to corrupt data
		}
	}()

	if len(data) < 8 {
		return
	}

	reqID := binary.LittleEndian.Uint64(data[:8])
	payload := data[8:]

	msg := ipc.GetRootAsMessage(payload, 0)

	builder.Reset()

	var payloadOffset flatbuffers.UOffsetT
	var payloadType ipc.Payload

	// This access might panic if payload is corrupt
	pt := msg.PayloadType()

	switch pt {
	case ipc.PayloadAddRequest:
		req := new(ipc.AddRequest)
		var t flatbuffers.Table
		if msg.Payload(&t) {
			req.Init(t.Bytes, t.Pos)
			res := req.X() + req.Y()

			ipc.AddResponseStart(builder)
			ipc.AddResponseAddResult(builder, res)
			payloadOffset = ipc.AddResponseEnd(builder)
			payloadType = ipc.PayloadAddResponse
		}

	case ipc.PayloadMyRandRequest:
		ipc.MyRandResponseStart(builder)
		ipc.MyRandResponseAddResult(builder, 0.12345)
		payloadOffset = ipc.MyRandResponseEnd(builder)
		payloadType = ipc.PayloadMyRandResponse
	}

	ipc.MessageStart(builder)
	ipc.MessageAddReqId(builder, 0)
	ipc.MessageAddPayloadType(builder, payloadType)
	ipc.MessageAddPayload(builder, payloadOffset)
	resMsg := ipc.MessageEnd(builder)

	builder.Finish(resMsg)
	resBytes := builder.FinishedBytes()

	respPacket := make([]byte, 8+len(resBytes))
	binary.LittleEndian.PutUint64(respPacket[:8], reqID)
	copy(respPacket[8:], resBytes)

	if !respQ.Enqueue(respPacket) {
		for !respQ.Enqueue(respPacket) {
			runtime.Gosched()
		}
	}
}
