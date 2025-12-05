package main

import (
	"fmt"
	"log"
	"math/rand"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strconv"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"shm/experiments/xll/go/generated/MyIPC"

	flatbuffers "github.com/google/flatbuffers/go"
)

const (
	shmName = "ZeroCopyAddInShm"

	// SlotState
	slOT_FREE        = 0
	slOT_BUSY        = 4 // C++ client locks it
	slOT_REQ_READY   = 1
	slOT_RESP_READY  = 2

	// GuestState
	gUEST_STATE_ACTIVE  = 0
	gUEST_STATE_WAITING = 1

	// HostState
	hOST_STATE_ACTIVE = 0
	hOST_STATE_WAITING = 1

	// Message ID
	mSG_ID_FLATBUFFER = 10

	fileMapAllAccess = 0xF001F
)

type ExchangeHeader struct {
	NumSlots   uint32
	SlotSize   uint32
	ReqOffset  uint32
	RespOffset uint32
	Padding    [48]byte
}

type SlotHeader struct {
	PrePad      [64]byte
	State       uint32
	ReqSize     int32
	RespSize    int32
	MsgId       uint32
	HostState   uint32
	GuestState  uint32
	Padding     [40]byte
}

var (
	procOpenFileMappingW = syscall.NewLazyDLL("kernel32.dll").NewProc("OpenFileMappingW")
	procMapViewOfFile    = syscall.NewLazyDLL("kernel32.dll").NewProc("MapViewOfFile")
	procUnmapViewOfFile  = syscall.NewLazyDLL("kernel32.dll").NewProc("UnmapViewOfFile")
	procOpenEventW       = syscall.NewLazyDLL("kernel32.dll").NewProc("OpenEventW")
	procSetEvent         = syscall.NewLazyDLL("kernel32.dll").NewProc("SetEvent")
	procWaitForSingleObject= syscall.NewLazyDLL("kernel32.dll").NewProc("WaitForSingleObject")
)

func utf16Ptr(s string) (*uint16, error) {
	return syscall.UTF16PtrFromString(s)
}

func main() {
	exePath, _ := os.Executable()
	logDir := filepath.Dir(exePath)
	logFilePath := filepath.Join(logDir, "go-server-xll-zerocopy.log")
	logFile, err := os.OpenFile(logFilePath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0666)
	if err != nil { log.Fatalf("Failed to open log file: %v", err) }
	defer logFile.Close()
	log.SetOutput(logFile)
	log.SetFlags(log.LstdFlags | log.Lmicroseconds)
	log.Println("Go server starting...")

	numSlots, _ := strconv.Atoi(os.Args[2])
	log.Printf("Number of slots from args: %d", numSlots)

	shmNamePtr, _ := utf16Ptr(shmName)

	hMap, _, err := procOpenFileMappingW.Call(syscall.FILE_MAP_ALL_ACCESS, 0, uintptr(unsafe.Pointer(shmNamePtr)))
	if hMap == 0 { log.Fatalf("OpenFileMappingW failed: %v", err) }
	defer syscall.CloseHandle(syscall.Handle(hMap))

	// Map header to get total size
	addr, err := syscall.MapViewOfFile(syscall.Handle(hMap), syscall.FILE_MAP_ALL_ACCESS, 0, 0, unsafe.Sizeof(ExchangeHeader{}))
	if addr == 0 { log.Fatalf("MapViewOfFile for header failed: %v", err) }
	exHeader := (*ExchangeHeader)(unsafe.Pointer(addr))
	slotTotalSize := unsafe.Sizeof(SlotHeader{}) + uintptr(exHeader.SlotSize)
    totalShmSize := unsafe.Sizeof(ExchangeHeader{}) + uintptr(exHeader.NumSlots)*slotTotalSize
	syscall.UnmapViewOfFile(addr)

	// Map full shared memory
	addr, err = syscall.MapViewOfFile(syscall.Handle(hMap), syscall.FILE_MAP_ALL_ACCESS, 0, 0, totalShmSize)
	if addr == 0 { log.Fatalf("MapViewOfFile for full SHM failed: %v", err) }
	defer syscall.UnmapViewOfFile(addr)

	log.Printf("SHM Mapped. Total size: %d, NumSlots: %d, SlotSize: %d", totalShmSize, exHeader.NumSlots, exHeader.SlotSize)

	slotBasePtr := uintptr(addr) + unsafe.Sizeof(ExchangeHeader{})
	for i := 0; i < int(exHeader.NumSlots); i++ {
		slotPtr := slotBasePtr + uintptr(i)*slotTotalSize
		go worker(slotPtr, i, exHeader)
	}

	log.Println("All workers started.")

	// Monitor parent process
	if len(os.Args) > 1 {
		if pid64, err := strconv.ParseUint(os.Args[1], 10, 32); err == nil {
			hProc, perr := syscall.OpenProcess(syscall.SYNCHRONIZE, false, uint32(pid64))
			if perr == nil {
				log.Printf("Monitoring parent PID=%d", pid64)
				go func(h syscall.Handle) {
					syscall.WaitForSingleObject(h, syscall.INFINITE)
					log.Println("Parent process exited, shutting down.")
					syscall.CloseHandle(h)
					os.Exit(0)
				}(hProc)
			}
		}
	}

	c := make(chan os.Signal, 1)
	signal.Notify(c, os.Interrupt, syscall.SIGTERM)
	<-c
	log.Println("Shutting down...")
}

