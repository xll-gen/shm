package shm

import (
	"fmt"
	"math"
	"sync"
	"sync/atomic"
	"time"
	"unsafe"
)

// MsgType represents the type of the message (command).
// It is distinct from the Message ID (Sequence ID).
//
// We use a distinct type definition (not an alias) to ensure type safety,
// preventing accidental mixing with other uint32 values like MsgId or sizes.
type MsgType uint32

// Constants defining slot states and message Types.
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
	// SlotGuestBusy indicates the Guest has claimed the slot and is writing data.
	SlotGuestBusy = 5

	// MsgTypeNormal is a standard data payload message.
	MsgTypeNormal        MsgType = 0
	// MsgTypeHeartbeatReq is a keep-alive request from the Host.
	MsgTypeHeartbeatReq  MsgType = 1
	// MsgTypeHeartbeatResp is the response to a keep-alive request.
	MsgTypeHeartbeatResp MsgType = 2
	// MsgTypeShutdown signals the Guest to terminate.
	MsgTypeShutdown      MsgType = 3
	// MsgTypeFlatbuffer indicates a Zero-Copy FlatBuffer payload.
	MsgTypeFlatbuffer    MsgType = 10
	// MsgTypeGuestCall indicates a Guest Call payload.
	MsgTypeGuestCall     MsgType = 11
	// MsgTypeStreamStart indicates a Stream Start payload.
	MsgTypeStreamStart   MsgType = 13
	// MsgTypeStreamChunk indicates a Stream Chunk payload.
	MsgTypeStreamChunk   MsgType = 14
	// MsgTypeSystemError indicates a system-level error (e.g. overflow).
	MsgTypeSystemError   MsgType = 127
	// MsgTypeAppStart is the start of Application Specific message types.
	// Types below 128 are reserved for internal protocol use.
	// Applications should define their own message types starting from this value.
	//
	// Example:
	//   const (
	//       MyMsgLogin  = shm.MsgTypeAppStart + 0
	//       MyMsgUpdate = shm.MsgTypeAppStart + 1
	//   )
	MsgTypeAppStart      MsgType = 128

	// Magic is the magic number for validating shared memory ("XLL!").
	Magic uint32 = 0x584C4C21
	// Version is the current protocol version (v0.6.0).
	Version uint32 = 0x00060000

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
	HostState uint32
	GuestState uint32
	MsgSeq    uint32
    MsgType   MsgType
	ReqSize   int32
	RespSize  int32
    _         [36]byte // Reserved
}

// ExchangeHeader represents the metadata at the start of the shared memory region.
// It describes the layout of the slot pool.
type ExchangeHeader struct {
	Magic         uint32
	Version       uint32
	NumSlots      uint32
	NumGuestSlots uint32
	SlotSize      uint32
	ReqOffset     uint32
	RespOffset    uint32
	_             [36]byte // Reserved
}

// slotContext holds local runtime state for a slot.
type slotContext struct {
	header     *SlotHeader
	reqBuffer  []byte
	respBuffer []byte
	reqEvent   EventHandle
	respEvent  EventHandle
    waitStrategy *WaitStrategy
    nextMsgSeq uint32
    ActiveWait int32 // Atomic flag: 1 if actively waiting, 0 otherwise
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
	responseTimeout time.Duration

	slots []slotContext
	wg    sync.WaitGroup
	closing int32

    nextGuestSlot uint32 // Atomic counter for Round-Robin slot selection
}

