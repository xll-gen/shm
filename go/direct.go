package shm

import (
	"fmt"
	"log"
	"runtime"
	"sync"
	"sync/atomic"
	"time"
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

const (
	ScannerStateActive   = 0
	ScannerStateSleeping = 1
)

// ExchangeHeader matching C++
type ExchangeHeader struct {
	NumSlots          uint32
	SlotSize          uint32
	HostScannerState  uint32
	GuestScannerState uint32
	GuestHeartbeat    uint64
	HostHeartbeat     uint64
	_                 [32]byte // Padding
}

type DirectGuest struct {
	shmBase        uintptr
	shmSize        uint64
	handle         ShmHandle
	numSlots       uint32
	slotSize       uint32
	exchangeHeader *ExchangeHeader

	slots []slotContext
	wg    sync.WaitGroup

	// Channels for waking up workers
	workerChans []chan bool

	// Global Guest Event (Scanner waits on this)
	globalEvent EventHandle

	// Global Host Event (Scanner signals this)
	globalHostEvent EventHandle

	running int32
}

type slotContext struct {
	header   *SlotHeader
	reqData  []byte
	respData []byte
	// No per-slot event needed for waking up worker, we use channel
}

func NewDirectGuest(name string) (*DirectGuest, error) {
	// 1. First Pass: Map just the header (4KB page) to read layout
	preliminaryTotalSize := uint64(4096)
	h, addr, err := OpenShm(name, preliminaryTotalSize)
	if err != nil {
		return nil, err
	}

	// Read Header
	tempHeader := (*ExchangeHeader)(unsafe.Pointer(addr))
	if tempHeader.NumSlots == 0 {
		CloseShm(h, addr)
		return nil, fmt.Errorf("host not ready (header not initialized)")
	}

	numSlots := int(tempHeader.NumSlots)
	slotSize := int(tempHeader.SlotSize)

	// Close temporary mapping
	CloseShm(h, addr)

	// 2. Second Pass: Calculate real size and remap
	headerSize := uint64(unsafe.Sizeof(ExchangeHeader{}))
	if headerSize < 64 {
		headerSize = 64
	}
	slotHeaderSize := uint64(unsafe.Sizeof(SlotHeader{}))
	if slotHeaderSize < 128 {
		slotHeaderSize = 128
	}
	perSlotSize := slotHeaderSize + uint64(slotSize)*2
	totalSize := headerSize + (perSlotSize * uint64(numSlots))

	// Map full size
	h, addr, err = OpenShm(name, totalSize)
	if err != nil {
		return nil, fmt.Errorf("failed to map full shm: %v", err)
	}
	exchangeHeader := (*ExchangeHeader)(unsafe.Pointer(addr))

	// Open Global Events
	gEvent, err := OpenEvent(name + "_event_guest")
	if err != nil {
		CloseShm(h, addr)
		return nil, fmt.Errorf("failed to open global guest event: %v", err)
	}

	hEvent, err := OpenEvent(name + "_event_host")
	if err != nil {
		CloseEvent(gEvent)
		CloseShm(h, addr)
		return nil, fmt.Errorf("failed to open global host event: %v", err)
	}

	g := &DirectGuest{
		shmBase:         addr,
		shmSize:         totalSize,
		handle:          h,
		numSlots:        uint32(numSlots),
		slotSize:        uint32(slotSize),
		exchangeHeader:  exchangeHeader,
		slots:           make([]slotContext, numSlots),
		workerChans:     make([]chan bool, numSlots),
		globalEvent:     gEvent,
		globalHostEvent: hEvent,
		running:         1,
	}

	ptr := addr + uintptr(headerSize)
	for i := 0; i < numSlots; i++ {
		g.slots[i].header = (*SlotHeader)(unsafe.Pointer(ptr))

		// Data Pointers
		// ReqData starts after Header
		reqPtr := unsafe.Pointer(ptr + uintptr(slotHeaderSize))
		g.slots[i].reqData = unsafe.Slice((*byte)(reqPtr), slotSize)

		// RespData starts after ReqData
		respPtr := unsafe.Pointer(ptr + uintptr(slotHeaderSize) + uintptr(slotSize))
		g.slots[i].respData = unsafe.Slice((*byte)(respPtr), slotSize)

		// Create channel (buffered 1)
		g.workerChans[i] = make(chan bool, 1)

		ptr += uintptr(perSlotSize)
	}

	return g, nil
}

// Start spawns workers and the scanner.
func (g *DirectGuest) Start(handler func([]byte, []byte) int, ready chan<- struct{}) {
	// Start Workers
	for i := 0; i < int(g.numSlots); i++ {
		g.wg.Add(1)
		go g.workerLoop(i, handler)
	}

	// Start Scanner
	g.wg.Add(1)
	go g.scannerLoop()

	// Start Heartbeat
	g.wg.Add(1)
	go g.heartbeatLoop()

	close(ready)
}

func (g *DirectGuest) heartbeatLoop() {
	defer func() {
		if r := recover(); r != nil {
			log.Println("Recovered in heartbeatLoop", r)
		}
		g.wg.Done()
	}()
	for atomic.LoadInt32(&g.running) == 1 {
		atomic.AddUint64(&g.exchangeHeader.GuestHeartbeat, 1)
		time.Sleep(1 * time.Second)
	}
}

func (g *DirectGuest) Wait() {
	g.wg.Wait()
}

func (g *DirectGuest) Close() {
	atomic.StoreInt32(&g.running, 0)
	CloseShm(g.handle, g.shmBase)
	CloseEvent(g.globalEvent)
	CloseEvent(g.globalHostEvent)
}

// scannerLoop polls all slots and wakes up workers
func (g *DirectGuest) scannerLoop() {
	defer func() {
		if r := recover(); r != nil {
			log.Println("Recovered in scannerLoop", r)
		}
		g.wg.Done()
	}()
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()

	// Initialize Scanner State
	atomic.StoreUint32(&g.exchangeHeader.GuestScannerState, ScannerStateActive)

	for atomic.LoadInt32(&g.running) == 1 {
		worked := false
		// Scan all slots
		for i := 0; i < int(g.numSlots); i++ {
			// Check if slot is ReqReady
			s := atomic.LoadUint32(&g.slots[i].header.State)
			if s == SlotReqReady {
				// Signal Worker
				// Non-blocking send to avoid hanging if worker is already busy (shouldn't happen in correct flow)
				select {
				case g.workerChans[i] <- true:
				default:
				}
				worked = true
			}
		}

		if !worked {
			// Adaptive Wait Strategy
            // 1. Spin Phase
            foundSpin := false
            for spin := 0; spin < 10000; spin++ {
                 runtime.Gosched() // Use Gosched instead of CpuRelax equivalent for Go
                 for i := 0; i < int(g.numSlots); i++ {
				    if atomic.LoadUint32(&g.slots[i].header.State) == SlotReqReady {
					    foundSpin = true
					    break
				    }
			    }
                if foundSpin { break }
            }
            if foundSpin { continue }

			// 2. Set Sleeping
			atomic.StoreUint32(&g.exchangeHeader.GuestScannerState, ScannerStateSleeping)

			// 3. Check again (Double check pattern)
			found := false
			for i := 0; i < int(g.numSlots); i++ {
				if atomic.LoadUint32(&g.slots[i].header.State) == SlotReqReady {
					found = true
					break
				}
			}

			if found {
				atomic.StoreUint32(&g.exchangeHeader.GuestScannerState, ScannerStateActive)
				continue
			}

			// 4. Wait
			WaitForEvent(g.globalEvent, 1) // 1ms timeout for responsiveness
			atomic.StoreUint32(&g.exchangeHeader.GuestScannerState, ScannerStateActive)
		} else {
            // Yield if we did work? No, keep going if we have load.
            // Maybe cpu relax.
             runtime.Gosched()
        }
	}
}

func (g *DirectGuest) workerLoop(idx int, handler func([]byte, []byte) int) {
	defer func() {
		if r := recover(); r != nil {
			log.Printf("Recovered in workerLoop %d: %v\n", idx, r)
		}
		g.wg.Done()
	}()
	runtime.LockOSThread()
	defer runtime.UnlockOSThread()
	slot := &g.slots[idx]
	header := slot.header
	ch := g.workerChans[idx]

	for atomic.LoadInt32(&g.running) == 1 {
		// Drain any stale signals from the channel first.
		// This can happen if the scanner is much faster than the worker
		// and signals multiple times for the same request.
	drain_loop:
		for {
			select {
			case <-ch:
				// drained a signal
			default:
				break drain_loop
			}
		}

		// Wait for signal from Scanner
		<-ch

		state := atomic.LoadUint32(&header.State)
		if state != SlotReqReady {
			continue
		}

		// Process Request
		msgId := header.MsgId
		if msgId == MsgIdShutdown {
			return
		}

		// Handle Heartbeat
		if msgId == MsgIdHeartbeatReq {
			header.MsgId = MsgIdHeartbeatResp
			header.RespSize = 0
		} else {
			// Normal Request
			reqSize := header.ReqSize
			// Range check
			if reqSize > g.slotSize {
				reqSize = g.slotSize
			}

			reqData := slot.reqData[:reqSize]

			// Call handler (Zero Copy)
			// User writes directly to respData
			n := handler(reqData, slot.respData)

			header.RespSize = uint32(n)
		}

		// 4. Signal Ready (Response Ready)
		atomic.StoreUint32(&header.State, SlotRespReady)

		// Wake up Host Scanner if sleeping
		// We read HostScannerState
		// Note: ExchangeHeader pointer might be nil if not set up correctly, but NewDirectGuest sets it.
		hostState := atomic.LoadUint32(&g.exchangeHeader.HostScannerState)
		if hostState == ScannerStateSleeping {
			SignalEvent(g.globalHostEvent)
		}

		// 5. Wait for Host Done (Deadlock Fix)
		// We spin here because Host should be fast.
		// Fallback to sleep if too long?
		spins := 0
		for atomic.LoadUint32(&header.State) != SlotHostDone {
			spins++
			if spins > 1000 {
				runtime.Gosched()
				spins = 0
			}
			// Check for shutdown
			if atomic.LoadInt32(&g.running) == 0 {
				return
			}
		}

		// 6. Set Free
		atomic.StoreUint32(&header.State, SlotFree)
	}
}