func worker(slotPtr uintptr, id int, exHeader *ExchangeHeader) {
	slotHeader := (*SlotHeader)(unsafe.Pointer(slotPtr))
	dataRegion := unsafe.Pointer(slotPtr + unsafe.Sizeof(SlotHeader{}))

	reqBuf := (*[1 << 24]byte)(unsafe.Pointer(uintptr(dataRegion) + uintptr(exHeader.ReqOffset)))[:exHeader.SlotSize-exHeader.RespOffset:exHeader.SlotSize-exHeader.RespOffset]
	respBuf := (*[1 << 24]byte)(unsafe.Pointer(uintptr(dataRegion) + uintptr(exHeader.RespOffset)))[:exHeader.SlotSize-exHeader.RespOffset:exHeader.SlotSize-exHeader.RespOffset]

	reqName, _ := utf16Ptr(fmt.Sprintf("%s_slot_%d", shmName, id))
	respName, _ := utf16Ptr(fmt.Sprintf("%s_slot_%d_resp", shmName, id))

	hReqEvent, _, _ := procOpenEventW.Call(syscall.EVENT_ALL_ACCESS, 0, uintptr(unsafe.Pointer(reqName)))
	defer syscall.CloseHandle(syscall.Handle(hReqEvent))
	hRespEvent, _, _ := procOpenEventW.Call(syscall.EVENT_ALL_ACCESS, 0, uintptr(unsafe.Pointer(respName)))
	defer syscall.CloseHandle(syscall.Handle(hRespEvent))

	log.Printf("Worker %d: Initialized", id)

	for {
		if atomic.LoadUint32(&slotHeader.State) != slOT_REQ_READY {
			atomic.StoreUint32(&slotHeader.GuestState, gUEST_STATE_WAITING)
			if atomic.LoadUint32(&slotHeader.State) != slOT_REQ_READY {
				syscall.WaitForSingleObject(syscall.Handle(hReqEvent), syscall.INFINITE)
			}
		}
		atomic.StoreUint32(&slotHeader.GuestState, gUEST_STATE_ACTIVE)

		reqSize := atomic.LoadInt32(&slotHeader.ReqSize)
		msgId := atomic.LoadUint32(&slotHeader.MsgId)
		var respSize int32 = 0

		if msgId == mSG_ID_FLATBUFFER && reqSize != 0 {
			absSize := -reqSize
			offset := len(reqBuf) - int(absSize)
			reqData := reqBuf[offset:]

			root := MyIPC.GetRootAsRoot(reqData, 0)
			req := new(MyIPC.Request)
			req.Init(root.Message(req).Table().Bytes, root.Message(req).Table().Pos)

			builder := flatbuffers.NewBuilder(1024)
			var respOffset flatbuffers.UOffsetT

			if req.BodyType() == MyIPC.RequestBodyAddRequest {
				addReq := new(MyIPC.AddRequest)
				addReq.Init(req.Body(addReq).Table().Bytes, req.Body(addReq).Table().Pos)
				sum := addReq.X() + addReq.Y()
				MyIPC.AddResponseStart(builder)
				MyIPC.AddResponseAddResult(builder, sum)
				addResp := MyIPC.AddResponseEnd(builder)
				MyIPC.ResponseStart(builder)
				MyIPC.ResponseAddBodyType(builder, MyIPC.ResponseBody_AddResponse)
				MyIPC.ResponseAddBody(builder, addResp)
				respOffset = MyIPC.ResponseEnd(builder)
			} else if req.BodyType() == MyIPC.RequestBodyRandRequest {
				randVal := rand.Float64()
				MyIPC.RandResponseStart(builder)
				MyIPC.RandResponseAddResult(builder, randVal)
				randResp := MyIPC.RandResponseEnd(builder)
				MyIPC.ResponseStart(builder)
				MyIPC.ResponseAddBodyType(builder, MyIPC.ResponseBody_RandResponse)
				MyIPC.ResponseAddBody(builder, randResp)
				respOffset = MyIPC.ResponseEnd(builder)
			}

			if respOffset > 0 {
				MyIPC.RootStart(builder)
				MyIPC.RootAddMessageType(builder, MyIPC.Message_Response)
				MyIPC.RootAddMessage(builder, respOffset.Union())
				rootRespOffset := MyIPC.RootEnd(builder)
				builder.Finish(rootRespOffset)
				respBytes := builder.FinishedBytes()

				absRespSize := len(respBytes)
				if absRespSize > len(respBuf) {
					respSize = 0
				} else {
					respOffset := len(respBuf) - absRespSize
					copy(respBuf[respOffset:], respBytes)
					respSize = -int32(absRespSize)
				}
			}
		}

		slotHeader.RespSize = respSize
		atomic.StoreUint32(&slotHeader.State, slOT_RESP_READY)

		if atomic.LoadUint32(&slotHeader.HostState) == hOST_STATE_WAITING {
			procSetEvent.Call(hRespEvent)
		}
	}
}

func init() {
    rand.New(rand.NewSource(time.Now().UnixNano()))
}