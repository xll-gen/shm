package main

import (
	"encoding/binary"
	"fmt"
	"runtime"
	"sync/atomic"
	"syscall"
	"unsafe"

	flatbuffers "github.com/google/flatbuffers/go"
	"benchmark/ipc" // Using local generated code
	"xll-gen/shm/go/shm"
)

const (
	SHM_NAME   = "Local\\SimpleRingBufferSHM" // Default name, can be changed
	QUEUE_SIZE = 1024 * 1024 * 4              // 4MB
    FILE_MAP_ALL_ACCESS = 0xF001F
)

var (
	kernel32 = syscall.NewLazyDLL("kernel32.dll")
	procCreateEventW = kernel32.NewProc("CreateEventW")
	procCreateFileMappingW = kernel32.NewProc("CreateFileMappingW")
	procMapViewOfFile = kernel32.NewProc("MapViewOfFile")
)

func createEvent(name string) (syscall.Handle, error) {
	n, err := syscall.UTF16PtrFromString(name)
	if err != nil {
		return 0, err
	}
	r1, _, err := procCreateEventW.Call(0, 0, 0, uintptr(unsafe.Pointer(n)))
	if r1 == 0 {
		return 0, err
	}
	return syscall.Handle(r1), nil
}

func createFileMapping(name string, size uint32) (syscall.Handle, error) {
	n, err := syscall.UTF16PtrFromString(name)
	if err != nil {
		return 0, err
	}
	// CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, size, name)
	r1, _, callErr := procCreateFileMappingW.Call(
		uintptr(syscall.InvalidHandle),
		0,
		uintptr(syscall.PAGE_READWRITE),
		0,
		uintptr(size),
		uintptr(unsafe.Pointer(n)),
	)
	if r1 == 0 {
		return 0, callErr
	}
	return syscall.Handle(r1), callErr
}

func mapViewOfFile(hMap syscall.Handle) (uintptr, error) {
	r1, _, err := procMapViewOfFile.Call(
		uintptr(hMap),
		uintptr(FILE_MAP_ALL_ACCESS),
		0,
		0,
		0,
	)
	if r1 == 0 {
		return 0, err
	}
	return r1, nil
}

func main() {
	fmt.Println("[Go] Starting Server...")

	hReq, err := createEvent("Local\\SimpleIPC_REQ")
	if err != nil {
		if errno, ok := err.(syscall.Errno); !ok || errno != syscall.ERROR_ALREADY_EXISTS {
			panic(err)
		}
	}
	hResp, err := createEvent("Local\\SimpleIPC_RESP")
	if err != nil {
		if errno, ok := err.(syscall.Errno); !ok || errno != syscall.ERROR_ALREADY_EXISTS {
			panic(err)
		}
	}

	// 2. Open SHM
	qTotalSize := uint64(shm.QueueHeaderSize + QUEUE_SIZE) // Header + Data
	totalSize := uint32(qTotalSize * 2)

	hMap, callErr := createFileMapping("Local\\SimpleIPC", totalSize)
	if hMap == 0 {
		panic(callErr)
	}

	isFirst := true
	if callErr != nil {
		if errno, ok := callErr.(syscall.Errno); ok && errno == syscall.ERROR_ALREADY_EXISTS {
			isFirst = false
		}
	}

	addr, err := mapViewOfFile(hMap)
	if err != nil {
		panic(err)
	}

	// 3. Init Queues
	// Req: [0 ~ qTotalSize]
	// Resp: [qTotalSize ~ 2*qTotalSize]
	reqQueue := shm.NewMPSCQueue(addr, QUEUE_SIZE, hReq)
	respQueue := shm.NewMPSCQueue(addr+uintptr(qTotalSize), QUEUE_SIZE, hResp)

	if isFirst {
		fmt.Println("[Go] First process, initializing SHM.")
		atomic.StoreUint64(&reqQueue.Header.Capacity, QUEUE_SIZE)
		atomic.StoreUint64(&respQueue.Header.Capacity, QUEUE_SIZE)
	} else {
		fmt.Println("[Go] SHM already exists. Waiting for init...")
		// In this case, C++ client should be the first. We wait until it sets the capacity.
		for atomic.LoadUint64(&reqQueue.Header.Capacity) == 0 {
			runtime.Gosched() 
		}
	}


	fmt.Println("[Go] Server Ready. Waiting for requests...")

	// 4. Worker Loop (Single Consumer for ReqQueue)
	// Hybrid Wait Strategy
	for {
		// Try Dequeue
		data, ok := reqQueue.Dequeue()
		if ok {
			handleRequest(data, respQueue)
			continue
		}

		// Spin
		spinCount := 0
		for {
			// Check if WritePos moved (cheap check)
			if atomic.LoadUint64(&reqQueue.Header.WritePos) != atomic.LoadUint64(&reqQueue.Header.ReadPos) {
				break // Data available
			}
			spinCount++
			if spinCount > 4000 {
				// Wait Kernel
				syscall.WaitForSingleObject(hReq, 100) // Timeout for liveness check
				break
			}
			runtime.Gosched()
		}
	}
}

func handleRequest(data []byte, respQ *shm.MPSCQueue) {
    if len(data) < 8 {
        return // Invalid
    }

    reqID := binary.LittleEndian.Uint64(data[:8])
    payload := data[8:]

	// Log received data for debugging
	// fmt.Printf("[Go] Received reqID %d, payload size %d\n", reqID, len(payload))

	msg := ipc.GetRootAsMessage(payload, 0)

	builder := flatbuffers.NewBuilder(128)

	var payloadOffset flatbuffers.UOffsetT
	var payloadType ipc.Payload

	switch msg.PayloadType() {
	case ipc.PayloadAddRequest:
		req := new(ipc.AddRequest)
		var t flatbuffers.Table
		if msg.Payload(&t) {
			req.Init(t.Bytes, t.Pos)
			res := req.X() + req.Y()

			ipc.AddResponseStart(builder)
			ipc.AddResponseAddResult(builder, res)
			payloadOffset = ipc.AddResponseEnd(builder)
			payloadType = ipc.PayloadAddResponse
		}

	case ipc.PayloadMyRandRequest:
		// Mock logic
		ipc.MyRandResponseStart(builder)
		ipc.MyRandResponseAddResult(builder, 0.12345)
		payloadOffset = ipc.MyRandResponseEnd(builder)
		payloadType = ipc.PayloadMyRandResponse
	}

	// Create Message
	ipc.MessageStart(builder)
    // We can just put 0 as reqID in the message, as the transport handles it.
	ipc.MessageAddReqId(builder, 0)
	ipc.MessageAddPayloadType(builder, payloadType)
	ipc.MessageAddPayload(builder, payloadOffset)
	resMsg := ipc.MessageEnd(builder)

	builder.Finish(resMsg)
	resBytes := builder.FinishedBytes()

    // Prepend ReqID to response
    respPacket := make([]byte, 8 + len(resBytes))
    binary.LittleEndian.PutUint64(respPacket[:8], reqID)
    copy(respPacket[8:], resBytes)

	// Enqueue Response
	if !respQ.Enqueue(respPacket) {
		fmt.Println("[Go] Response Queue Full!")
	}
}
