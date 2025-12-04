package main

import (
	"fmt"
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

// Packet matches the C++ struct layout
// struct Packet {
//     std::atomic<uint32_t> state;
//     uint32_t req_id;
//     int64_t val_a;
//     int64_t val_b;
//     int64_t sum;
//     uint8_t padding[...];
// };
type Packet struct {
	State uint32
	ReqId uint32
	ValA  int64
	ValB  int64
	Sum   int64
}

func main() {
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

	packet := (*Packet)(unsafe.Pointer(&data[0]))
	fmt.Println("[Guest] Ready.")

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
		} else {
			// Spin
			// runtime.Gosched() // Removed as per request for "busy loop", but might add back if it hangs
		}
	}
	fmt.Println("[Guest] Done.")
}
