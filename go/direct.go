package shm

import (
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"unsafe"
)

// Constants defining slot states and message IDs.
const (
	// SlotFree indicates the slot is available for the Host to claim.
	SlotFree      = 0
	// SlotReqReady indicates the Host has written a request and it is ready for the Guest.
	SlotReqReady  = 1
	// SlotRespReady indicates the Guest has written a response and it is ready for the Host.
	SlotRespReady = 2
	// SlotDone is a transient state indicating transaction completion.
	SlotDone      = 3
	// SlotBusy indicates the Host has claimed the slot and is writing data.
	SlotBusy      = 4

	// MsgIdNormal is a standard data payload message.
	MsgIdNormal        = 0
	// MsgIdHeartbeatReq is a keep-alive request from the Host.
	MsgIdHeartbeatReq  = 1
	// MsgIdHeartbeatResp is the response to a keep-alive request.
	MsgIdHeartbeatResp = 2
	// MsgIdShutdown signals the Guest to terminate.
	MsgIdShutdown      = 3
	// MsgIdFlatbuffer indicates a Zero-Copy FlatBuffer payload.
	MsgIdFlatbuffer    = 10
	// MsgIdGuestCall indicates a Guest Call payload.
	MsgIdGuestCall     = 11

	// HostStateActive indicates the Host is spinning or processing.
	HostStateActive  = 0
	// HostStateWaiting indicates the Host is sleeping on the Response Event.
	HostStateWaiting = 1
	// GuestStateActive indicates the Guest is spinning or processing.
	GuestStateActive = 0
	// GuestStateWaiting indicates the Guest is sleeping on the Request Event.
	GuestStateWaiting = 1
)

// SlotHeader represents the metadata for a single slot in shared memory.
// It must match the C++ layout exactly (128 bytes).
type SlotHeader struct {
    _         [64]byte
	State     uint32
	ReqSize   int32
	RespSize  int32
	MsgId     uint32
	HostState uint32
	GuestState uint32
    _         [40]byte
}

// ExchangeHeader represents the metadata at the start of the shared memory region.
// It describes the layout of the slot pool.
type ExchangeHeader struct {
	NumSlots      uint32
	NumGuestSlots uint32
	SlotSize      uint32
	ReqOffset     uint32
	RespOffset    uint32
	_             [44]byte
}

// slotContext holds local runtime state for a slot.
type slotContext struct {
	header     *SlotHeader
	reqBuffer  []byte
	respBuffer []byte
	reqEvent   EventHandle
	respEvent  EventHandle
    spinLimit  int
}

// DirectGuest implements the Guest side of the Direct Mode IPC.
// It manages multiple workers, each attached to a specific slot.
type DirectGuest struct {
	name          string
	shmBase       uintptr
	shmSize       uint64
	handle        ShmHandle
	numSlots      uint32
	numGuestSlots uint32
	slotSize      uint32
	reqOffset     uint32
	respOffset    uint32

	slots []slotContext
	wg    sync.WaitGroup
}

