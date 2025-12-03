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

	// Handler - using simple byte slice handler now
	client.Handle(func(reqData []byte) []byte {
		if len(reqData) != ReqSize {
			// Malformed request - ignore or return empty/error frame
            // In a benchmark we typically panic or return nil to indicate drop
            // But let's be robust
			return nil
		}

		// Parse Request (Little Endian)
		// C++: struct { int64_t id; double x; double y; }
		id := int64(binary.LittleEndian.Uint64(reqData[0:8]))
		x := math.Float64frombits(binary.LittleEndian.Uint64(reqData[8:16]))
		y := math.Float64frombits(binary.LittleEndian.Uint64(reqData[16:24]))

		// Process
		res := x + y

		// Prepare Response
		// C++: struct { int64_t id; double result; }
		respData := make([]byte, 16)
		binary.LittleEndian.PutUint64(respData[0:8], uint64(id))
		binary.LittleEndian.PutUint64(respData[8:16], math.Float64bits(res))

		return respData
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
