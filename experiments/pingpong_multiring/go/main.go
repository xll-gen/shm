package main

/*
#cgo LDFLAGS: -lrt
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>

int my_shm_open(const char* name, int oflag, mode_t mode) {
    return shm_open(name, oflag, mode);
}
*/
import "C"

import (
	"flag"
	"fmt"
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

const (
	SHM_NAME       = "/pingpong_multiring_shm"
	QUEUE_CAPACITY = 64
	QUEUE_MASK     = QUEUE_CAPACITY - 1
	MAX_THREADS    = 16
	// Size calculated in C++: 2304 per RingBuffer.
	// 2 RingBuffers per Thread.
	// Total = 2304 * 2 * MAX_THREADS.
)

type Message struct {
	ID   uint32
	_pad uint32
	ValA int64
	ValB int64
	Sum  int64
}

type Atomic64 struct {
	Val  uint64
	_pad [120]byte
}

type RingBuffer struct {
	Head Atomic64
	Tail Atomic64
	Data [QUEUE_CAPACITY]Message
}

type SharedMemory struct {
	ToGuest [MAX_THREADS]RingBuffer
	ToHost  [MAX_THREADS]RingBuffer
}

func worker(id int, shm *SharedMemory, wg *sync.WaitGroup) {
	defer wg.Done()
	qIn := &shm.ToGuest[id]
	qOut := &shm.ToHost[id]

	for {
		// Poll Input
		head := atomic.LoadUint64(&qIn.Head.Val)
		tail := atomic.LoadUint64(&qIn.Tail.Val)

		if head == tail {
			// Empty
			continue
		}

		idx := tail & QUEUE_MASK
		msg := &qIn.Data[idx]
		valA := msg.ValA
		valB := msg.ValB
		idVal := msg.ID

		// Process
		sum := valA + valB

		// Push Output
		respHead := atomic.LoadUint64(&qOut.Head.Val)
		respTail := atomic.LoadUint64(&qOut.Tail.Val)

		if (respHead - respTail) >= QUEUE_CAPACITY {
			// Full, shouldn't happen often in pingpong
			continue
		}

		respIdx := respHead & QUEUE_MASK
		respMsg := &qOut.Data[respIdx]
		respMsg.ID = idVal
		respMsg.ValA = valA
		respMsg.ValB = valB
		respMsg.Sum = sum

		atomic.StoreUint64(&qOut.Head.Val, respHead+1)
		atomic.StoreUint64(&qIn.Tail.Val, tail+1)
	}
}

func main() {
	numThreads := flag.Int("w", 1, "number of workers")
	flag.Parse()

	if *numThreads > MAX_THREADS {
		*numThreads = MAX_THREADS
	}

	cName := C.CString(SHM_NAME)
	defer C.free(unsafe.Pointer(cName))

	shmSize := int(unsafe.Sizeof(SharedMemory{}))
    // Safety check size
    // 2304 * 32 = 73728
    // fmt.Println("Expected Size:", shmSize)

	fd := C.my_shm_open(cName, C.O_RDWR, 0666)
	if fd < 0 {
		fd = C.my_shm_open(cName, C.O_CREAT|C.O_RDWR, 0666)
		if fd < 0 {
			panic("shm_open failed")
		}
	}
	defer syscall.Close(int(fd))
	syscall.Ftruncate(int(fd), int64(shmSize))

	data, err := syscall.Mmap(int(fd), 0, shmSize, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		panic(err)
	}

	shm := (*SharedMemory)(unsafe.Pointer(&data[0]))

	fmt.Printf("Guest attached. Workers: %d\n", *numThreads)

	var wg sync.WaitGroup
	wg.Add(*numThreads)
	for i := 0; i < *numThreads; i++ {
		go worker(i, shm, &wg)
	}
	wg.Wait()
}
