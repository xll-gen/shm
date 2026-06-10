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
	SlotFree = 0
	// SlotReqReady indicates the Host has written a request and it is ready for the Guest.
	SlotReqReady = 1
	// SlotRespReady indicates the Guest has written a response and it is ready for the Host.
	SlotRespReady = 2
	// SlotDone is a transient state indicating transaction completion.
	// SLOT_DONE = 3 (reserved; not currently used by the protocol but
	// retained for future flow extensions).
	SlotDone = 3
	// SlotBusy indicates the Host has claimed the slot and is writing data.
	SlotBusy = 4
	// SlotGuestBusy indicates the Guest has claimed the slot and is writing data.
	SlotGuestBusy = 5

	// MsgTypeNormal is a standard data payload message.
	MsgTypeNormal MsgType = 0
	// MsgTypeHeartbeatReq is a keep-alive request from the Host.
	MsgTypeHeartbeatReq MsgType = 1
	// MsgTypeHeartbeatResp is the response to a keep-alive request.
	MsgTypeHeartbeatResp MsgType = 2
	// MsgTypeShutdown signals the Guest to terminate.
	MsgTypeShutdown MsgType = 3
	// MsgTypeFlatbuffer indicates a Zero-Copy FlatBuffer payload.
	MsgTypeFlatbuffer MsgType = 10
	// MsgTypeGuestCall indicates a Guest Call payload.
	MsgTypeGuestCall MsgType = 11
	// MsgTypeStreamStart indicates a Stream Start payload.
	MsgTypeStreamStart MsgType = 13
	// MsgTypeStreamChunk indicates a Stream Chunk payload.
	MsgTypeStreamChunk MsgType = 14
	// MsgTypeSystemError indicates a system-level error (e.g. overflow).
	MsgTypeSystemError MsgType = 127
	// MsgTypeAppStart is the start of Application Specific message types.
	// Types below 128 are reserved for internal protocol use.
	// Applications should define their own message types starting from this value.
	//
	// Example:
	//   const (
	//       MyMsgLogin  = shm.MsgTypeAppStart + 0
	//       MyMsgUpdate = shm.MsgTypeAppStart + 1
	//   )
	MsgTypeAppStart MsgType = 128

	// Magic is the magic number for validating shared memory ("XLL!").
	Magic uint32 = 0x584C4C21
	// Version is the current protocol version (v0.7.0). Adds SlotHeader.Lease
	// at offset 96 (92 is alignment padding) for crash-recovery
	// (reclamation in v0.7.1).
	Version uint32 = 0x00070000

	// HostStateActive indicates the Host is spinning or processing.
	HostStateActive = 0
	// HostStateWaiting indicates the Host is sleeping on the Response Event.
	HostStateWaiting = 1
	// GuestStateActive indicates the Guest is spinning or processing.
	GuestStateActive = 0
	// GuestStateWaiting indicates the Guest is sleeping on the Request Event.
	GuestStateWaiting = 1
)

