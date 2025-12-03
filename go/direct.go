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
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	defer g.wg.Done()
	slot := &g.slots[idx]
	header := slot.header

	// Ensure we start fresh
	atomic.StoreUint32(&header.State, SlotFree)

    // Simplified Polling Loop (Aggressive)
	for {
        // Set to Polling immediately (we are always polling)
        // If we were in Free, we move to Polling.
        // If we were in HostDone, we move to Polling.
        // We only transition Free -> Polling once here if needed, but really we just loop.

        // Wait for ReqReady
        for {
             s := atomic.LoadUint32(&header.State)
             if s == SlotReqReady {
                 break
             }
             if s == SlotFree {
                 // Try to advertise we are polling
                 atomic.CompareAndSwapUint32(&header.State, SlotFree, SlotPolling)
             }
             // Busy wait - no sleep, no yield
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

        // In pure busy-loop mode, Host is likely spinning, so HostState might stay Active.
        // But if Host *did* go to sleep (shouldn't happened with our C++ changes), we signal.
		if atomic.LoadUint32(&header.HostState) == HostStateWaiting {
			SignalEvent(slot.respEvent)
		}

		// 5. Wait for Host Done
		for atomic.LoadUint32(&header.State) != SlotHostDone {
			// Busy wait
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
