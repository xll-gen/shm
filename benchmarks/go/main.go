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
	workers := flag.Int("w", 1, "Number of worker threads")
	mode := flag.String("mode", "spsc", "Queue mode: spsc, mpsc, or direct")
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

	fmt.Printf("[Go] Starting Guest with %d workers in %s mode...\n", *workers, *mode)

    if *mode == "direct" {
        runDirect(*workers)
    } else {
	// 1. Open SHM (Retry loop until Host creates it)
	// We assume Host creates sufficient space.
	// For MPSC/SPSC, header sizes are same (128 bytes).
	qTotalSize := uint64(shm.QueueHeaderSize + QUEUE_SIZE)
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

	runSPSC(addr, qTotalSize, hToGuest, hFromGuest, *workers)
    _ = hMap
    }

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

func runDirect(workers int) {
    // Retry loop for SHM
    var guest *shm.DirectGuest
    var err error

    fmt.Println("[Go] Waiting for Host to create SHM (Direct Mode)...")
    for {
        guest, err = shm.NewDirectGuest("SimpleIPC", workers, 1024*1024)
        if err == nil {
            break
        }
        time.Sleep(100 * time.Millisecond)
    }
    fmt.Println("[Go] Connected to Direct Exchange.")

    pool := sync.Pool{
        New: func() interface{} {
            return flatbuffers.NewBuilder(1024)
        },
    }

    handler := func(reqData []byte) []byte {
        defer func() {
            if r := recover(); r != nil {
                fmt.Printf("[Go] Panic in handler: %v\n", r)
            }
        }()

        builder := pool.Get().(*flatbuffers.Builder)
        builder.Reset()
        defer pool.Put(builder)

        msg := ipc.GetRootAsMessage(reqData, 0)
        reqID := msg.ReqId()

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
        // ... (other cases if needed)
        }

        ipc.MessageStart(builder)
        ipc.MessageAddReqId(builder, reqID)
        ipc.MessageAddPayloadType(builder, payloadType)
        ipc.MessageAddPayload(builder, payloadOffset)
        resMsg := ipc.MessageEnd(builder)

        builder.Finish(resMsg)
        return builder.FinishedBytes()
    }

    guest.Start(handler)

    // Wait for shutdown
    guest.Wait()
    fmt.Println("[Go] Received Shutdown. Exiting...")
    guest.Close()
}


func runSPSC(addr uintptr, qTotalSize uint64, hToGuest, hFromGuest shm.EventHandle, workers int) {
	toGuestQueue := shm.NewLockedSPSCQueue(addr, QUEUE_SIZE, hToGuest)
	fromGuestQueue := shm.NewLockedSPSCQueue(addr+uintptr(qTotalSize), QUEUE_SIZE, hFromGuest)

	fmt.Println("[Go] Waiting for Queue initialization...")
	for atomic.LoadUint64(&toGuestQueue.Header.Capacity) == 0 {
		runtime.Gosched()
		time.Sleep(10 * time.Millisecond)
	}

	client := shm.NewIPCGuest[*shm.LockedSPSCQueue](toGuestQueue, fromGuestQueue)
	runGuest(client, workers)
}


// runGuest is generic
func runGuest[Q shm.IPCQueue](client *shm.IPCGuest[Q], workers int) {
	fmt.Println("[Go] Guest Ready. Waiting for requests from Host...")

	workChan := make(chan []byte, 1024)
	var wg sync.WaitGroup

	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			builder := flatbuffers.NewBuilder(1024)
			for data := range workChan {
				handleRequest(data, client, builder)
			}
		}(i)
	}

	client.StartReader(func(data []byte) {
		workChan <- data
	})

	client.Wait()
	fmt.Println("[Go] Received Shutdown. Exiting...")
	close(workChan)
	wg.Wait()
}

func handleRequest[Q shm.IPCQueue](data []byte, client *shm.IPCGuest[Q], builder *flatbuffers.Builder) {
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

	client.SendBytes(resBytes)
}