// NewDirectGuest initializes the DirectGuest by attaching to an existing shared memory region.
//
// name: The name of the shared memory region.
//
// Returns a pointer to the initialized DirectGuest or an error if attachment fails.
func NewDirectGuest(name string) (*DirectGuest, error) {
	Info("Initializing DirectGuest", "name", name)

	const HeaderMapSize = 64
	h, addr, err := OpenShm(name, HeaderMapSize)
	if err != nil {
		return nil, err
	}

	header := (*ExchangeHeader)(unsafe.Pointer(addr))

	if header.Magic != Magic {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("invalid magic number: 0x%x (expected 0x%x)", header.Magic, Magic)
	}
	if header.Version != Version {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("protocol version mismatch: 0x%x (expected 0x%x)", header.Version, Version)
	}

	numSlots := header.NumSlots
	numGuestSlots := header.NumGuestSlots
	slotSize := header.SlotSize
	reqOffset := header.ReqOffset
	respOffset := header.RespOffset

	if numSlots == 0 || numSlots > 100000 {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("invalid numSlots: %d", numSlots)
	}
	if slotSize == 0 || slotSize > 1024*1024*1024 {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("invalid slotSize: %d", slotSize)
	}

	CloseShm(h, addr, HeaderMapSize)

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
		responseTimeout: 10 * time.Second,
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

        maxReq := respOffset - reqOffset
        maxResp := slotSize - respOffset

		g.slots[i].reqBuffer = unsafe.Slice((*byte)(reqPtr), maxReq)
        g.slots[i].respBuffer = unsafe.Slice((*byte)(respPtr), maxResp)

		var reqName string
		if uint32(i) < numSlots {
			reqName = fmt.Sprintf("%s_slot_%d", name, i)
		} else {
			reqName = fmt.Sprintf("%s_guest_call", name)
		}
		respName := fmt.Sprintf("%s_slot_%d_resp", name, i)

		evReq, err := OpenEvent(reqName)
		if err != nil {
			CloseShm(h, addr, totalSize)
			for j := 0; j < i; j++ {
				CloseEvent(g.slots[j].reqEvent)
				CloseEvent(g.slots[j].respEvent)
			}
			return nil, fmt.Errorf("failed to open event %s: %v", reqName, err)
		}

		evResp, err := OpenEvent(respName)
		if err != nil {
			CloseEvent(evReq)
			CloseShm(h, addr, totalSize)
			for j := 0; j < i; j++ {
				CloseEvent(g.slots[j].reqEvent)
				CloseEvent(g.slots[j].respEvent)
			}
			return nil, fmt.Errorf("failed to open event %s: %v", respName, err)
		}

		g.slots[i].reqEvent = evReq
		g.slots[i].respEvent = evResp

        g.slots[i].nextMsgSeq = uint32(i + 1)

        // Optimize for Single Thread (latency) vs Multi Thread (throughput)
        enableYield := numSlots > 1
        g.slots[i].waitStrategy = NewWaitStrategy(enableYield)

		ptr += uintptr(perSlotSize)
	}

	Info("Connected to DirectGuest", "name", name, "slots", numSlots)
	return g, nil
}

// Start launches the worker goroutines.
// It spawns one goroutine per slot to handle incoming requests from the Host.
//
// handler: The function to process requests. It receives the request buffer, response buffer, and message type.
//          It must return the response size (negative for end-aligned) and response message type.
func (g *DirectGuest) Start(handler func(req []byte, resp []byte, msgType MsgType) (int32, MsgType)) {
	for i := 0; i < int(g.numSlots); i++ {
		g.wg.Add(1)
		go g.workerLoop(i, handler)
	}
}

// Close releases shared memory resources.
// It signals workers to exit and cleans up resources.
func (g *DirectGuest) Close() {
	Info("Closing DirectGuest", "name", g.name)
	atomic.StoreInt32(&g.closing, 1)
	g.wg.Wait()

	for i := range g.slots {
		CloseEvent(g.slots[i].reqEvent)
		CloseEvent(g.slots[i].respEvent)
	}
	CloseShm(g.handle, g.shmBase, g.shmSize)
}

// Wait blocks the calling thread until all worker goroutines have exited.
// Workers usually exit when the Host sends a Shutdown signal.
func (g *DirectGuest) Wait() {
    g.wg.Wait()
}

// SetTimeout sets the default timeout for Guest Call responses.
//
// d: The timeout duration. Default is 10 seconds.
func (g *DirectGuest) SetTimeout(d time.Duration) {
	g.responseTimeout = d
}

// SendGuestCall sends a request to the Host using a Guest Slot.
// It blocks until a response is received or the default timeout occurs.
//
// data: The payload to send to the Host.
// msgType: The message type identifier.
//
// Returns the response payload or an error if the call fails or times out.
func (g *DirectGuest) SendGuestCall(data []byte, msgType MsgType) ([]byte, error) {
	return g.sendGuestCallInternal(data, nil, msgType, g.responseTimeout)
}

// SendGuestCallBuffer sends a request to the Host using a provided buffer for the response.
// It reduces allocations by reusing the buffer.
//
// data: The payload to send.
// buffer: The buffer to store the response. If nil or too small, a new buffer is allocated.
// msgType: The message type identifier.
//
// Returns the response payload (slice of buffer) or an error.
func (g *DirectGuest) SendGuestCallBuffer(data []byte, buffer []byte, msgType MsgType) ([]byte, error) {
	return g.sendGuestCallInternal(data, buffer, msgType, g.responseTimeout)
}

// SendGuestCallWithTimeout sends a request to the Host using a Guest Slot with a custom timeout.
//
// data: The payload to send.
// msgType: The message type identifier.
// timeout: The custom duration to wait for a response.
//
// Returns the response payload or an error.
func (g *DirectGuest) SendGuestCallWithTimeout(data []byte, msgType MsgType, timeout time.Duration) ([]byte, error) {
	return g.sendGuestCallInternal(data, nil, msgType, timeout)
}

