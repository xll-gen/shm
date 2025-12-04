package shm

import (
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"unsafe"
)

// Constants matching C++
const (
	SlotWaitReq   = 0
	SlotReqReady  = 1
	SlotRespReady = 2
	SlotDone      = 3
)

const (
	MsgIdNormal        = 0
	MsgIdHeartbeatReq  = 1
	MsgIdHeartbeatResp = 2
	MsgIdShutdown      = 3
)

type SlotHeader struct {
	_             [64]byte
	State         uint32
	ReqSize       uint32
	RespSize      uint32
	MsgId         uint32
	HostSleeping  uint32
	GuestSleeping uint32
	_             [40]byte
}

type ExchangeHeader struct {
	NumSlots uint32
	SlotSize uint32
	_        [56]byte
}

type DirectGuest struct {
	shmBase  uintptr
	shmSize  uint64
	handle   ShmHandle
	numSlots uint32
	halfSize uint32

	slots []slotContext
	wg    sync.WaitGroup
}

type slotContext struct {
	header    *SlotHeader
	reqBuf    []byte
	respBuf   []byte
	reqEvent  EventHandle // Guest waits on this
	respEvent EventHandle // Host waits on this
}

func NewDirectGuest(name string) (*DirectGuest, error) {
	// Step 1: Map the first page to read header
	const HeaderMapSize = 4096
	h, addr, err := OpenShm(name, HeaderMapSize)
	if err != nil {
		return nil, err
	}

	header := (*ExchangeHeader)(unsafe.Pointer(addr))
	numSlots := header.NumSlots
	slotSize := header.SlotSize // Total data size

	if numSlots == 0 || slotSize == 0 {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("shared memory not ready (slots/size is 0)")
	}

	CloseShm(h, addr, HeaderMapSize)

	// Step 2: Remap full size
	// C++ layout: ExchangeHeader + (SlotHeader + SlotSize) * NumSlots
	headerSize := uint64(unsafe.Sizeof(ExchangeHeader{}))
	if headerSize < 64 { headerSize = 64 }

	slotHeaderSize := uint64(unsafe.Sizeof(SlotHeader{})) // 128
	perSlotSize := slotHeaderSize + uint64(slotSize)
	totalSize := headerSize + (perSlotSize * uint64(numSlots))

	h, addr, err = OpenShm(name, totalSize)
	if err != nil {
		return nil, err
	}

	g := &DirectGuest{
		shmBase:  addr,
		shmSize:  totalSize,
		handle:   h,
		numSlots: numSlots,
		halfSize: slotSize / 2,
		slots:    make([]slotContext, numSlots),
	}

	ptr := addr + uintptr(headerSize)
	for i := 0; i < int(numSlots); i++ {
		g.slots[i].header = (*SlotHeader)(unsafe.Pointer(ptr))

		// Req Buffer starts after header
		reqPtr := unsafe.Pointer(ptr + uintptr(slotHeaderSize))
		g.slots[i].reqBuf = unsafe.Slice((*byte)(reqPtr), g.halfSize)

		// Resp Buffer starts after Req Buffer
		respPtr := unsafe.Pointer(uintptr(reqPtr) + uintptr(g.halfSize))
		g.slots[i].respBuf = unsafe.Slice((*byte)(respPtr), g.halfSize)

		// Events
		// reqEvent: {name}_slot_{i}
		// respEvent: {name}_slot_{i}_resp
		reqName := fmt.Sprintf("%s_slot_%d", name, i)
		respName := fmt.Sprintf("%s_slot_%d_resp", name, i)

		evReq, err := OpenEvent(reqName)
		if err != nil {
			g.Close()
			return nil, err
		}
		g.slots[i].reqEvent = evReq

		evResp, err := OpenEvent(respName)
		if err != nil {
			g.Close()
			return nil, err
		}
		g.slots[i].respEvent = evResp

		ptr += uintptr(perSlotSize)
	}

	return g, nil
}

func (g *DirectGuest) Start(handler func([]byte, []byte) int) {
	for i := 0; i < int(g.numSlots); i++ {
		g.wg.Add(1)
		go g.workerLoop(i, handler)
	}
}

func (g *DirectGuest) Wait() {
	g.wg.Wait()
}

func (g *DirectGuest) Close() {
	CloseShm(g.handle, g.shmBase, g.shmSize)
	for _, s := range g.slots {
		if s.reqEvent != 0 {
			CloseEvent(s.reqEvent)
		}
		if s.respEvent != 0 {
			CloseEvent(s.respEvent)
		}
	}
}

func (g *DirectGuest) workerLoop(idx int, handler func([]byte, []byte) int) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	defer g.wg.Done()

	slot := &g.slots[idx]
	header := slot.header

	// Reset state
	// In PingPong, we start by waiting for REQ.
	// Host initializes to WAIT_REQ.

	spinLimit := 2000
	const minSpin = 1
	const maxSpin = 2000

	for {
		ready := false

		// 1. Spin Phase
		for i := 0; i < spinLimit; i++ {
			s := atomic.LoadUint32(&header.State)
			if s == SlotReqReady || s == SlotDone {
				ready = true
				break
			}
			runtime.Gosched()
		}

		if ready {
			if spinLimit < maxSpin {
				spinLimit += 100
			}
		} else {
			if spinLimit > minSpin {
				spinLimit -= 500
			}

			// 2. Sleep Phase
			atomic.StoreUint32(&header.GuestSleeping, 1)

			// Double Check
			s := atomic.LoadUint32(&header.State)
			if s == SlotReqReady || s == SlotDone {
				atomic.StoreUint32(&header.GuestSleeping, 0)
				ready = true
			} else {
				// Wait on reqEvent (Host signals this when ReqReady)
				WaitForEvent(slot.reqEvent, 1000)
				atomic.StoreUint32(&header.GuestSleeping, 0)
			}
		}

		// Check State
		state := atomic.LoadUint32(&header.State)
		if state == SlotDone || (header.MsgId == MsgIdShutdown) {
			return
		}

		if state == SlotReqReady {
			// Process
			reqLen := header.ReqSize
			if reqLen > uint32(len(slot.reqBuf)) {
				reqLen = uint32(len(slot.reqBuf))
			}
			reqData := slot.reqBuf[:reqLen]

			// Handle Heartbeat internally?
			if header.MsgId == MsgIdHeartbeatReq {
				header.MsgId = MsgIdHeartbeatResp
				header.RespSize = 0
			} else {
				// Call User Handler
				respLen := handler(reqData, slot.respBuf)
				if respLen > len(slot.respBuf) {
					respLen = len(slot.respBuf)
				}
				header.RespSize = uint32(respLen)
			}

			// Signal Done
			atomic.StoreUint32(&header.State, SlotRespReady)

			// Wake Host if sleeping
			if atomic.LoadUint32(&header.HostSleeping) == 1 {
				SignalEvent(slot.respEvent)
			}
		}
	}
}
