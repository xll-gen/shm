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

const (
	MsgIdNormal        = 0
	MsgIdHeartbeatReq  = 1
	MsgIdHeartbeatResp = 2
	MsgIdShutdown      = 3
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
// Reference: include/IPCUtils.h
// struct ExchangeHeader {
//     uint32_t numSlots;
//     uint32_t slotSize;
//     uint8_t padding[56]; // Padding to 64 bytes
// };
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

func NewDirectGuest(name string, _ int, _ int) (*DirectGuest, error) {
	// Step 1: Map the first page (4KB) to read the header
	const HeaderMapSize = 4096
	h, addr, err := OpenShm(name, HeaderMapSize)
	if err != nil {
		return nil, err
	}

	// Read ExchangeHeader
	header := (*ExchangeHeader)(unsafe.Pointer(addr))

	numSlots := header.NumSlots
	slotSize := header.SlotSize

	// Basic validation: if 0, Host hasn't initialized yet.
	if numSlots == 0 || slotSize == 0 {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("shared memory not ready (slots/size is 0)")
	}

	// Unmap header
	CloseShm(h, addr, HeaderMapSize)

	// Step 2: Calculate full size and remap
	headerSize := uint64(unsafe.Sizeof(ExchangeHeader{}))
    if headerSize < 64 { headerSize = 64 }

    slotHeaderSize := uint64(unsafe.Sizeof(SlotHeader{}))
    if slotHeaderSize < 128 { slotHeaderSize = 128 }

	perSlotSize := slotHeaderSize + uint64(slotSize)
	totalSize := headerSize + (perSlotSize * uint64(numSlots))

	h, addr, err = OpenShm(name, totalSize)
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
		numSlots:      numSlots,
		slotSize:      slotSize,
		slots:         make([]slotContext, numSlots),
		targetPollers: target,
	}

	ptr := addr + uintptr(headerSize)
	for i := 0; i < int(numSlots); i++ {
		g.slots[i].header = (*SlotHeader)(unsafe.Pointer(ptr))

		dataPtr := unsafe.Pointer(ptr + uintptr(slotHeaderSize))
		g.slots[i].data = unsafe.Slice((*byte)(dataPtr), slotSize)

		eventName := fmt.Sprintf("%s_slot_%d", name, i)
		ev, err := OpenEvent(eventName)
		if err != nil {
			g.Close()
			return nil, err
		}
		g.slots[i].event = ev

		respName := fmt.Sprintf("%s_slot_%d_resp", name, i)
		evResp, err := OpenEvent(respName)
		if err != nil {
            // Response event might not exist if Host doesn't use it (busy polling).
            // We ignore this error and just don't signal response.
            g.slots[i].respEvent = 0
		} else {
		    g.slots[i].respEvent = evResp
        }

		ptr += uintptr(perSlotSize)
	}

	return g, nil
}

// Implement Transport Interface

func (g *DirectGuest) Send(data []byte) {
	// Not used in Direct Mode loop
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
	CloseShm(g.handle, g.shmBase, g.shmSize)
	for _, s := range g.slots {
		if s.event != 0 {
			CloseEvent(s.event)
		}
		if s.respEvent != 0 {
			CloseEvent(s.respEvent)
		}
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
			// Check if already ReqReady before setting Polling
			// Using CAS from SlotFree to SlotPolling
			if !atomic.CompareAndSwapUint32(&header.State, SlotFree, SlotPolling) {
				// Failed CAS
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
            if slot.respEvent != 0 {
			    SignalEvent(slot.respEvent)
            }
		}

		// 5. Wait for Host Done
		for atomic.LoadUint32(&header.State) != SlotHostDone {
			runtime.Gosched()
		}
	}
}
