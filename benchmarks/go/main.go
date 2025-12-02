package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"runtime"
	"runtime/pprof"
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
	workers := flag.Int("w", 1, "Number of worker threads (lanes)")
	mode := flag.String("mode", "spsc", "Queue mode: spsc or mpsc")
	cpuprofile := flag.String("cpuprofile", "", "write cpu profile to file")
	memprofile := flag.String("memprofile", "", "write memory profile to file")
	flag.Parse()

	if *cpuprofile != "" {
		f, err := os.Create(*cpuprofile)
		if err != nil {
			log.Fatal(err)
		}
		pprof.StartCPUProfile(f)
		defer pprof.StopCPUProfile()
	}

	fmt.Printf("[Go] Starting Guest with %d lanes in %s mode...\n", *workers, *mode)

	// 1. Calculate SHM size
	// Each lane: ReqQ + RespQ
	qRequiredSize := uint64(shm.QueueHeaderSize + QUEUE_SIZE)
	totalSize := uint64(*workers) * qRequiredSize * 2

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

	// 2. Initialize Queues and run Guest
	if *mode == "mpsc" {
		runMPSC(addr, *workers)
	} else {
		runSPSC(addr, *workers)
	}

	_ = hMap

	if *memprofile != "" {
		f, err := os.Create(*memprofile)
		if err != nil {
			log.Fatal("could not create memory profile: ", err)
		}
		defer f.Close()
		runtime.GC()
		if err := pprof.WriteHeapProfile(f); err != nil {
			log.Fatal("could not write memory profile: ", err)
		}
	}
}

func runSPSC(addr uintptr, lanes int) {
	// Wait for events and init queues
	// Retry loop for events
	var reqQs, respQs []*shm.LockedSPSCQueue

	for {
		// Try to open all lanes
		rQs, wQs, err := shm.OpenLanes("SimpleIPC", lanes, QUEUE_SIZE, addr, 0,
			func(base uintptr, size uint64, h shm.EventHandle) *shm.LockedSPSCQueue {
				return shm.NewLockedSPSCQueue(base, size, h)
			},
		)
		if err == nil {
			reqQs = rQs
			respQs = wQs
			break
		}
		time.Sleep(100 * time.Millisecond)
	}

	fmt.Println("[Go] Connected to Events and Queues.")

	// Wait for Host to init memory (check capacity of first queue)
	// Actually OpenLanes just wraps pointers. Capacity check needed.
	fmt.Println("[Go] Waiting for Queue initialization...")
	for atomic.LoadUint64(&reqQs[0].Header.Capacity) == 0 {
		runtime.Gosched()
		time.Sleep(10 * time.Millisecond)
	}

	client := shm.NewIPCGuest[*shm.LockedSPSCQueue](reqQs, respQs)
	runGuest(client)
}

func runMPSC(addr uintptr, lanes int) {
	var reqQs, respQs []*shm.MPSCQueue

	for {
		rQs, wQs, err := shm.OpenLanes("SimpleIPC", lanes, QUEUE_SIZE, addr, 0,
			func(base uintptr, size uint64, h shm.EventHandle) *shm.MPSCQueue {
				return shm.NewMPSCQueue(base, size, h)
			},
		)
		if err == nil {
			reqQs = rQs
			respQs = wQs
			break
		}
		time.Sleep(100 * time.Millisecond)
	}

	fmt.Println("[Go] Connected to Events and Queues.")

	fmt.Println("[Go] Waiting for Queue initialization...")
	for atomic.LoadUint64(&reqQs[0].Header.Capacity) == 0 {
		runtime.Gosched()
		time.Sleep(10 * time.Millisecond)
	}

	client := shm.NewIPCGuest[*shm.MPSCQueue](reqQs, respQs)
	runGuest(client)
}

// runGuest is generic
func runGuest[Q shm.IPCQueue](client *shm.IPCGuest[Q]) {
	fmt.Println("[Go] Guest Ready. Waiting for requests from Host...")

	// Create a pool of builders to avoid allocation?
	// Or just one builder per lane?
	// StartWorkers spawns one goroutine per lane.
	// We can use a sync.Pool for builders.

	builderPool := sync.Pool{
		New: func() any {
			return flatbuffers.NewBuilder(1024)
		},
	}

	client.StartWorkers(func(data []byte, laneID int) {
		builder := builderPool.Get().(*flatbuffers.Builder)
		handleRequest(data, client, builder, laneID)
		builderPool.Put(builder)
	})

	client.Wait()
	fmt.Println("[Go] Received Shutdown. Exiting...")
}

func handleRequest[Q shm.IPCQueue](data []byte, client *shm.IPCGuest[Q], builder *flatbuffers.Builder, laneID int) {
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

	client.SendToLane(resBytes, laneID)
}
