package main

import (
	"flag"
	"fmt"
	"os"
	"runtime"
	"runtime/pprof"
	"time"

	"github.com/xll-gen/shm/go"
)

var (
	workers    = flag.Int("w", 1, "Number of worker threads (ignored for single reactor)")
	shmName    = flag.String("name", "SimpleIPC", "Shared memory name")
	cpuprofile = flag.String("cpuprofile", "", "write cpu profile to file")
	memprofile = flag.String("memprofile", "", "write memory profile to file")
)

// Helper for opening SHM
func OpenSHM(name string, size uint64) (shm.ShmHandle, uintptr, *shm.SPSCQueue, *shm.SPSCQueue, error) {
	// 1. Open Shared Memory
	shmHandle, shmAddr, err := shm.OpenShm(name, size*2)
	if err != nil {
		return 0, 0, nil, nil, err
	}

	// 2. Open Events
	reqEvent, err := shm.OpenEvent(name + "_event_req")
	if err != nil {
		shm.CloseShm(shmHandle, shmAddr) // Close SHM if event creation fails
		return 0, 0, nil, nil, err
	}
	respEvent, err := shm.OpenEvent(name + "_event_resp")
	if err != nil {
		shm.CloseEvent(reqEvent)         // Close reqEvent if respEvent creation fails
		shm.CloseShm(shmHandle, shmAddr) // Close SHM
		return 0, 0, nil, nil, err
	}

	// 3. Create Queues
	// Req: Read from 0
	reqQ := shm.NewSPSCQueue(shmAddr, size, reqEvent)

	// Resp: Write to size
	// 128 + size
	reqSize := uint64(128) + size
	respQ := shm.NewSPSCQueue(shmAddr+uintptr(reqSize), size, respEvent)

	return shmHandle, shmAddr, reqQ, respQ, nil
}

func main() {
	flag.Parse()

	// 1. Profiling Setup
	if *cpuprofile != "" {
		f, err := os.Create(*cpuprofile)
		if err != nil {
			panic(err)
		}
		pprof.StartCPUProfile(f)
		defer pprof.StopCPUProfile()
	}

	// 2. Open SHM (Retry loop)
	queueSize := uint64(32 * 1024 * 1024)
	var shmHandle shm.ShmHandle
	var shmAddr uintptr
	var reqQ, respQ *shm.SPSCQueue
	var err error

	for i := 0; i < 50; i++ {
		shmHandle, shmAddr, reqQ, respQ, err = OpenSHM(*shmName, queueSize)
		if err == nil {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}

	if err != nil {
		fmt.Printf("Failed to open SHM after retries: %v\n", err)
		return
	}
	// Defer closing the shared memory handle and address
	defer shm.CloseShm(shmHandle, shmAddr)
	defer shm.CloseEvent(reqQ.Event)
	defer shm.CloseEvent(respQ.Event)


	// 3. Start Guest
	guest := shm.NewIPCGuest(reqQ, respQ)
	guest.Start()

	fmt.Println("Go Server Started")

	// 4. Register Handler (Fiber-like)
	guest.StartReader(func(c *shm.Context) error {
		// Zero-allocation Echo:
		// 1. Get Request (no alloc, points to pooled buffer)
		req := c.Request()

		// 2. Get Response Buffer (pooled) and Append
		c.Response().Append(req)

		// 3. Send (returns buffer to pool)
		return c.Response().Send()
	})

	// 5. Wait for shutdown
	guest.Wait()

	// Write Memory Profile
	if *memprofile != "" {
		f, err := os.Create(*memprofile)
		if err != nil {
			fmt.Printf("could not create memory profile: %v\n", err)
		}
		defer f.Close()
		runtime.GC() // get up-to-date statistics
		if err := pprof.WriteHeapProfile(f); err != nil {
			fmt.Printf("could not write memory profile: %v\n", err)
		}
	}
}