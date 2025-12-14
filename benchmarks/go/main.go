package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"runtime"
	"runtime/pprof"
	"strconv"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/xll-gen/shm/go"
)

var (
	shmName       = flag.String("name", "SimpleIPC", "Shared memory name")
	numWorkers    = flag.Int("w", 1, "Number of worker goroutines")
	verbose       = flag.Bool("v", false, "Verbose logging")
	cpuProfile    = flag.String("cpuprofile", "", "Write cpu profile to file")
	memProfile    = flag.String("memprofile", "", "Write memory profile to file")
	guestCallMode = flag.Bool("guest-call", false, "Enable Guest Call benchmark mode")
)

func main() {
	flag.Parse()

	if *cpuProfile != "" {
		f, err := os.Create(*cpuProfile)
		if err != nil {
			log.Fatal(err)
		}
		pprof.StartCPUProfile(f)
		defer pprof.StopCPUProfile()
	}

	// Use default GOMAXPROCS

	fmt.Printf("Starting Go Benchmark Server:\n")
	fmt.Printf("  SHM Name: %s\n", *shmName)
	fmt.Printf("  Workers: %d\n", *numWorkers)
	fmt.Printf("  Guest Call Mode: %v\n", *guestCallMode)

	// High-level Client API
	client, err := shm.Connect(shm.ClientConfig{
		ShmName:           *shmName,
		ConnectionTimeout: 30 * time.Second,
		RetryInterval:     100 * time.Millisecond,
	})
	if err != nil {
		log.Fatalf("Failed to connect to SHM: %v", err)
	}
	defer client.Close()

	// Handler
	handler := func(req []byte, respBuf []byte, msgType shm.MsgType) (int32, shm.MsgType) {
		// Verify Header?
		// Benchmark C++ client sends [8 byte ID][Payload]
		// We just echo it back.
		if len(req) == 0 {
			return 0, shm.MsgTypeNormal
		}

		// Just copy req to respBuf
		copy(respBuf, req)
		return int32(len(req)), shm.MsgTypeNormal
	}

	client.Handle(handler)
	client.Start()

	// Guest Call Benchmark Logic
	if *guestCallMode {
		go func() {
			fmt.Println("Starting Guest Call Worker...")
			// Wait a bit for Host to be ready
			time.Sleep(2 * time.Second)

			payload := make([]byte, 64) // 64 bytes payload
			// Fill payload
			for i := range payload {
				payload[i] = byte(i)
			}

			var ops uint64
			start := time.Now()

			for {
				// Send Guest Call
				_, err := client.SendGuestCall(payload, shm.MsgTypeGuestCall)
				if err != nil {
					// Host shutdown or timeout
					break
				}
				atomic.AddUint64(&ops, 1)

				if atomic.LoadUint64(&ops)%10000 == 0 {
					// Check if stopped?
				}
			}
			elapsed := time.Since(start)
			finalOps := atomic.LoadUint64(&ops)
			fmt.Printf("Guest Call Results:\n")
			fmt.Printf("  Total Ops:      %s\n", formatUint(finalOps))
			fmt.Printf("  Duration:       %v\n", elapsed)
			fmt.Printf("  Throughput:     %s ops/s\n", formatFloat(float64(finalOps)/elapsed.Seconds()))
		}()
	}

	// Wait for signal
	sigs := make(chan os.Signal, 1)
	signal.Notify(sigs, syscall.SIGINT, syscall.SIGTERM)

	// Wait for client to exit (shutdown msg) or signal
	done := make(chan struct{})
	go func() {
		client.Wait()
		close(done)
	}()

	select {
	case <-sigs:
		fmt.Println("Signal received, exiting...")
	case <-done:
		fmt.Println("Client stopped (Host Shutdown).")
	}

	if *memProfile != "" {
		f, err := os.Create(*memProfile)
		if err != nil {
			log.Fatal(err)
		}
		runtime.GC()
		if err := pprof.WriteHeapProfile(f); err != nil {
			log.Fatal(err)
		}
		f.Close()
	}
}

// Helper to parse uint64 from bytes
func readUint64(b []byte) uint64 {
	if len(b) < 8 {
		return 0
	}
	return binary.LittleEndian.Uint64(b)
}

func formatUint(n uint64) string {
	s := strconv.FormatUint(n, 10)
	for i := len(s) - 3; i > 0; i -= 3 {
		s = s[:i] + "," + s[i:]
	}
	return s
}

func formatFloat(n float64) string {
	s := fmt.Sprintf("%.2f", n)
	dot := -1
	for i := 0; i < len(s); i++ {
		if s[i] == '.' {
			dot = i
			break
		}
	}
	if dot == -1 {
		dot = len(s)
	}
	for i := dot - 3; i > 0; i -= 3 {
		s = s[:i] + "," + s[i:]
	}
	return s
}
