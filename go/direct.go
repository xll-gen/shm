package shm

import (
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"unsafe"
)

// SlotState constants matching C++ definitions.
const (
	SlotFree      = 0 // Slot is free for use.
	SlotPolling   = 1 // Guest is polling this slot.
	SlotBusy      = 2 // Host is writing to this slot.
	SlotReqReady  = 3 // Request is ready for Guest.
	SlotRespReady = 4 // Response is ready for Host.
	SlotHostDone  = 5 // Host has read response.
)

// SlotHeader defines the layout of a slot in shared memory.
// It matches the C++ SlotHeader struct with explicit padding for cache line alignment.
type SlotHeader struct {
    _         [64]byte // Pre-padding to avoid false sharing
	State     uint32   // Atomic state
	ReqSize   uint32   // Size of request data
	RespSize  uint32   // Size of response data
	MsgId     uint32   // Message ID
	HostState uint32   // Atomic Host State (0=Active, 1=Waiting)
	_         [44]byte // Padding to 128 bytes
}

// HostState constants.
const (
	HostStateActive  = 0
	HostStateWaiting = 1
)

// ExchangeHeader describes the layout of the direct exchange region.
// (Legacy/Reserved)
type ExchangeHeader struct {
	NumSlots uint32
	SlotSize uint32
	_        [56]byte // Padding
}

// DirectGuest implements the Guest side of the Direct IPC mode.
//
// It manages a set of workers, each pinned to a specific slot index ("Lane").
// This mode provides the lowest latency by eliminating queue overhead.
type DirectGuest struct {
	shmBase  uintptr
	shmSize  uint64
	handle   ShmHandle
	numSlots uint32
	slotSize uint32

	slots []slotContext
	wg    sync.WaitGroup

	targetPollers int32 // atomic: Desired number of active spinning pollers
	activePollers int32 // atomic: Current number of active spinning pollers
}

// slotContext holds runtime data for a single slot/lane.
type slotContext struct {
	header    *SlotHeader
	data      []byte
	event     EventHandle // Event to wake Guest (ReqReady)
	respEvent EventHandle // Event to wake Host (RespReady)
}

// NewDirectGuest creates a new Direct IPC Guest.
//
// Parameters:
//   - name: Shared memory name.
//   - numSlots: Number of slots/lanes to support.
//   - slotSize: Size of data buffer per slot.
//
// Returns:
//   - *DirectGuest: The instance.
//   - error: If SHM or Events cannot be opened.
func NewDirectGuest(name string, numSlots int, slotSize int) (*DirectGuest, error) {
	// Calculate size
	headerSize := uint64(unsafe.Sizeof(ExchangeHeader{}))
    if headerSize < 64 { headerSize = 64 }

    slotHeaderSize := uint64(unsafe.Sizeof(SlotHeader{}))
    if slotHeaderSize < 128 { slotHeaderSize = 128 }

	perSlotSize := slotHeaderSize + uint64(slotSize)
	totalSize := headerSize + (perSlotSize * uint64(numSlots))

	h, addr, err := OpenShm(name, totalSize)
	if err != nil {
		return nil, err
	}

	// Initialize target pollers to 1 (container safe default)
    target := int32(1)
	if target < 1 {
		target = 1
	}

	g := &DirectGuest{
		shmBase:       addr,
		shmSize:       totalSize,
		handle:        h,
		numSlots:      uint32(numSlots),
		slotSize:      uint32(slotSize),
		slots:         make([]slotContext, numSlots),
		targetPollers: target,
	}

	ptr := addr + uintptr(headerSize)
	for i := 0; i < numSlots; i++ {
		g.slots[i].header = (*SlotHeader)(unsafe.Pointer(ptr))

		dataPtr := unsafe.Pointer(ptr + uintptr(slotHeaderSize))
		g.slots[i].data = unsafe.Slice((*byte)(dataPtr), slotSize)

		eventName := fmt.Sprintf("%s_slot_%d", name, i)
		ev, err := OpenEvent(eventName)
		if err != nil {
			return nil, err
		}
		g.slots[i].event = ev

		respName := fmt.Sprintf("%s_slot_%d_resp", name, i)
		evResp, err := OpenEvent(respName)
		if err != nil {
			return nil, err
		}
		g.slots[i].respEvent = evResp

		ptr += uintptr(perSlotSize)
	}

	return g, nil
}

// Send is not supported in DirectGuest in the traditional sense.
// Responses are sent implicitly by the worker loop.
func (g *DirectGuest) Send(data []byte) {
	// No-op. Logic is handled in workerLoop.
}