// NewDirectGuest initializes the DirectGuest by attaching to an existing shared memory region.
//
// name: The name of the shared memory region.
// unused1, unused2: Parameters maintained for legacy interface compatibility (ignored).
//
// Returns a DirectGuest instance or an error.
func NewDirectGuest(name string, _ int, _ int) (*DirectGuest, error) {
	// 1. Map Header
	const HeaderMapSize = 4096
	h, addr, err := OpenShm(name, HeaderMapSize)
	if err != nil {
		return nil, err
	}

	header := (*ExchangeHeader)(unsafe.Pointer(addr))
	numSlots := header.NumSlots
	numGuestSlots := header.NumGuestSlots
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
	if headerSize < 64 {
		headerSize = 64
	}

	slotHeaderSize := uint64(unsafe.Sizeof(SlotHeader{})) // Should be 128
	if slotHeaderSize < 128 {
		slotHeaderSize = 128
	}

	perSlotSize := slotHeaderSize + uint64(slotSize)
	totalSlots := numSlots + numGuestSlots
	totalSize := headerSize + (perSlotSize * uint64(totalSlots))

	h, addr, err = OpenShm(name, totalSize)
	if err != nil {
		return nil, err
	}

	g := &DirectGuest{
		name:          name,
		shmBase:       addr,
		shmSize:       totalSize,
		handle:        h,
		numSlots:      numSlots,
		numGuestSlots: numGuestSlots,
		slotSize:      slotSize,
		reqOffset:     reqOffset,
		respOffset:    respOffset,
		slots:         make([]slotContext, totalSlots),
	}

	ptr := addr + uintptr(headerSize)
	for i := 0; i < int(totalSlots); i++ {
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

// Start launches the worker goroutines.
//
// handler: The function to process requests.
func (g *DirectGuest) Start(handler func(req []byte, resp []byte, msgId uint32) int32) {
	for i := 0; i < int(g.numSlots); i++ {
		g.wg.Add(1)
		go g.workerLoop(i, handler)
	}
}

// Close releases resources.
func (g *DirectGuest) Close() {
	// Logic to stop workers?
    // They will likely stop when SHM is closed or via Shutdown msg
	CloseShm(g.handle, g.shmBase, g.shmSize)
	UnlinkShm(g.name)

	for i, s := range g.slots {
        if s.reqEvent != 0 { CloseEvent(s.reqEvent) }
        if s.respEvent != 0 { CloseEvent(s.respEvent) }

		reqName := fmt.Sprintf("%s_slot_%d", g.name, i)
		respName := fmt.Sprintf("%s_slot_%d_resp", g.name, i)
		UnlinkEvent(reqName)
		UnlinkEvent(respName)
	}
}

// Wait blocks until all workers finish.
func (g *DirectGuest) Wait() {
    g.wg.Wait()
}

// SendGuestCall sends a request to the Host using a Guest Slot.
// It blocks until a response is received or a timeout occurs.
func (g *DirectGuest) SendGuestCall(data []byte, msgId uint32) ([]byte, error) {
	if g.numGuestSlots == 0 {
		return nil, fmt.Errorf("no guest slots available")
	}

	startIdx := int(g.numSlots)
	endIdx := int(g.numSlots + g.numGuestSlots)

	var slot *slotContext
	// Simple scan
	for i := startIdx; i < endIdx; i++ {
		s := &g.slots[i]
		if atomic.CompareAndSwapUint32(&s.header.State, SlotFree, SlotBusy) {
			slot = s
			break
		}
	}

	if slot == nil {
		return nil, fmt.Errorf("all guest slots busy")
	}

	// Write Data
	if len(data) > len(slot.reqBuffer) {
		atomic.StoreUint32(&slot.header.State, SlotFree)
		return nil, fmt.Errorf("data too large")
	}

	if msgId == MsgIdFlatbuffer {
		// End-aligned for Zero-Copy FlatBuffers
		offset := len(slot.reqBuffer) - len(data)
		copy(slot.reqBuffer[offset:], data)
		slot.header.ReqSize = -int32(len(data))
	} else {
		copy(slot.reqBuffer, data)
		slot.header.ReqSize = int32(len(data))
	}

	slot.header.MsgId = msgId

	// Signal Ready
	atomic.StoreUint32(&slot.header.State, SlotReqReady)
	SignalEvent(slot.reqEvent)

	// Wait for Response
	ready := false
	currentLimit := 2000

	for i := 0; i < currentLimit; i++ {
		if atomic.LoadUint32(&slot.header.State) == SlotRespReady {
			ready = true
			break
		}
		runtime.Gosched()
	}

	if !ready {
		atomic.StoreUint32(&slot.header.GuestState, GuestStateWaiting)
		if atomic.LoadUint32(&slot.header.State) == SlotRespReady {
			ready = true
			atomic.StoreUint32(&slot.header.GuestState, GuestStateActive)
		} else {
			WaitForEvent(slot.respEvent, 2000) // 2s timeout
			if atomic.LoadUint32(&slot.header.State) == SlotRespReady {
				ready = true
			}
			atomic.StoreUint32(&slot.header.GuestState, GuestStateActive)
		}
	}

	if !ready {
		// Timeout - DO NOT free, Host might process late.
		// Returning error will likely cause caller to retry or fail.
		return nil, fmt.Errorf("timeout waiting for host")
	}

	// Read Response
	respSize := slot.header.RespSize
	var respData []byte

	if respSize >= 0 {
		if int(respSize) > len(slot.respBuffer) {
			respSize = int32(len(slot.respBuffer))
		}
		respData = make([]byte, respSize)
		copy(respData, slot.respBuffer[:respSize])
	} else {
		// End aligned support could be added here
		respData = make([]byte, 0)
	}

	// Release Slot
	atomic.StoreUint32(&slot.header.State, SlotFree)

	return respData, nil
}

// workerLoop is the main loop for a single slot worker.
func (g *DirectGuest) workerLoop(idx int, handler func([]byte, []byte, uint32) int32) {
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
                 header.RespSize = 0
                 atomic.StoreUint32(&header.State, SlotRespReady)
                 if atomic.LoadUint32(&header.HostState) == HostStateWaiting {
                     SignalEvent(slot.respEvent)
                 }
                 return
             }

             if msgId == MsgIdHeartbeatReq {
                 header.MsgId = MsgIdHeartbeatResp
                 header.RespSize = 0
             } else {
                 reqSize := header.ReqSize
                 var reqData []byte

                 if reqSize >= 0 {
                     if reqSize > int32(len(slot.reqBuffer)) { reqSize = int32(len(slot.reqBuffer)) }
                     reqData = slot.reqBuffer[:reqSize]
                 } else {
                     // Negative size means data is at the end
                     rLen := -reqSize
                     if rLen > int32(len(slot.reqBuffer)) { rLen = int32(len(slot.reqBuffer)) }
                     offset := int32(len(slot.reqBuffer)) - rLen
                     reqData = slot.reqBuffer[offset:]
                 }

                 respSize := handler(reqData, slot.respBuffer, msgId)
                 header.RespSize = respSize
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
