package main

import (
	"flag"
	"fmt"
	"os"
	"runtime"
	"sync"
	"sync/atomic"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"
	"benchmark/ipc" // Using local generated code
	"github.com/xll-gen/shm/go" // Changed import path to match module
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
		hToGuest, err = shm.OpenEvent("SimpleIPC_event_req") // REQ = ToGuest
		if err == nil {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}

	for {
		hFromGuest, err = shm.OpenEvent("SimpleIPC_event_resp") // RESP = FromGuest
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

	fmt.Println("[Go] Guest Ready. Waiting for requests from Host...")

	// Init IPCGuest
	client := shm.NewIPCGuest(toGuestQueue, fromGuestQueue)

	// 4. Start Workers
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

	// 5. Start Reader (handles normal, heartbeat, shutdown)
	client.StartReader(func(data []byte) {
		// Copy data because IPCGuest reuses/frees buffer?
		// SPSCQueue Dequeue returns allocated buffer.
		// IPCGuest.StartReader just passes it.
		// So it's safe to pass to channel.
		workChan <- data
	})

	// Wait for Shutdown
	client.Wait()

	fmt.Println("[Go] Received Shutdown. Exiting...")
	close(workChan)
	wg.Wait()
	_ = hMap
	os.Exit(0)
}

func handleRequest(data []byte, client *shm.IPCGuest, builder *flatbuffers.Builder) {
	defer func() {
		if r := recover(); r != nil {
			// Recover from potential FlatBuffers panics due to corrupt data
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

	// Use IPCGuest to send (uses spinlock and signal-before-lock opt)
	client.SendBytes(resBytes)
}
