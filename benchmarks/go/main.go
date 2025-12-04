package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"log"
	"math"
	"os"
	"runtime"
	"runtime/pprof"

	"github.com/xll-gen/shm/go"
)

// Define protocol constants to match C++
const (
	ReqSize  = 24
	RespSize = 16
)

func main() {
	workers := flag.Int("w", 1, "Number of worker threads")
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

	fmt.Printf("[Go] Starting Guest with %d workers (Direct Mode)...\n", *workers)

	// Connect
	fmt.Println("[Go] Connecting to Host...")
	client, err := shm.Connect("SimpleIPC")
	if err != nil {
		log.Fatalf("Failed to connect: %v", err)
	}
	fmt.Println("[Go] Connected.")

	// Handler - Zero Copy
	client.Handle(func(reqData []byte, respBuf []byte) int {
		// Expect TransportHeader (8 bytes) + ReqSize
		if len(reqData) < 8+ReqSize {
			return 0
		}

		// Extract Transport Header (req_id)
		reqIdHeader := reqData[0:8]

		// Parse Request (Little Endian) - Skip 8 bytes header
		payload := reqData[8:]
		// C++: struct { int64_t id; double x; double y; }
		id := int64(binary.LittleEndian.Uint64(payload[0:8]))
		x := math.Float64frombits(binary.LittleEndian.Uint64(payload[8:16]))
		y := math.Float64frombits(binary.LittleEndian.Uint64(payload[16:24]))

		// Process
		res := x + y

		// Prepare Response: Header (8) + Resp (16)
		if len(respBuf) < 8+16 {
			return 0
		}

		copy(respBuf[0:8], reqIdHeader)

		// C++: struct { int64_t id; double result; }
		binary.LittleEndian.PutUint64(respBuf[8:16], uint64(id))
		binary.LittleEndian.PutUint64(respBuf[16:24], math.Float64bits(res))

		return 8 + 16
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
