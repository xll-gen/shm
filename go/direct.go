package shm

import (
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"unsafe"
)

// Constants
const (
	SlotFree      = 0
	SlotReqReady  = 1
	SlotRespReady = 2
	SlotDone      = 3
	SlotBusy      = 4

	MsgIdNormal        = 0
	MsgIdHeartbeatReq  = 1
	MsgIdHeartbeatResp = 2
	MsgIdShutdown      = 3

	HostStateActive  = 0
	HostStateWaiting = 1
	GuestStateActive = 0
	GuestStateWaiting = 1
)

// SlotHeader
type SlotHeader struct {
    _         [64]byte
	State     uint32
	ReqSize   uint32
	RespSize  uint32
	MsgId     uint32
	HostState uint32
	GuestState uint32
    _         [40]byte
}

type ExchangeHeader struct {
	NumSlots   uint32
	SlotSize   uint32
	ReqOffset  uint32
	RespOffset uint32
	_          [48]byte
}

type slotContext struct {
	header     *SlotHeader
	reqBuffer  []byte
	respBuffer []byte
	reqEvent   EventHandle
	respEvent  EventHandle
    spinLimit  int
}

type DirectGuest struct {
	shmBase  uintptr
	shmSize  uint64
	handle   ShmHandle
	numSlots uint32
	slotSize uint32
    reqOffset uint32
    respOffset uint32

	slots []slotContext
	wg    sync.WaitGroup
}

func NewDirectGuest(name string, _ int, _ int) (*DirectGuest, error) {
	// 1. Map Header
	const HeaderMapSize = 4096
	h, addr, err := OpenShm(name, HeaderMapSize)
	if err != nil {
		return nil, err
	}

	header := (*ExchangeHeader)(unsafe.Pointer(addr))
	numSlots := header.NumSlots
	slotSize := header.SlotSize
    reqOffset := header.ReqOffset
    respOffset := header.RespOffset

	if numSlots == 0 || slotSize == 0 {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("shared memory not ready")
	}

	CloseShm(h, addr, HeaderMapSize)

	// 2. Map Full
    headerSize := uint64(unsafe.Sizeof(ExchangeHeader{}))
    if headerSize < 64 { headerSize = 64 }

    slotHeaderSize := uint64(unsafe.Sizeof(SlotHeader{})) // Should be 128
    if slotHeaderSize < 128 { slotHeaderSize = 128 }

    perSlotSize := slotHeaderSize + uint64(slotSize)
    totalSize := headerSize + (perSlotSize * uint64(numSlots))

	h, addr, err = OpenShm(name, totalSize)
	if err != nil {
		return nil, err
	}

	g := &DirectGuest{
		shmBase:   addr,
		shmSize:   totalSize,
		handle:    h,
		numSlots:  numSlots,
		slotSize:  slotSize,
        reqOffset: reqOffset,
        respOffset: respOffset,
		slots:     make([]slotContext, numSlots),
	}

	ptr := addr + uintptr(headerSize)
	for i := 0; i < int(numSlots); i++ {
		g.slots[i].header = (*SlotHeader)(unsafe.Pointer(ptr))

        dataBase := ptr + uintptr(slotHeaderSize)

        // Zero-copy slicing
        // unsafe.Slice requires Go 1.17+
        reqPtr := unsafe.Pointer(dataBase + uintptr(reqOffset))
        respPtr := unsafe.Pointer(dataBase + uintptr(respOffset))

        // Calculate sizes
        // Assuming split at respOffset
        maxReq := respOffset - reqOffset
        maxResp := slotSize - respOffset

		g.slots[i].reqBuffer = unsafe.Slice((*byte)(reqPtr), maxReq)
        g.slots[i].respBuffer = unsafe.Slice((*byte)(respPtr), maxResp)

		reqName := fmt.Sprintf("%s_slot_%d", name, i)
		respName := fmt.Sprintf("%s_slot_%d_resp", name, i)

		evReq, _ := OpenEvent(reqName)
		evResp, _ := OpenEvent(respName)

		g.slots[i].reqEvent = evReq
		g.slots[i].respEvent = evResp
        g.slots[i].spinLimit = 2000

		ptr += uintptr(perSlotSize)
	}

	return g, nil
}

func (g *DirectGuest) Start(handler func(req []byte, resp []byte) uint32) {
	for i := 0; i < int(g.numSlots); i++ {
		g.wg.Add(1)
		go g.workerLoop(i, handler)
	}
}

func (g *DirectGuest) Close() {
	// Logic to stop workers?
    // They will likely stop when SHM is closed or via Shutdown msg
	CloseShm(g.handle, g.shmBase, g.shmSize)
	for _, s := range g.slots {
        if s.reqEvent != 0 { CloseEvent(s.reqEvent) }
        if s.respEvent != 0 { CloseEvent(s.respEvent) }
	}
}

func (g *DirectGuest) Wait() {
    g.wg.Wait()
}

func (g *DirectGuest) workerLoop(idx int, handler func([]byte, []byte) uint32) {
	defer g.wg.Done()

	slot := &g.slots[idx]
	header := slot.header

    // Reset guest state
    atomic.StoreUint32(&header.GuestState, GuestStateActive)

	for {
		// Adaptive Wait for REQ_READY
        ready := false
        currentLimit := slot.spinLimit
        const minSpin = 1
        const maxSpin = 2000

        // 1. Spin
        for i := 0; i < currentLimit; i++ {
            if atomic.LoadUint32(&header.State) == SlotReqReady {
                ready = true
                break
            }
            runtime.Gosched()
        }

        if ready {
            if currentLimit < maxSpin { currentLimit += 100 }
        } else {
            if currentLimit > minSpin { currentLimit -= 500 }
            if currentLimit < minSpin { currentLimit = minSpin }

            // 2. Sleep
            atomic.StoreUint32(&header.GuestState, GuestStateWaiting)

            // Double check
            if atomic.LoadUint32(&header.State) == SlotReqReady {
                 ready = true
                 atomic.StoreUint32(&header.GuestState, GuestStateActive)
            } else {
                 WaitForEvent(slot.reqEvent, 100)
                 // Check again
                 if atomic.LoadUint32(&header.State) == SlotReqReady {
                     ready = true
                 }
                 atomic.StoreUint32(&header.GuestState, GuestStateActive)
            }
        }
        slot.spinLimit = currentLimit

        if ready {
             // Process
             msgId := header.MsgId
             if msgId == MsgIdShutdown {
                 return
             }

             if msgId == MsgIdHeartbeatReq {
                 header.MsgId = MsgIdHeartbeatResp
                 header.RespSize = 0
             } else {
                 reqLen := header.ReqSize
                 if reqLen > uint32(len(slot.reqBuffer)) { reqLen = uint32(len(slot.reqBuffer)) }

                 reqData := slot.reqBuffer[:reqLen]
                 respLen := handler(reqData, slot.respBuffer)
                 header.RespSize = respLen
             }

             // Signal Ready
             atomic.StoreUint32(&header.State, SlotRespReady)

             // Wake Host
             if atomic.LoadUint32(&header.HostState) == HostStateWaiting {
                 SignalEvent(slot.respEvent)
             }

             // Wait for Host to ack/release (Host sets FREE)
             // Actually, we loop back to wait for REQ_READY.
             // If Host is slow to read, State remains RESP_READY.
             // We must NOT process until State becomes REQ_READY again.
             // Host transition: RESP_READY -> FREE -> BUSY -> REQ_READY.

        }
	}
}
