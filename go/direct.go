package shm

import (
	"fmt"
	"runtime"
	"sync"
	"sync/atomic"
	"unsafe"
)

// SlotState constants matching C++
const (
	SlotFree      = 0
	SlotPolling   = 1
	SlotBusy      = 2
	SlotReqReady  = 3
	SlotRespReady = 4
	SlotHostDone  = 5
)

// SlotHeader matching C++
type SlotHeader struct {
    _         [64]byte // Pre-padding
	State     uint32
	ReqSize   uint32
	RespSize  uint32
	MsgId     uint32
	HostState uint32   // Atomic
	_         [44]byte // Padding
}

const (
	HostStateActive  = 0
	HostStateWaiting = 1
)

// ExchangeHeader matching C++
type ExchangeHeader struct {
	NumSlots uint32
	SlotSize uint32
	_        [56]byte // Padding
}

type DirectGuest struct {
	shmBase  uintptr
	shmSize  uint64
	handle   ShmHandle
	numSlots uint32
	slotSize uint32

	slots []slotContext
	wg    sync.WaitGroup

	targetPollers int32 // atomic
	activePollers int32 // atomic
}

type slotContext struct {
	header    *SlotHeader
	data      []byte
	event     EventHandle // Req Event
	respEvent EventHandle // Resp Event
}

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

	// Initialize target pollers to max(1, NumCPU/4)
    target := int32(1) // Force 1 for container safety
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

// Implement Transport Interface

func (g *DirectGuest) Send(data []byte) {
	// Direct mode does not support arbitrary 'Send'.
	// It only sends 'Response' to the current slot being processed.
	// However, the unified 'Client' expects to call 'Send' with the response data
	// AFTER the handler returns.
	// But 'Start' calls the handler.
	// In 'DirectGuest', the worker loop calls the handler and writes the response IMMEDIATELY to the slot.
	// This means DirectGuest CANNOT implement generic 'Send' easily unless 'Start' manages the flow differently.

	// FIX: The 'Client' implementation I wrote calls 'Start' with a wrapper handler that calls 'handler' then 'Send'.
	// This works for Queue mode (read -> handle -> write).
	// But for Direct mode, the 'workerLoop' handles the write step implicitly.
	// Therefore, DirectGuest's 'Start' should take the 'handler' and do everything.
	// BUT, 'Client' is trying to be generic.

	// Solution: 'DirectGuest.Send' is a no-op or panic because DirectGuest.Start() handles the response writing internally?
	// No, the 'handler' passed to Start returns void in Client's wrapper?
	// Client.Start passes `func(data)` to Transport.Start.
	// This function calls user handler, gets result, and calls Transport.Send(result).

	// This flow breaks DirectGuest which expects handler to return []byte.

	// Refactoring Client:
	// Let Transport.Start take `func([]byte) []byte`?
	// If Queue mode, it reads, calls handler, sends result.
	// If Direct mode, it reads, calls handler, writes result to slot.
	// This is much better for Direct mode efficiency too (no extra buffer copy/Send call).

	// Let's change the plan for Client slightly.
	// Transport.Start(handler func([]byte) []byte)
}

// Start spawns workers. Each worker is pinned to a slot index.
func (g *DirectGuest) Start(handler func([]byte) []byte) {
	for i := 0; i < int(g.numSlots); i++ {
        g.wg.Add(1)
		go g.workerLoop(i, handler)
	}
}

// StartWithResponder is an alias for Start
func (g *DirectGuest) StartWithResponder(handler func([]byte) []byte) {
    g.Start(handler)
}

// Wait blocks until all workers have exited (via Shutdown message).
func (g *DirectGuest) Wait() {
    g.wg.Wait()
}

func (g *DirectGuest) Close() {
	CloseShm(g.handle, g.shmBase)
	for _, s := range g.slots {
		CloseEvent(s.event)
		CloseEvent(s.respEvent)
	}
}

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
			atomic.StoreUint32(&header.State, SlotPolling)

			// 2. Spin wait
			spins := 0
			// Spin limit: 100k iterations (approx 10-50us depending on machine)
			const SpinLimit = 100000
			for spins < SpinLimit {
				s := atomic.LoadUint32(&header.State)
				if s == SlotReqReady {
					found = true
					break
				}
				spins++
				// Small pause to be friendly
				if spins%100 == 0 {
					runtime.Gosched()
				}
			}

			atomic.AddInt32(&g.activePollers, -1)

			if !found {
				// Transition to Free (Sleeping)
				if !atomic.CompareAndSwapUint32(&header.State, SlotPolling, SlotFree) {
					// Failed means state changed -> ReqReady
					found = true
				}
			}
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
		for atomic.LoadUint32(&header.State) != SlotHostDone {
			runtime.Gosched()
		}
	}
}