// SlotHeader represents the metadata for a single slot in shared memory.
// It must match the C++ layout exactly (128 bytes).
//
// Lease (offset 96, added in v0.7.0): the side that last CAS's State to a
// non-FREE value writes the current monotonic-ns timestamp here, marking
// the slot as actively owned. v0.7.0 only writes the lease; the
// reclamation logic that consumes it ships in v0.7.1 alongside a
// property-based crash-injection test. Until then external readers may
// poll Lease for liveness detection but no automatic action is taken.
type SlotHeader struct {
	_          [64]byte
	State      uint32
	HostState  uint32
	GuestState uint32
	MsgSeq     uint32
	MsgType    MsgType
	ReqSize    int32
	RespSize   int32
	_          uint32   // 4-byte alignment pad for Lease (mirrors C++ auto-pad)
	Lease      uint64   // atomic monotonic-ns; offset 96
	_          [24]byte // Reserved (shrunk from 36 to make room for Lease + pad)
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

// Compile-time size assertions: any layout drift versus the C++ ABI causes a
// build failure here. The trick uses a zero-length array indexed by the
// difference between the expected size and the actual size; a non-zero diff
// yields a negative array length (build error) or non-zero length (which we
// reject via the matching dual expression).
var (
	_ [128 - unsafe.Sizeof(SlotHeader{})]byte
	_ [unsafe.Sizeof(SlotHeader{}) - 128]byte
	_ [64 - unsafe.Sizeof(ExchangeHeader{})]byte
	_ [unsafe.Sizeof(ExchangeHeader{}) - 64]byte
)

// slotContext holds local runtime state for a slot.
type slotContext struct {
	header       *SlotHeader
	reqBuffer    []byte
	respBuffer   []byte
	reqEvent     EventHandle
	respEvent    EventHandle
	waitStrategy *WaitStrategy
	nextMsgSeq   uint32
	ActiveWait   int32 // Atomic flag: 1 if actively waiting, 0 otherwise
}

// DirectGuest implements the Guest side of the Direct Mode IPC.
// It manages multiple workers, each attached to a specific slot.
type DirectGuest struct {
	name            string
	shmBase         uintptr
	shmSize         uint64
	handle          ShmHandle
	numSlots        uint32
	numGuestSlots   uint32
	slotSize        uint32
	reqOffset       uint32
	respOffset      uint32
	responseTimeout time.Duration

	slots   []slotContext
	wg      sync.WaitGroup
	closing int32

	nextGuestSlot uint32 // Atomic counter for Round-Robin slot selection

	// v0.7.2: auto-reclaim threshold. When sendGuestCallInternal's slow
	// path fails to find a free guest slot it walks every guest slot
	// and tries TryReclaimAbandonedSlot with this threshold. Zero
	// (default) disables auto-reclaim — opt-in for safety.
	autoReclaimTimeoutNs uint64
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
	if numGuestSlots > 100000 {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("invalid numGuestSlots: %d", numGuestSlots)
	}
	if slotSize == 0 || slotSize > 1024*1024*1024 {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("invalid slotSize: %d", slotSize)
	}
	// Slot headers live at addr + 64 + i*(128 + slotSize): with the two
	// fixed sizes being multiples of 8, slotSize%8 != 0 would misalign every
	// SlotHeader after the first, making the uint32/uint64 atomics on State
	// and Lease operate on unaligned addresses. reqOffset feeds buffer bases
	// that may host atomics too (ring-buffer bootstrap). Reject both early.
	if slotSize%8 != 0 {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("invalid slotSize: %d (must be 8-byte aligned)", slotSize)
	}
	if reqOffset%8 != 0 {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("invalid reqOffset: %d (must be 8-byte aligned)", reqOffset)
	}

	// Validate relative offset ordering. The C++ host writes these fields to
	// shared memory; a corrupted or buggy host that produces inverted offsets
	// would underflow the uint32 subtractions for maxReq/maxResp below and
	// produce an enormous slice via unsafe.Slice that escapes the mmap'd region.
	// Reject early with a clear error instead.
	if reqOffset >= slotSize {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("shm: invalid ExchangeHeader: reqOffset (%d) >= slotSize (%d)", reqOffset, slotSize)
	}
	if respOffset <= reqOffset {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("shm: invalid ExchangeHeader: respOffset (%d) <= reqOffset (%d)", respOffset, reqOffset)
	}
	if respOffset >= slotSize {
		CloseShm(h, addr, HeaderMapSize)
		return nil, fmt.Errorf("shm: invalid ExchangeHeader: respOffset (%d) >= slotSize (%d)", respOffset, slotSize)
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
		name:            name,
		shmBase:         addr,
		shmSize:         totalSize,
		handle:          h,
		numSlots:        numSlots,
		numGuestSlots:   numGuestSlots,
		slotSize:        slotSize,
		reqOffset:       reqOffset,
		respOffset:      respOffset,
		responseTimeout: 10 * time.Second,
		slots:           make([]slotContext, totalSlots),
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

		g.slots[i].waitStrategy = NewWaitStrategy()

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

// SetAutoReclaimTimeout enables AcquireSlot-equivalent auto-reclaim during
// guest-slot acquisition. When SendGuestCall can't find a free guest slot
// in its normal scan, it walks each guest slot and tries
// TryReclaimAbandonedSlot with this threshold before giving up.
//
// Zero (default) disables auto-reclaim — opt-in for safety. Typical
// values: 5× the response timeout. Safe to call at any time.
//
// Thread-safe: atomic uint64 store under the hood.
func (g *DirectGuest) SetAutoReclaimTimeout(d time.Duration) {
	atomic.StoreUint64(&g.autoReclaimTimeoutNs, uint64(d.Nanoseconds()))
}

// GetAutoReclaimTimeout returns the current threshold (0 = disabled).
func (g *DirectGuest) GetAutoReclaimTimeout() time.Duration {
	return time.Duration(atomic.LoadUint64(&g.autoReclaimTimeoutNs))
}

// TryReclaimAbandonedSlot attempts to reclaim a slot whose Lease is older
// than maxLeaseAge.
//
// For crash recovery: if the slot's current owner crashed, its lease will
// not refresh and the slot will sit in a non-FREE state forever. This
// method reads State and Lease, and if the lease is stale by the
// threshold, attempts to CAS State back to SlotFree.
//
// Safety: the CAS ensures we never reclaim a slot a live peer just
// advanced. If a live peer heartbeats between the lease read and the
// CAS, the CAS succeeds (state still matches what we observed) and the
// slot becomes Free; the peer's next state write will fail or be a CAS
// from a stale state. The dangerous case — reclaiming while the peer is
// mid-write — is probabilistically protected by the lease window: the
// peer either heartbeated recently (lease fresh, no reclaim) or it
// didn't (lease stale, reclaim is correct).
//
// v0.7.1 ships this as an opt-in API only. Auto-reclamation inside
// WaitStrategy and an end-to-end crash-process test will follow.
//
// Returns true if the slot was reclaimed. Returns false if: the slot is
// already Free, the lease is fresher than the threshold, the lease is
// zero (peer never heartbeated — likely a v0.6.x peer, refuse to
// reclaim), or the CAS lost to a concurrent legitimate state change.
func (g *DirectGuest) TryReclaimAbandonedSlot(slotIdx int, maxLeaseAge time.Duration) bool {
	if slotIdx < 0 || slotIdx >= len(g.slots) {
		return false
	}
	s := &g.slots[slotIdx]

	state := atomic.LoadUint32(&s.header.State)
	if state == SlotFree {
		return false
	}
	lease := atomic.LoadUint64(&s.header.Lease)
	if lease == 0 {
		return false
	}
	now := MonotonicNanos()
	if now <= lease || (now-lease) <= uint64(maxLeaseAge.Nanoseconds()) {
		return false
	}
	return atomic.CompareAndSwapUint32(&s.header.State, state, SlotFree)
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
				atomic.StoreUint64(&s.header.Lease, MonotonicNanos())
				slot = s
				break
			}
		}

		if currentState == SlotRespReady {
			if atomic.LoadInt32(&s.ActiveWait) == 0 {
				if atomic.CompareAndSwapUint32(&s.header.State, SlotRespReady, SlotGuestBusy) {
					atomic.StoreUint64(&s.header.Lease, MonotonicNanos())
					slot = s
					break
				}
			}
		}
	}

	if slot == nil {
		// v0.7.2: auto-reclaim. If the caller opted in via
		// SetAutoReclaimTimeout, scan every guest slot and try
		// reclaiming any whose lease is stale. Retry the acquisition
		// once after the sweep — at most one retry, so a persistent
		// shortage still returns the original error.
		reclaimThresh := time.Duration(atomic.LoadUint64(&g.autoReclaimTimeoutNs))
		if reclaimThresh > 0 {
			reclaimed := false
			for j := 0; j < numGuest; j++ {
				idx := startBase + j
				if g.TryReclaimAbandonedSlot(idx, reclaimThresh) {
					reclaimed = true
				}
			}
			if reclaimed {
				for j := 0; j < numGuest; j++ {
					idx := startBase + j
					s := &g.slots[idx]
					if atomic.CompareAndSwapUint32(&s.header.State, SlotFree, SlotGuestBusy) {
						atomic.StoreUint64(&s.header.Lease, MonotonicNanos())
						slot = s
						break
					}
				}
			}
		}
		if slot == nil {
			return nil, fmt.Errorf("all guest slots busy")
		}
	}

	if len(data) > len(slot.reqBuffer) {
		atomic.CompareAndSwapUint32(&slot.header.State, SlotGuestBusy, SlotFree)
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

	// Mark this goroutine as an active waiter BEFORE publishing the request:
	// from the moment State can become SlotRespReady, the slot must never
	// exhibit the zombie signature (SlotRespReady && ActiveWait==0) while
	// still owned, or the Case-2 reclaim above could steal it.
	atomic.StoreInt32(&slot.ActiveWait, 1)

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

	ready := slot.waitStrategy.WaitState(&slot.header.State, SlotRespReady, sleepAction)
	claimed := false
	if ready {
		// Consume-claim: take the slot back to SlotGuestBusy BEFORE
		// clearing ActiveWait, so it never looks like a zombie
		// (SlotRespReady && ActiveWait==0) while we read the response.
		// Refresh the lease per SPECIFICATION.md §3.6.
		claimed = atomic.CompareAndSwapUint32(&slot.header.State, SlotRespReady, SlotGuestBusy)
		if claimed {
			atomic.StoreUint64(&slot.header.Lease, MonotonicNanos())
		}
	}
	atomic.StoreInt32(&slot.ActiveWait, 0)

	if !ready {
		// Timeout: the host may still own the slot (SlotReqReady/SlotBusy).
		// Do NOT store SlotFree — recovery is handled by the Case-2 zombie
		// reclaim (once the host posts its late response) or lease reclaim.
		Debug("SendGuestCall timed out waiting for host")
		return nil, fmt.Errorf("timeout waiting for host")
	}
	if !claimed {
		// A reclaimer (lease-based crash recovery) took the slot between
		// observing SlotRespReady and the consume-claim CAS. The response
		// buffer can no longer be trusted.
		return nil, fmt.Errorf("slot reclaimed while consuming response")
	}

	if slot.header.MsgSeq != currentSeq {
		atomic.CompareAndSwapUint32(&slot.header.State, SlotGuestBusy, SlotFree)
		return nil, fmt.Errorf("msgSeq mismatch: expected %d, got %d", currentSeq, slot.header.MsgSeq)
	}

	if slot.header.MsgType == MsgTypeSystemError {
		atomic.CompareAndSwapUint32(&slot.header.State, SlotGuestBusy, SlotFree)
		return nil, fmt.Errorf("system error: host rejected request (likely buffer overflow)")
	}

	respSize := slot.header.RespSize
	var respData []byte

	if respSize >= 0 {
		if int(respSize) > len(slot.respBuffer) {
			atomic.CompareAndSwapUint32(&slot.header.State, SlotGuestBusy, SlotFree)
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
			atomic.CompareAndSwapUint32(&slot.header.State, SlotGuestBusy, SlotFree)
			return nil, fmt.Errorf("invalid response size: %d", respSize)
		}
		if int(rLen) > len(slot.respBuffer) {
			atomic.CompareAndSwapUint32(&slot.header.State, SlotGuestBusy, SlotFree)
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

	atomic.CompareAndSwapUint32(&slot.header.State, SlotGuestBusy, SlotFree)

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
					// Note: a fault on actually-unmapped memory is a fatal
					// runtime error that recover() cannot catch; this guard
					// covers ordinary handler panics and shutdown races.
					if atomic.LoadInt32(&g.closing) == 1 {
						return
					}

					Error("Worker panic recovered (restarting)", "worker", idx, "panic", r)

					// Unblock Host if we were processing
					// If the panic happened while we owned the slot, we must release it.
					slot := &g.slots[idx]
					header := slot.header

					// Check if state implies we are holding the slot
					state := atomic.LoadUint32(&header.State)
					if state == SlotReqReady || state == SlotGuestBusy {
						// We failed during processing. Host is likely waiting.
						// Mark the response as a SYSTEM_ERROR: RespSize=0 with
						// the request's MsgType intact would read as a
						// successful empty response on the host side.
						header.MsgType = MsgTypeSystemError
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

		sleepAction := func() {
			atomic.StoreUint32(&header.GuestState, GuestStateWaiting)

			if atomic.LoadUint32(&header.State) == SlotReqReady {
				atomic.StoreUint32(&header.GuestState, GuestStateActive)
				return
			}

			WaitForEvent(slot.reqEvent, 100)

			if atomic.LoadInt32(&g.closing) == 1 {
				return
			}

			atomic.StoreUint32(&header.GuestState, GuestStateActive)
		}

		ready := slot.waitStrategy.WaitState(&header.State, SlotReqReady, sleepAction)

		if ready {
			if !atomic.CompareAndSwapUint32(&header.State, SlotReqReady, SlotGuestBusy) {
				continue
			}
			atomic.StoreUint64(&header.Lease, MonotonicNanos())

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
}
