package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"runtime"
	"runtime/pprof"
	"sync"
	"time"

	flatbuffers "github.com/google/flatbuffers/go"
	"benchmark/ipc" // Using local generated code
	"github.com/xll-gen/shm/go" // Changed import path to match module
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

    // Select Mode
    var clientMode shm.Mode
    if *mode == "direct" {
        clientMode = shm.ModeDirect
    } else {
        clientMode = shm.ModeQueue
    }

    // Connect
    fmt.Println("[Go] Connecting to Host...")
    client, err := shm.Connect("SimpleIPC", clientMode)
    if err != nil {
        log.Fatalf("Failed to connect: %v", err)
    }
    fmt.Println("[Go] Connected.")

    // Pool for builders
    pool := sync.Pool{
        New: func() interface{} {
            return flatbuffers.NewBuilder(1024)
        },
    }

    // Handler
    client.Handle(func(reqData []byte) []byte {
        builder := pool.Get().(*flatbuffers.Builder)
        builder.Reset()
        defer pool.Put(builder)

        msg := ipc.GetRootAsMessage(reqData, 0)
        reqID := msg.ReqId() // App-level ID (optional now)

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
        return builder.FinishedBytes()
    })

    // Start (Blocking)
    go client.Start()

    // Wait
    client.Wait()
    fmt.Println("[Go] Received Shutdown. Exiting...")
    client.Close()

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
