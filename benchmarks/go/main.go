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
	streamMode    = flag.Bool("stream", false, "Enable Stream benchmark mode")
	affinityFlag  = flag.String("affinity", "auto", "Worker affinity mode: auto | none | local | sibling")
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
	fmt.Printf("  Stream Mode: %v\n", *streamMode)

	// High-level Client API
	var affinityMode shm.AffinityMode
	switch *affinityFlag {
	case "auto", "":
		affinityMode = shm.AffinityAuto
	case "none":
		affinityMode = shm.AffinityNone
	case "local":
		affinityMode = shm.AffinityLocal
	case "sibling":
		affinityMode = shm.AffinitySibling
	default:
		log.Fatalf("unknown -affinity value %q (use 'auto', 'none', 'local', or 'sibling')", *affinityFlag)
	}
	masks := shm.CcxMasks()
	pairs := shm.SmtPairs()
	fmt.Printf("  Affinity: %s (CCXs detected: %d, SMT pairs: %d)\n", affinityMode, len(masks), len(pairs))

	client, err := shm.Connect(shm.ClientConfig{
		ShmName:           *shmName,
		ConnectionTimeout: 30 * time.Second,
		RetryInterval:     100 * time.Millisecond,
		AffinityMode:      affinityMode,
	})
	if err != nil {
		log.Fatalf("Failed to connect to SHM: %v", err)
	}
	defer client.Close()

	// Base Handler
	baseHandler := func(req []byte, respBuf []byte, msgType shm.MsgType) (int32, shm.MsgType) {
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

	var handler func(req []byte, respBuf []byte, msgType shm.MsgType) (int32, shm.MsgType)
	if *streamMode {
		streamCount := uint64(0)
		handler = shm.NewStreamReassembler(func(streamID uint64, data []byte) {
			newVal := atomic.AddUint64(&streamCount, 1)
			if *verbose {
				fmt.Printf("Received Stream: ID=%d, Size=%d\n", streamID, len(data))
			}
			if newVal%100 == 0 {
				// Log occasionally?
			}
		}, baseHandler)
	} else {
		handler = baseHandler
	}

	client.Handle(handler)
	client.Start()

	// Guest Call Benchmark Logic. Ops counter and start time live outside the
	// goroutine so main() can print the results at shutdown: the old
	// print-inside-the-goroutine never ran — the final SendGuestCall blocks in
	// its response timeout while main() exits on client.Wait(), so the cell's
	// numbers were silently lost from every harness log.
	var guestCallOps uint64
	var guestCallStartNs int64 // atomic: written by the sender goroutine, read by main at shutdown
	if *guestCallMode {
		go func() {
			fmt.Println("Starting Guest Call Worker...")
			// Co-locate with the C++ guest-call worker (v0.8.9): it pins to
			// the host LP of SMT pair [numWorkers % pairs] (see main.cpp), so
			// pin this dedicated sender to the same pair's guest LP — the
			// shared-L1d placement Direct Exchange gets from AffinitySibling.
			// Skipped under -affinity none (mirrors the C++ gate).
			if affinityMode != shm.AffinityNone {
				if pairs := shm.SmtPairs(); len(pairs) > 0 {
					shm.PinCurrentGoroutine(pairs[*numWorkers%len(pairs)].Guest)
				}
			}
			// Wait a bit for Host to be ready
			time.Sleep(2 * time.Second)

			payload := make([]byte, 64) // 64 bytes payload
			// Fill payload
			for i := range payload {
				payload[i] = byte(i)
			}
			// Response buffer hoisted out of the loop: SendGuestCall allocates
			// a fresh response slice per call, which is benchmark overhead of
			// the same kind as the hoisted payload memcpy on the C++ side
			// (v0.8.4). SendGuestCallBuffer is the API real hot-path callers
			// should use, so the cell measures it.
			respBuf := make([]byte, 64)

			atomic.StoreInt64(&guestCallStartNs, time.Now().UnixNano())

			for {
				// Send Guest Call
				_, err := client.SendGuestCallBuffer(payload, respBuf, shm.MsgTypeGuestCall)
				if err != nil {
					// Host shutdown or timeout
					break
				}
				atomic.AddUint64(&guestCallOps, 1)
			}
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

	if startNs := atomic.LoadInt64(&guestCallStartNs); *guestCallMode && startNs != 0 {
		elapsed := time.Since(time.Unix(0, startNs))
		finalOps := atomic.LoadUint64(&guestCallOps)
		fmt.Printf("Guest Call Results:\n")
		fmt.Printf("  Total Ops:      %s\n", formatUint(finalOps))
		fmt.Printf("  Duration:       %v\n", elapsed)
		fmt.Printf("  Throughput:     %s ops/s\n", formatFloat(float64(finalOps)/elapsed.Seconds()))
	}

	gSpin := shm.WaitStatsSpinSuccess()
	gSleep := shm.WaitStatsSleepFallback()
	gTotal := gSpin + gSleep
	var sleepPct float64
	if gTotal > 0 {
		sleepPct = 100.0 * float64(gSleep) / float64(gTotal)
	}
	gIters := shm.WaitStatsIterCount()
	var avgIters float64
	if gSpin > 0 {
		avgIters = float64(gIters) / float64(gSpin)
	}
	fmt.Printf("[GuestWS] SpinSuccess:   %s\n", formatUint(gSpin))
	fmt.Printf("[GuestWS] SleepFallback: %s (%.2f%%)\n", formatUint(gSleep), sleepPct)
	fmt.Printf("[GuestWS] AvgItersPerSpin: %.1f\n", avgIters)

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