// Start launches worker goroutines for each slot.
//
// Each worker listens for requests on its assigned slot and invokes the handler.
//
// Parameters:
//   - handler: Function to process requests.
func (g *DirectGuest) Start(handler func([]byte) []byte) {
	for i := 0; i < int(g.numSlots); i++ {
        g.wg.Add(1)
		go g.workerLoop(i, handler)
	}
}

// StartWithResponder is an alias for Start.
func (g *DirectGuest) StartWithResponder(handler func([]byte) []byte) {
    g.Start(handler)
}

// Wait blocks until all workers have finished (received Shutdown).
func (g *DirectGuest) Wait() {
    g.wg.Wait()
}

// Close releases shared memory and event handles.
func (g *DirectGuest) Close() {
	CloseShm(g.handle, g.shmBase)
	for _, s := range g.slots {
		CloseEvent(s.event)
		CloseEvent(s.respEvent)
	}
}

// workerLoop is the main loop for a single lane.
//
// It implements an adaptive spin/wait strategy:
// 1. Attempts to enter "Polling" state via CAS.
// 2. If successful, spins for a while checking for requests.
// 3. If no request arrives, reverts to "Free" and waits on the Event.
// 4. Processes request, writes response, and signals Host.
func (g *DirectGuest) workerLoop(idx int, handler func([]byte) []byte) {
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	defer g.wg.Done()
	slot := &g.slots[idx]
	header := slot.header

	// Ensure we start fresh
	atomic.StoreUint32(&header.State, SlotFree)

	for {
		// Check adaptive logic: Are we allowed to poll?
		canPoll := false
		active := atomic.LoadInt32(&g.activePollers)
		target := atomic.LoadInt32(&g.targetPollers)

		if active < target {
			if atomic.CompareAndSwapInt32(&g.activePollers, active, active+1) {
				canPoll = true
			}
		}

		found := false

		if canPoll {
			// 1. Enter Polling State
			// Check if already ReqReady before setting Polling
			// Using CAS from SlotFree to SlotPolling
			if !atomic.CompareAndSwapUint32(&header.State, SlotFree, SlotPolling) {
				// Failed CAS: Check if it was because data arrived
				s := atomic.LoadUint32(&header.State)
				if s == SlotReqReady {
					found = true
				}
			} else {
				// Successfully transitioned to Polling. Now spin.
				// 2. Spin wait
				spins := 0
				const SpinLimit = 100000
				for spins < SpinLimit {
					s := atomic.LoadUint32(&header.State)
					if s == SlotReqReady {
						found = true
						break
					}
					spins++
					if spins%100 == 0 {
						runtime.Gosched()
					}
				}

				// If still not found, try to revert to Free
				if !found {
					if !atomic.CompareAndSwapUint32(&header.State, SlotPolling, SlotFree) {
						// Failed means state changed -> ReqReady
						found = true
					}
				}
			}
			atomic.AddInt32(&g.activePollers, -1)
		}

		if !found {
			// Robust Wait Loop: Keep waiting until State is ReqReady
			for {
				s := atomic.LoadUint32(&header.State)
				if s == SlotReqReady {
					break
				}
				WaitForEvent(slot.event, 100)
			}
		} else {
             // Found while polling. Just wait for state to be definitely ReqReady
             for atomic.LoadUint32(&header.State) != SlotReqReady {
                runtime.Gosched()
             }
        }

		// 3. Process Request
		msgId := header.MsgId
		if msgId == MsgIdShutdown {
			return
		}

		// Handle Heartbeat
		if msgId == MsgIdHeartbeatReq {
			header.MsgId = MsgIdHeartbeatResp
		}

		reqSize := header.ReqSize
		reqData := slot.data[:reqSize]

		// Call handler
		respData := handler(reqData)

		respLen := uint32(len(respData))
		if respLen > g.slotSize {
			respLen = g.slotSize
		}

		copy(slot.data[:respLen], respData)
		header.RespSize = respLen

		// 4. Signal Ready
		atomic.StoreUint32(&header.State, SlotRespReady)

		// Check if Host is waiting
		if atomic.LoadUint32(&header.HostState) == HostStateWaiting {
			SignalEvent(slot.respEvent)
		}

		// 5. Wait for Host Done
		// Host sets state to SlotHostDone (or Free) when it reads the response.
		// Actually Host sets it to Free.
		// Wait until state != SlotRespReady
		for atomic.LoadUint32(&header.State) == SlotRespReady {
			runtime.Gosched()
		}
	}
}
