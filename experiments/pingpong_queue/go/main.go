package main

/*
#cgo LDFLAGS: -lrt
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>

// Wrapper for shm_open
int my_shm_open(const char* name, int oflag, mode_t mode) {
    return shm_open(name, oflag, mode);
}
*/
import "C"

import (
	"fmt"
	"syscall"
	"unsafe"
	"sync/atomic"
)

const (
	SHM_NAME = "/pingpong_queue_shm"
	QUEUE_CAPACITY = 64
	QUEUE_MASK = QUEUE_CAPACITY - 1
	// Size of shared memory:
	// RingBuffer = 128 (Head) + 128 (Tail) + 64 * 32 (Message is 4+8+8+8=28 -> aligned to 32? No, let's calculate exact)
	// Message struct in C++:
	// uint32_t id; (4)
	// int64_t val_a; (8)
	// int64_t val_b; (8)
	// int64_t sum; (8)
	// Total = 28 bytes.
	// Alignment of struct Message in C++ array: Usually 8 bytes.
	// sizeof(Message) is likely 32 bytes (4 padding).
	// Let's assume 32 bytes for safety.
	//
	// RingBuffer size:
	// Head: 128
	// Tail: 128
	// Data: 64 * 32 = 2048. aligned to 128.
	// Total RingBuffer = 128 + 128 + 2048 = 2304 bytes.
	// Total SHM = 2 * RingBuffer = 4608 bytes.
	SHM_SIZE = 4096 * 4
)

type Message struct {
	ID    uint32
	_pad  uint32 // Padding for alignment
	ValA  int64
	ValB  int64
	Sum   int64
}

// Atomic64 wraps uint64 for aligned atomic access
type Atomic64 struct {
	Val uint64
	_pad [120]byte // Pad to 128 bytes
}

type RingBuffer struct {
	Head Atomic64 // 128 bytes
	Tail Atomic64 // 128 bytes
	Data [QUEUE_CAPACITY]Message
}

type SharedMemory struct {
	ToGuest RingBuffer
	ToHost  RingBuffer
}

func main() {
	// 1. Open Shared Memory
	cName := C.CString(SHM_NAME)
	defer C.free(unsafe.Pointer(cName))

	// Try creating (Host should usually do it, but robustness)
	fd := C.my_shm_open(cName, C.O_RDWR, 0666)
	if fd < 0 {
		// Try create
		fd = C.my_shm_open(cName, C.O_CREAT|C.O_RDWR, 0666)
		if fd < 0 {
			panic(fmt.Sprintf("shm_open failed"))
		}
	}
	defer syscall.Close(int(fd))

	// Truncate just in case
	syscall.Ftruncate(int(fd), SHM_SIZE)

	data, err := syscall.Mmap(int(fd), 0, SHM_SIZE, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		panic(fmt.Sprintf("mmap: %v", err))
	}

	// Get pointer to shared struct
	shm := (*SharedMemory)(unsafe.Pointer(&data[0]))

	fmt.Println("Guest attached to Shared Memory.")

	// Run loop
	for {
		// --- READ REQUEST (Consumer of ToGuest) ---
		// Wait until Head != Tail
		head := atomic.LoadUint64(&shm.ToGuest.Head.Val)
		tail := atomic.LoadUint64(&shm.ToGuest.Tail.Val)

		if head == tail {
			// Empty
			// cpu_relax equivalents in Go
			// runtime.Gosched() is too slow for tight loop?
			// Just busy spin
			continue
		}

		// Process Data
		idx := tail & QUEUE_MASK
		msg := &shm.ToGuest.Data[idx]

		// Copy inputs locally to avoid reading changing memory (though it shouldn't change)
		valA := msg.ValA
		valB := msg.ValB

		// Compute
		sum := valA + valB

		// --- WRITE RESPONSE (Producer of ToHost) ---
		// We need to write to ToHost queue.
		// First, reserve space.
		respHead := atomic.LoadUint64(&shm.ToHost.Head.Val)
		respTail := atomic.LoadUint64(&shm.ToHost.Tail.Val)

		// Check full
		if (respHead - respTail) >= QUEUE_CAPACITY {
			// Full, must wait for Host to consume.
			// Ideally we shouldn't block the consumption of requests,
			// but for PingPong 1:1, we must output result.
			continue
		}

		// Write to Response Slot
		respIdx := respHead & QUEUE_MASK
		respMsg := &shm.ToHost.Data[respIdx]

		respMsg.ID = msg.ID
		respMsg.ValA = valA
		respMsg.ValB = valB
		respMsg.Sum = sum

		// Commit Response (Push)
		atomic.StoreUint64(&shm.ToHost.Head.Val, respHead + 1)

		// Commit Consumption of Request (Pop)
		// We do this AFTER writing response to ensure transaction safety?
		// Or we can do it immediately.
		// Let's do it immediately after reading to clear the slot.
		atomic.StoreUint64(&shm.ToGuest.Tail.Val, tail + 1)
	}
}