func (g *DirectGuest) sendGuestCallInternal(data []byte, buffer []byte, msgType MsgType, timeout time.Duration) ([]byte, error) {
	if len(data) > math.MaxInt32 {
		return nil, fmt.Errorf("data too large: %d exceeds max int32", len(data))
	}

	if g.numGuestSlots == 0 {
		return nil, fmt.Errorf("no guest slots available")
	}

	// Use Round-Robin to pick a start index to reduce contention
	offset := atomic.AddUint32(&g.nextGuestSlot, 1)
	startBase := int(g.numSlots)
	numGuest := int(g.numGuestSlots)

	var slot *slotContext
	for j := 0; j < numGuest; j++ {
		idx := startBase + int((uint32(j)+offset)%uint32(numGuest))
		s := &g.slots[idx]
		currentState := atomic.LoadUint32(&s.header.State)

		if currentState == SlotFree {
			if atomic.CompareAndSwapUint32(&s.header.State, SlotFree, SlotGuestBusy) {
				slot = s
				break
			}
		}

		if currentState == SlotRespReady {
			if atomic.LoadInt32(&s.ActiveWait) == 0 {
				if atomic.CompareAndSwapUint32(&s.header.State, SlotRespReady, SlotGuestBusy) {
					slot = s
					break
				}
			}
		}
	}

	if slot == nil {
		return nil, fmt.Errorf("all guest slots busy")
	}

	if len(data) > len(slot.reqBuffer) {
		atomic.StoreUint32(&slot.header.State, SlotFree)
		return nil, fmt.Errorf("data too large")
	}

	if msgType == MsgTypeFlatbuffer {
		offset := len(slot.reqBuffer) - len(data)
		copy(slot.reqBuffer[offset:], data)
		slot.header.ReqSize = -int32(len(data))
	} else {
		copy(slot.reqBuffer, data)
		slot.header.ReqSize = int32(len(data))
	}

	slot.header.MsgType = msgType

    currentSeq := slot.nextMsgSeq
    slot.header.MsgSeq = currentSeq
    slot.nextMsgSeq += uint32(len(g.slots))

	atomic.StoreUint32(&slot.header.State, SlotReqReady)
	SignalEvent(slot.reqEvent)

    checkReady := func() bool {
        return atomic.LoadUint32(&slot.header.State) == SlotRespReady
    }

    sleepAction := func() {
        atomic.StoreUint32(&slot.header.GuestState, GuestStateWaiting)
        if checkReady() {
            atomic.StoreUint32(&slot.header.GuestState, GuestStateActive)
            return
        }

        start := time.Now()
        for {
            if checkReady() {
                break
            }

            elapsed := time.Since(start)
            if elapsed >= timeout {
                break
            }

            remaining := timeout - elapsed
            waitMs := uint32(remaining.Milliseconds())
            if waitMs == 0 && remaining > 0 {
                waitMs = 1
            }
            if waitMs > 100 {
                waitMs = 100
            }

            WaitForEvent(slot.respEvent, waitMs)
        }
        atomic.StoreUint32(&slot.header.GuestState, GuestStateActive)
    }

	atomic.StoreInt32(&slot.ActiveWait, 1)
    ready := slot.waitStrategy.Wait(checkReady, sleepAction)
	atomic.StoreInt32(&slot.ActiveWait, 0)

	if !ready {
		Debug("SendGuestCall timed out waiting for host")
		return nil, fmt.Errorf("timeout waiting for host")
	}

    if slot.header.MsgSeq != currentSeq {
         return nil, fmt.Errorf("msgSeq mismatch: expected %d, got %d", currentSeq, slot.header.MsgSeq)
    }

    if slot.header.MsgType == MsgTypeSystemError {
        atomic.StoreUint32(&slot.header.State, SlotFree)
        return nil, fmt.Errorf("system error: host rejected request (likely buffer overflow)")
    }

	respSize := slot.header.RespSize
	var respData []byte

	if respSize >= 0 {
		if int(respSize) > len(slot.respBuffer) {
			atomic.StoreUint32(&slot.header.State, SlotFree)
			return nil, fmt.Errorf("response size %d exceeds buffer size %d", respSize, len(slot.respBuffer))
		}
		if buffer != nil && cap(buffer) >= int(respSize) {
			respData = buffer[:respSize]
		} else {
			respData = make([]byte, respSize)
		}
		copy(respData, slot.respBuffer[:respSize])
	} else {
		rLen := -respSize
		if rLen < 0 {
			atomic.StoreUint32(&slot.header.State, SlotFree)
			return nil, fmt.Errorf("invalid response size: %d", respSize)
		}
		if int(rLen) > len(slot.respBuffer) {
			atomic.StoreUint32(&slot.header.State, SlotFree)
			return nil, fmt.Errorf("response size %d exceeds buffer size %d", rLen, len(slot.respBuffer))
		}
		offset := int32(len(slot.respBuffer)) - rLen
		if buffer != nil && cap(buffer) >= int(rLen) {
			respData = buffer[:rLen]
		} else {
			respData = make([]byte, rLen)
		}
		copy(respData, slot.respBuffer[offset:])
	}

	atomic.StoreUint32(&slot.header.State, SlotFree)

	return respData, nil
}

