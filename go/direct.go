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
	State    uint32
	ReqSize  uint32
	RespSize uint32
	MsgId    uint32
	_        [48]byte // Padding
}

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
}

type slotContext struct {
	header *SlotHeader
	data   []byte
	event  EventHandle
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

	g := &DirectGuest{
		shmBase:  addr,
		shmSize:  totalSize,
		handle:   h,
		numSlots: uint32(numSlots),
		slotSize: uint32(slotSize),
		slots:    make([]slotContext, numSlots),
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
		// 1. Enter Polling State
		atomic.StoreUint32(&header.State, SlotPolling)

		// 2. Spin wait
		spins := 0
		found := false
		for spins < 1000000 { // heuristic
			s := atomic.LoadUint32(&header.State)
			if s == SlotReqReady {
				found = true
				break
			}
			if s != SlotPolling && s != SlotBusy {
				// Something weird or transition happening
			}
			spins++
			runtime.Gosched()
		}

		if !found {
			// Transition to Free (Sleeping)
			if !atomic.CompareAndSwapUint32(&header.State, SlotPolling, SlotFree) {
				// Failed means state changed. Likely to Busy or ReqReady?
				for atomic.LoadUint32(&header.State) != SlotReqReady {
					runtime.Gosched()
				}
			} else {
				// Successfully set to Free. Wait on Event.
				WaitForEvent(slot.event, 0xFFFFFFFF) // Infinite wait

				if atomic.LoadUint32(&header.State) != SlotReqReady {
					continue
				}
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
	}
}
