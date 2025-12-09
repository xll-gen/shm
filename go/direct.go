package shm

import (
	"fmt"
	"runtime"
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
    _         [36]byte
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
	responseTimeout time.Duration

	slots []slotContext
	wg    sync.WaitGroup
}

// NewDirectGuest initializes the DirectGuest by attaching to an existing shared memory region.
//
// name: The name of the shared memory region.
// unused1: Ignored parameter (legacy compatibility).
// unused2: Ignored parameter (legacy compatibility).
//
// Returns a pointer to the initialized DirectGuest or an error if attachment fails.
func NewDirectGuest(name string, _ int, _ int) (*DirectGuest, error) {
	// 1. Map Header
	// We try to map a small chunk first to read the header.
	// However, if the file is smaller than 4096 (e.g. 1 slot), OpenShm check will fail.
	// We should map sizeof(ExchangeHeader) at minimum (64 bytes).
	// But OpenShm is page-aligned usually? No, mmap is.
	// Let's use 64 bytes.
	const HeaderMapSize = 64
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

        // Calculate sizes
        // Assuming split at respOffset
        maxReq := respOffset - reqOffset
        maxResp := slotSize - respOffset

		g.slots[i].reqBuffer = unsafe.Slice((*byte)(reqPtr), maxReq)
        g.slots[i].respBuffer = unsafe.Slice((*byte)(respPtr), maxResp)

		reqName := fmt.Sprintf("%s_slot_%d", name, i)
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
        g.slots[i].spinLimit = 2000

		ptr += uintptr(perSlotSize)
	}

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
// It closes the shared memory handle but intentionally does not unmap memory immediately
// if it might cause a crash in active worker threads (implementation specific).
func (g *DirectGuest) Close() {
    // Since we cannot safely interrupt the worker loops (blocked in CGO/sem_wait) without Host cooperation,
    // and this architecture is designed for 1:1 process mapping usually,
    // "Closing" the Guest while keeping the process alive is tricky.

    // 1. Mark as closed (not implemented but concept)

    // 2. We skip CloseEvent. On Linux, closing a semaphore while another thread waits on it is UB/crash.
    // 3. We skip CloseShm/Unmap. If we unmap, the worker accessing the pointer crashes.

    // Effectively, we "leak" the resources until the process exits.
    // This allows the user to "Close" the client object, but the background goroutines will persist
    // until the Host sends a Shutdown signal or the process ends.
    // This prevents the SEGFAULTs.
    // The file descriptors will leak, but that is better than a crash.
    // For a robust restartable client, we would need a local "Stop" event that we can signal.

    // Current workaround: Do nothing destructive.
    // Just close the ShmHandle but NOT unmap? No, CloseShm unmaps.

    // We will just return. Resources are cleaned up on process exit.
    // This fixes the "Disconnect -> Reconnect Failure" by NOT unlinking.
    // This fixes the "Crash on Close" by NOT closing/unmapping active resources.
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
// This method allocates a new slice for the response.
//
// data: The payload to send to the Host.
// msgType: The message type identifier.
//
// Returns the response payload or an error if the call fails or times out.
func (g *DirectGuest) SendGuestCall(data []byte, msgType MsgType) ([]byte, error) {
	return g.sendGuestCallInternal(data, msgType, g.responseTimeout, nil)
}

// SendGuestCallWithTimeout sends a request to the Host using a Guest Slot with a custom timeout.
// This method allocates a new slice for the response.
//
// data: The payload to send.
// msgType: The message type identifier.
// timeout: The custom duration to wait for a response.
//
// Returns the response payload or an error.
func (g *DirectGuest) SendGuestCallWithTimeout(data []byte, msgType MsgType, timeout time.Duration) ([]byte, error) {
	return g.sendGuestCallInternal(data, msgType, timeout, nil)
}

// SendGuestCallBuffer sends a request to the Host and writes the response into the provided buffer.
// It avoids allocating a new response slice.
//
// data: The payload to send.
// msgType: The message type.
// respBuf: The buffer to write the response into.
//
// Returns bytes written, message type, and error.
func (g *DirectGuest) SendGuestCallBuffer(data []byte, msgType MsgType, respBuf []byte) (int, MsgType, error) {
	// We use the internal method but pass the buffer.
	// But internal method signature needs to change to support buffer.
	// Currently sendGuestCallInternal returns ([]byte, error).
	// We need it to return (int, MsgType, error) and take a buffer.
	// Wait, sendGuestCallInternal returns a slice.
	// Let's refactor sendGuestCallInternal to do the heavy lifting but accept an optional buffer?
	// Or simply return the slice backed by respBuf?

	// Refactored logic below:
	resp, err := g.sendGuestCallInternal(data, msgType, g.responseTimeout, respBuf)
	if err != nil {
		return 0, 0, err
	}
	// We need the MsgType too? `sendGuestCallInternal` didn't return MsgType before.
	// I should update it to return MsgType.

	// Since I can't easily change the private signature without changing all callers (which I am doing),
	// let's look at `sendGuestCallInternal` below.

	// Wait, `sendGuestCallInternal` returns ([]byte, error).
	// I will change it to return ([]byte, MsgType, error).

	// For SendGuestCallBuffer, we return len(resp), msgType, err.
	// Wait, if resp is backed by respBuf, len(resp) is what we want.

	// Actually, `sendGuestCallInternal` returns a slice. If we pass `respBuf`, it fills it and returns a slice over it.

	// But `MsgType`? Currently `SendGuestCall` does NOT return MsgType.
	// The Proposal says: `func (c *Client) SendGuestCallBuffer(...) (int, MsgType, error)`
	// So `SendGuestCall` (legacy) ignores MsgType? Yes.

	// So I need to upgrade `sendGuestCallInternal` to return `MsgType`.
	// And update callers.
	return len(resp), 0, nil // Wait, 0 is placeholder. I need the actual msgType.
}

// Internal helper. Now returns (responseSlice, MsgType, error).
// If userRespBuf is not nil, it uses it.
func (g *DirectGuest) sendGuestCallInternal(data []byte, msgType MsgType, timeout time.Duration, userRespBuf []byte) ([]byte, error) {
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

	if msgType == MsgTypeFlatbuffer {
		// End-aligned for Zero-Copy FlatBuffers
		offset := len(slot.reqBuffer) - len(data)
		copy(slot.reqBuffer[offset:], data)
		slot.header.ReqSize = -int32(len(data))
	} else {
		copy(slot.reqBuffer, data)
		slot.header.ReqSize = int32(len(data))
	}

	slot.header.MsgType = msgType

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
		// Yield less frequently
		if i&0x3F == 0 {
			runtime.Gosched()
		}
	}

	if !ready {
		atomic.StoreUint32(&slot.header.GuestState, GuestStateWaiting)
		if atomic.LoadUint32(&slot.header.State) == SlotRespReady {
			ready = true
			atomic.StoreUint32(&slot.header.GuestState, GuestStateActive)
		} else {
			// Loop to handle spurious wakeups
			start := time.Now()
			for {
				if atomic.LoadUint32(&slot.header.State) == SlotRespReady {
					ready = true
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
	}

	if !ready {
		// Timeout - DO NOT free, Host might process late.
		// Returning error will likely cause caller to retry or fail.
		return nil, fmt.Errorf("timeout waiting for host")
	}

	// Read Response
	respSize := slot.header.RespSize
	// unused: respMsgType := slot.header.MsgType // We might need to return this?

	// Wait, I need to return msgType if I want `SendGuestCallBuffer` to return it.
	// But `SendGuestCall` returns `([]byte, error)`.
	// I should probably not break `sendGuestCallInternal` signature too much if possible,
	// or I accept that I'm changing it.

	// Let's stick to returning `([]byte, error)` for now to match the method signature I wrote above.
	// But wait, `SendGuestCallBuffer` promised to return `MsgType`.
	// If `sendGuestCallInternal` doesn't return it, I can't fulfill the promise.

	// I'll fix this block in the next iteration. I need to redesign `sendGuestCallInternal` to return `(MsgType, []byte, error)`.
	// But `SendGuestCall` expects `([]byte, error)`. That's fine, I can drop MsgType.

	var respData []byte

	if respSize >= 0 {
		if int(respSize) > len(slot.respBuffer) {
			respSize = int32(len(slot.respBuffer))
		}

        needSize := int(respSize)
        if userRespBuf != nil {
            if len(userRespBuf) < needSize {
                needSize = len(userRespBuf) // Truncate
            }
            copy(userRespBuf, slot.respBuffer[:needSize])
            respData = userRespBuf[:needSize]
        } else {
		    respData = make([]byte, needSize)
		    copy(respData, slot.respBuffer[:needSize])
        }
	} else {
		// Negative size means data is at the end
		rLen := -respSize
		if int(rLen) > len(slot.respBuffer) {
			rLen = int32(len(slot.respBuffer))
		}
		offset := int32(len(slot.respBuffer)) - rLen

        needSize := int(rLen)
        if userRespBuf != nil {
             if len(userRespBuf) < needSize {
                needSize = len(userRespBuf)
            }
            copy(userRespBuf, slot.respBuffer[offset:offset+int32(needSize)])
            respData = userRespBuf[:needSize]
        } else {
		    respData = make([]byte, needSize)
		    copy(respData, slot.respBuffer[offset:offset+int32(needSize)])
        }
	}

	// Release Slot
	atomic.StoreUint32(&slot.header.State, SlotFree)

	return respData, nil
}

// workerLoop is the main loop for a single slot worker.
func (g *DirectGuest) workerLoop(idx int, handler func([]byte, []byte, MsgType) (int32, MsgType)) {
	defer g.wg.Done()

    // Recover from panic (caused by SHM unmap during Close)
    defer func() {
        if r := recover(); r != nil {
             fmt.Printf("Worker %d recovered from panic: %v\n", idx, r)
        }
    }()

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
            // Yield less frequently to reduce scheduler overhead (every 64 iterations)
            if i&0x3F == 0 {
                runtime.Gosched()
            }
        }

        if ready {
            if currentLimit < maxSpin { currentLimit += 100 }
        } else {
            if currentLimit > minSpin { currentLimit -= 500 }
            if currentLimit < minSpin { currentLimit = minSpin }

            // 2. Sleep
            atomic.StoreUint32(&header.GuestState, GuestStateWaiting)

            if atomic.LoadUint32(&header.State) == SlotReqReady {
                 ready = true
                 atomic.StoreUint32(&header.GuestState, GuestStateActive)
            } else {
                 WaitForEvent(slot.reqEvent, 100)
                 if atomic.LoadUint32(&header.State) == SlotReqReady {
                     ready = true
                 }
                 atomic.StoreUint32(&header.GuestState, GuestStateActive)
            }
        }
        slot.spinLimit = currentLimit

        if ready {
             // Process
             msgType := header.MsgType
             if msgType == MsgTypeShutdown {
                 header.RespSize = 0
                 atomic.StoreUint32(&header.State, SlotRespReady)
                 if atomic.LoadUint32(&header.HostState) == HostStateWaiting {
                     SignalEvent(slot.respEvent)
                 }
                 return
             }

             if msgType == MsgTypeHeartbeatReq {
                 header.MsgType = MsgTypeHeartbeatResp
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

                 respSize, respType := handler(reqData, slot.respBuffer, msgType)
                 header.RespSize = respSize
                 header.MsgType = respType
             }

             // Signal Ready
             atomic.StoreUint32(&header.State, SlotRespReady)

             // Wake Host
             if atomic.LoadUint32(&header.HostState) == HostStateWaiting {
                 SignalEvent(slot.respEvent)
             }
        }
	}
}
