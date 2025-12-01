package main

import (
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
	QUEUE_SIZE = 1024 * 1024 * 32 // 32MB
)

func main() {
	workers := flag.Int("w", 1, "Number of worker threads")
	flag.Parse()

	fmt.Printf("[Go] Starting Guest with %d workers...\n", *workers)

	// 1. Open SHM (Retry loop until Host creates it)
	qTotalSize := uint64(shm.QueueHeaderSize + QUEUE_SIZE) // Header + Data
	totalSize := uint64(qTotalSize * 2)

	var hMap shm.ShmHandle
	var addr uintptr
	var err error

	fmt.Println("[Go] Waiting for Host to create SHM...")
	for {
		hMap, addr, err = shm.OpenShm("SimpleIPC", totalSize)
		if err == nil {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	fmt.Println("[Go] Connected to SHM.")

	// 2. Open Events (Retry loop)
	var hToGuest, hFromGuest shm.EventHandle

	for {
		hToGuest, err = shm.OpenEvent("SimpleIPC_REQ") // REQ = ToGuest
		if err == nil {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}

	for {
		hFromGuest, err = shm.OpenEvent("SimpleIPC_RESP") // RESP = FromGuest
		if err == nil {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	fmt.Println("[Go] Connected to Events.")

	// 3. Init Queues
	// Layout: [ToGuestQueue] [FromGuestQueue]
	// Queue 1: ToGuest (Guest reads from here)
	// Queue 2: FromGuest (Guest writes to here)
	toGuestQueue := shm.NewSPSCQueue(addr, QUEUE_SIZE, hToGuest)
	fromGuestQueue := shm.NewSPSCQueue(addr+uintptr(qTotalSize), QUEUE_SIZE, hFromGuest)

	fmt.Println("[Go] Waiting for Queue initialization...")
	// Wait for Host to initialize headers
	for atomic.LoadUint64(&toGuestQueue.Header.Capacity) == 0 {
		runtime.Gosched()
		time.Sleep(10 * time.Millisecond)
	}

	// 4. Init IPC Client
	client := shm.NewIPCClient(toGuestQueue, fromGuestQueue)
	client.Start()
	defer client.Stop()

	fmt.Println("[Go] Guest Ready. Waiting for requests from Host...")

	// 5. Start Workers
	workChan := make(chan []byte, 1024)

	var wg sync.WaitGroup
	for i := 0; i < *workers; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			builder := flatbuffers.NewBuilder(1024)
			for data := range workChan {
				handleRequest(data, client, builder)
			}
		}(i)
	}

	// 6. Consumer Loop (Reading from toGuestQueue)
	// We can use client.StartReader if we refactor, but strict loop here is fine too.
	// But `client.StartReader` runs in background.
	// To keep `main` blocking, we can use `StartReader` and then wait?
	// Or just loop here manually calling Dequeue (as Client is mainly for Sending).
	// Let's loop manually to read from toGuestQueue and push to workers.

	// Actually, if we use Client.StartReader, we need to pass a callback.
	// client.StartReader(func(data []byte) { workChan <- data })
	// select {}

	for {
		data := toGuestQueue.Dequeue()
		workChan <- data
	}

	_ = hMap
}

func handleRequest(data []byte, client *shm.IPCClient, builder *flatbuffers.Builder) {
	defer func() {
		if r := recover(); r != nil {
			// Recover
		}
	}()

	msg := ipc.GetRootAsMessage(data, 0)
	reqID := msg.ReqId()

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
	ipc.MessageAddReqId(builder, reqID)
	ipc.MessageAddPayloadType(builder, payloadType)
	ipc.MessageAddPayload(builder, payloadOffset)
	resMsg := ipc.MessageEnd(builder)

	builder.Finish(resMsg)
	resBytes := builder.FinishedBytes()

	// Get buffer from pool and copy
	// Because FlatBuffers builder reuses its internal buffer, we must copy out.
	// `resBytes` is a slice of builder's buffer.

	bufPtr := client.GetBuffer()

	// Resize bufPtr if needed
	if cap(*bufPtr) < len(resBytes) {
		*bufPtr = make([]byte, len(resBytes))
	} else {
		*bufPtr = (*bufPtr)[:len(resBytes)]
	}
	copy(*bufPtr, resBytes)

	// Send (Client takes ownership and returns to pool)
	client.Send(bufPtr)
}