// workerLoop is the main loop for a single slot worker.
func (g *DirectGuest) workerLoop(idx int, handler func([]byte, []byte, MsgType) (int32, MsgType)) {
	defer g.wg.Done()

	for {
		if atomic.LoadInt32(&g.closing) == 1 {
			return
		}

		shouldExit := false
		func() {
			defer func() {
				if r := recover(); r != nil {
					// If closing, we expect potential panics due to unmap. Just return.
					if atomic.LoadInt32(&g.closing) == 1 {
						return
					}

					fmt.Printf("Worker %d panic recovered (restarting): %v\n", idx, r)

					// Unblock Host if we were processing
					// If the panic happened while we owned the slot, we must release it.
					slot := &g.slots[idx]
					header := slot.header

					// Check if state implies we are holding the slot
					state := atomic.LoadUint32(&header.State)
					if state == SlotReqReady || state == SlotGuestBusy {
						// We failed during processing. Host is likely waiting.
						// Signal generic error (RespSize 0)
						header.RespSize = 0
						atomic.StoreUint32(&header.State, SlotRespReady)
						SignalEvent(slot.respEvent)
					}
				}
			}()

			shouldExit = !g.workerLoopInternal(idx, handler)
		}()

		if shouldExit {
			return
		}
	}
}

func (g *DirectGuest) workerLoopInternal(idx int, handler func([]byte, []byte, MsgType) (int32, MsgType)) bool {
	slot := &g.slots[idx]
	header := slot.header

	atomic.StoreUint32(&header.GuestState, GuestStateActive)

	for {
		if atomic.LoadInt32(&g.closing) == 1 {
			return false
		}

		checkReady := func() bool {
			return atomic.LoadUint32(&header.State) == SlotReqReady
		}

		sleepAction := func() {
			atomic.StoreUint32(&header.GuestState, GuestStateWaiting)

			if checkReady() {
				atomic.StoreUint32(&header.GuestState, GuestStateActive)
				return
			}

			WaitForEvent(slot.reqEvent, 100)

			if atomic.LoadInt32(&g.closing) == 1 {
				return
			}

			atomic.StoreUint32(&header.GuestState, GuestStateActive)
		}

		ready := slot.waitStrategy.Wait(checkReady, sleepAction)

		if ready {
			if !atomic.CompareAndSwapUint32(&header.State, SlotReqReady, SlotGuestBusy) {
				continue
			}

			msgType := header.MsgType
			if msgType == MsgTypeShutdown {
				header.RespSize = 0
				atomic.StoreUint32(&header.State, SlotRespReady)
				if atomic.LoadUint32(&header.HostState) == HostStateWaiting {
					SignalEvent(slot.respEvent)
				}
				return false
			}

			if msgType == MsgTypeHeartbeatReq {
				header.MsgType = MsgTypeHeartbeatResp
				header.RespSize = 0
			} else {
				reqSize := header.ReqSize
				var reqData []byte

				if reqSize >= 0 {
					if reqSize > int32(len(slot.reqBuffer)) {
                        header.RespSize = 0
                        header.MsgType = MsgTypeSystemError
                        atomic.StoreUint32(&header.State, SlotRespReady)
                        if atomic.LoadUint32(&header.HostState) == HostStateWaiting {
				            SignalEvent(slot.respEvent)
			            }
                        continue
					}
					reqData = slot.reqBuffer[:reqSize]
				} else {
					rLen := -reqSize
					if rLen < 0 || rLen > int32(len(slot.reqBuffer)) {
                        header.RespSize = 0
                        header.MsgType = MsgTypeSystemError
                        atomic.StoreUint32(&header.State, SlotRespReady)
                        if atomic.LoadUint32(&header.HostState) == HostStateWaiting {
				            SignalEvent(slot.respEvent)
			            }
                        continue
					}
					offset := int32(len(slot.reqBuffer)) - rLen
					reqData = slot.reqBuffer[offset:]
				}

				respSize, respType := handler(reqData, slot.respBuffer, msgType)
				header.RespSize = respSize
				header.MsgType = respType
			}

			atomic.StoreUint32(&header.State, SlotRespReady)

			if atomic.LoadUint32(&header.HostState) == HostStateWaiting {
				SignalEvent(slot.respEvent)
			}
		}
	}
	return true
}
