// Package main implements the Guest side of the 1KB Payload experiment.
//
// It processes 1KB data packets by performing a bitwise NOT operation, allowing verification of data integrity.
package main

/*
#include <semaphore.h>
#include <fcntl.h>
#include <stdlib.h>

// Helper to check for SEM_FAILED (which is -1 cast to ptr)
int is_sem_failed(sem_t* sem) {
    return sem == SEM_FAILED;
}

// Helper to open existing semaphore to avoid CGO variadic issues
sem_t* open_sem(const char* name) {
    return sem_open(name, 0);
}
*/
import "C"

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
	"runtime"
)

const (
	SHM_NAME         = "/pingpong_shm_1kb"
	SHM_SIZE         = 64 * 1024
	STATE_WAIT_REQ   = 0
	STATE_REQ_READY  = 1
	STATE_RESP_READY = 2
	STATE_DONE       = 3
)

// Packet represents the shared memory structure for a single worker.
// Must match the C++ Packet struct layout (1088 bytes).
type Packet struct {
	State         uint32
	ReqId         uint32
	HostSleeping  uint32
	GuestSleeping uint32
	Data          [1024]byte
	Pad           [48]byte // 4*4 + 1024 + 48 = 16 + 1072 = 1088 bytes
}

// worker function handles the processing loop for a single thread.
func worker(id int, packet *Packet, wg *sync.WaitGroup) {
	defer wg.Done()

	hName := C.CString(fmt.Sprintf("/pp1k_h_%d", id))
	gName := C.CString(fmt.Sprintf("/pp1k_g_%d", id))
	defer C.free(unsafe.Pointer(hName))
	defer C.free(unsafe.Pointer(gName))

	// Open semaphores (Host should have created them)
	// Retry loop for semaphores
	var semHost, semGuest *C.sem_t
	for i := 0; i < 50; i++ {
		semHost = C.open_sem(hName)
		semGuest = C.open_sem(gName)
		if C.is_sem_failed(semHost) == 0 && C.is_sem_failed(semGuest) == 0 {
			break
		}
		if C.is_sem_failed(semHost) == 0 { C.sem_close(semHost) }
		if C.is_sem_failed(semGuest) == 0 { C.sem_close(semGuest) }
		time.Sleep(100 * time.Millisecond)
	}

	if C.is_sem_failed(semHost) != 0 || C.is_sem_failed(semGuest) != 0 {
		fmt.Printf("[GuestWorker %d] Failed to open semaphores\n", id)
		return
	}
	defer C.sem_close(semHost)
	defer C.sem_close(semGuest)

	spinLimit := 2000
	const minSpin = 1
	const maxSpin = 2000

	for {
		// Adaptive Wait for Request
		ready := false

		// 1. Spin Phase
		for i := 0; i < spinLimit; i++ {
			s := atomic.LoadUint32(&packet.State)
			if s == STATE_REQ_READY || s == STATE_DONE {
				ready = true
				break
			}
			runtime.Gosched()
		}

		if ready {
			// Case A: Success - Increase spin limit
			if spinLimit < maxSpin {
				spinLimit += 100
			}
			if spinLimit > maxSpin {
				spinLimit = maxSpin
			}
		} else {
			// Case B: Failure - Decrease spin limit
			if spinLimit > minSpin {
				spinLimit -= 500
			}
			if spinLimit < minSpin {
				spinLimit = minSpin
			}

			// 2. Sleep Phase
			atomic.StoreUint32(&packet.GuestSleeping, 1)

			// Double check
			s := atomic.LoadUint32(&packet.State)
			if s == STATE_REQ_READY || s == STATE_DONE {
				atomic.StoreUint32(&packet.GuestSleeping, 0)
				ready = true
			} else {
				C.sem_wait(semGuest)
				atomic.StoreUint32(&packet.GuestSleeping, 0)
			}
		}

		// Re-check state after wake
		state := atomic.LoadUint32(&packet.State)
		if state == STATE_DONE {
			break
		}

		if state == STATE_REQ_READY {
			// Process: Bitwise NOT
			// Go array access is bounds-checked, but iteration is fast.
			// Direct access is better.
			for k := 0; k < 1024; k++ {
				packet.Data[k] = ^packet.Data[k]
			}

			// Signal Done
			atomic.StoreUint32(&packet.State, STATE_RESP_READY)

			// Wake host if sleeping
			if atomic.LoadUint32(&packet.HostSleeping) == 1 {
				C.sem_post(semHost)
			}
		}
	}
}

// main entry point.
func main() {
	// Verify Packet Struct Size and Offsets
	expectedSize := uintptr(1088)
	actualSize := unsafe.Sizeof(Packet{})
	if actualSize != expectedSize {
		fmt.Printf("[Guest] Critical Error: Packet size mismatch. Expected %d, got %d\n", expectedSize, actualSize)
		os.Exit(1)
	}

	// Double check offsets
	var p Packet
	if unsafe.Offsetof(p.Data) != 16 {
		fmt.Printf("[Guest] Critical Error: Data offset mismatch. Expected 16, got %d\n", unsafe.Offsetof(p.Data))
		os.Exit(1)
	}

	numThreads := 3 // Default to 3
	if len(os.Args) > 1 {
		if n, err := strconv.Atoi(os.Args[1]); err == nil {
			numThreads = n
		}
	}

	var fd int
	var err error

	// Wait for Host to create SHM
	for i := 0; i < 50; i++ {
		fd, err = syscall.Open("/dev/shm"+SHM_NAME, syscall.O_RDWR, 0666)
		if err == nil {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if err != nil {
		fmt.Printf("[Guest] Failed to open SHM (timeout): %v\n", err)
		return
	}
	defer syscall.Close(fd)

	data, err := syscall.Mmap(fd, 0, SHM_SIZE, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		fmt.Printf("[Guest] Mmap failed: %v\n", err)
		return
	}
	defer syscall.Munmap(data)

	// Base pointer
	basePtr := unsafe.Pointer(&data[0])
	packetSize := uintptr(1088)

	var wg sync.WaitGroup
	fmt.Printf("[Guest] Starting %d workers (Adaptive Wait)...\n", numThreads)

	for i := 0; i < numThreads; i++ {
		wg.Add(1)
		// Calculate offset
		p := (*Packet)(unsafe.Pointer(uintptr(basePtr) + uintptr(i)*packetSize))
		go worker(i, p, &wg)
	}

	wg.Wait()
	fmt.Println("[Guest] All workers done.")
}
