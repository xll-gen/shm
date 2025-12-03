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
	perSlotSize := uint64(unsafe.Sizeof(SlotHeader{})) + uint64(slotSize)
	totalSize := headerSize + (perSlotSize * uint64(numSlots))

	h, addr, err := OpenShm(name, totalSize)
	if err != nil {
		return nil, err
	}

	// Initialize target pollers to max(1, NumCPU/4)
	target := int32(runtime.NumCPU() / 4)
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

		dataPtr := unsafe.Pointer(ptr + unsafe.Sizeof(SlotHeader{}))
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

// Start spawns workers. Each worker is pinned to a slot index.
func (g *DirectGuest) Start(handler func([]byte) []byte) {
	for i := 0; i < int(g.numSlots); i++ {
        g.wg.Add(1)
		go g.workerLoop(i, handler)
	}
}

// Wait blocks until all workers have exited (via Shutdown message).
func (g *DirectGuest) Wait() {
    g.wg.Wait()
}

func (g *DirectGuest) workerLoop(idx int, handler func([]byte) []byte) {
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

			// If we found it, we keep activePollers incremented until we finish processing?
			// Actually, if we are processing, we are effectively "busy" but not "polling".
			// But for simplicity of "max CPU usage", processing also consumes CPU.
			// However, the "poller count" usually restricts the *busy wait* phase.
			// Once we have work, we should probably release the slot token so another thread can poll?
			// But if we release, another thread wakes up and spins, potentially oversubscribing CPUs if we are also CPU bound processing.
			// BUT: The requirement is "limit busy loop workers". Processing is useful work.
			// So we should decrement activePollers once we exit the spin loop (success or fail).
			atomic.AddInt32(&g.activePollers, -1)

			if !found {
				// Failed to find work while spinning.
				// This implies work is sparse. Decrease target to save CPU.
				// Decrease logic: if target > 1, decrement.
				// To avoid rapid oscillation, maybe probabilistic? Or just do it.
				// Let's do it simply.
				curTarget := atomic.LoadInt32(&g.targetPollers)
				if curTarget > 1 {
					atomic.CompareAndSwapInt32(&g.targetPollers, curTarget, curTarget-1)
				}

				// Transition to Free (Sleeping)
				if !atomic.CompareAndSwapUint32(&header.State, SlotPolling, SlotFree) {
					// Failed means state changed -> ReqReady
					found = true
				}
			}
		}

		if !found {
			// Wait on Event
			WaitForEvent(slot.event, 0xFFFFFFFF)

			// We woke up. This means there is work.
			// Increase target pollers because we were forced to sleep.
			curTarget := atomic.LoadInt32(&g.targetPollers)
			if curTarget < int32(g.numSlots) { // Cap at numSlots or NumCPU?
				// Cap at numSlots is logical max.
				atomic.CompareAndSwapInt32(&g.targetPollers, curTarget, curTarget+1)
			}
		}

		// Wait until state is definitively ReqReady (in case of race after wake)
		for atomic.LoadUint32(&header.State) != SlotReqReady {
			runtime.Gosched()
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

func (g *DirectGuest) Close() {
	CloseShm(g.handle, g.shmBase)
	for _, s := range g.slots {
		CloseEvent(s.event)
		CloseEvent(s.respEvent)
	}
}
