package main

import (
	"fmt"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

const (
	SHM_NAME         = "/pingpong_shm"
	SHM_SIZE         = 4096
	STATE_WAIT_REQ   = 0
	STATE_REQ_READY  = 1
	STATE_RESP_READY = 2
	STATE_DONE       = 3
)

type Packet struct {
	State uint32
	ReqId uint32
	ValA  int64
	ValB  int64
	Sum   int64
	Pad   [20]byte // 64 - 4 - 4 - 8 - 8 - 8 = 32.
	// Wait, C++ struct:
	// atomic<uint32> state (4)
	// uint32 req_id (4)
	// int64 val_a (8)
	// int64 val_b (8)
	// int64 sum (8)
	// Total data = 32 bytes.
	// Padding = 64 - 32 = 32 bytes.
	// [32]byte
}

// Ensure correct size manually if needed, but the cast handles layout.
// Go struct alignment might add padding between fields?
// 4+4 = 8, aligned. +8+8+8 = 32.
// So we need 32 bytes of padding at the end to reach 64.
// Let's make sure we step by 64 bytes explicitly.

func worker(id int, packet *Packet, wg *sync.WaitGroup) {
	defer wg.Done()
	// fmt.Printf("[GuestWorker %d] Started\n", id)
	for {
		// Busy loop
		state := atomic.LoadUint32(&packet.State)
		if state == STATE_DONE {
			break
		}
		if state == STATE_REQ_READY {
			// Read values
			a := packet.ValA
			b := packet.ValB
			// Calc
			packet.Sum = a + b

			// Signal Done
			atomic.StoreUint32(&packet.State, STATE_RESP_READY)
		}
	}
	// fmt.Printf("[GuestWorker %d] Done\n", id)
}

func main() {
	numThreads := 1
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
	packetSize := uintptr(64) // Assuming 64 byte stride

	var wg sync.WaitGroup
	fmt.Printf("[Guest] Starting %d workers...\n", numThreads)

	for i := 0; i < numThreads; i++ {
		wg.Add(1)
		// Calculate offset
		p := (*Packet)(unsafe.Pointer(uintptr(basePtr) + uintptr(i)*packetSize))
		go worker(i, p, &wg)
	}

	wg.Wait()
	fmt.Println("[Guest] All workers done.")
}
