package shm

import (
	"fmt"
	"sync"
	"sync/atomic"
)

const (
	MsgIdNormal        = 0
	MsgIdHeartbeatReq  = 1
	MsgIdHeartbeatResp = 2
	MsgIdShutdown      = 3
)

// IPCGuest manages the Go-side Guest connection with multiple lanes.
type IPCGuest[Q IPCQueue] struct {
	reqQueues  []Q // Inbound (Host->Guest)
	respQueues []Q // Outbound (Guest->Host)

	pool *sync.Pool

	running int32
	wg      sync.WaitGroup
}

// NewIPCGuest creates a new guest with mapped queues.
// reqQs and respQs must have the same length (numLanes).
func NewIPCGuest[Q IPCQueue](reqQs []Q, respQs []Q) *IPCGuest[Q] {
	if len(reqQs) != len(respQs) {
		panic("reqQs and respQs must have same length")
	}

	c := &IPCGuest[Q]{
		reqQueues:  reqQs,
		respQueues: respQs,
		pool: &sync.Pool{
			New: func() any {
				b := make([]byte, 0, 1024)
				return &b
			},
		},
		running: 1,
	}
	return c
}

// Stop signals all workers to stop.
func (c *IPCGuest[Q]) Stop() {
	atomic.StoreInt32(&c.running, 0)
	c.wg.Wait()
}

// StartWorkers starts a dedicated worker for each lane.
// The handler is called inline for each request.
// The handler receives the data and a responder function.
func (c *IPCGuest[Q]) StartWorkers(handler func(data []byte, laneID int)) {
	for i := 0; i < len(c.reqQueues); i++ {
		laneID := i
		c.wg.Add(1)
		go func(lID int) {
			defer c.wg.Done()
			reqQ := c.reqQueues[lID]
			// respQ is used via SendToLane

			for atomic.LoadInt32(&c.running) == 1 {
				data, msgId := reqQ.Dequeue()

				if msgId == MsgIdHeartbeatReq {
					// Respond immediately on the same lane
					c.SendControl(lID, MsgIdHeartbeatResp)
					continue
				}

				if msgId == MsgIdShutdown {
					// Stop entire guest
					// Only one lane needs to receive this to shut everything down?
					// Or wait for all?
					// Usually Host sends shutdown to all lanes.
					atomic.StoreInt32(&c.running, 0)
					return
				}

				if msgId == MsgIdNormal {
					handler(data, lID)
				}
			}
		}(laneID)
	}
}

// SendToLane sends a raw byte slice to a specific lane.
func (c *IPCGuest[Q]) SendToLane(data []byte, laneID int) {
	if atomic.LoadInt32(&c.running) == 0 {
		return
	}
	c.respQueues[laneID].Enqueue(data, MsgIdNormal)
}

// SendControl sends a control message to a specific lane.
func (c *IPCGuest[Q]) SendControl(laneID int, msgId uint32) {
	c.respQueues[laneID].Enqueue(nil, msgId)
}

func (c *IPCGuest[Q]) Wait() {
	c.wg.Wait()
}

// Helper to open N lanes of queues
func OpenLanes[Q any](
	shmName string,
	numLanes int,
	queueSize uint64,
	baseAddr uintptr,
	shmTotalSize uint64,
	createQueueFunc func(uintptr, uint64, EventHandle) Q,
) ([]Q, []Q, error) {

	reqQs := make([]Q, numLanes)
	respQs := make([]Q, numLanes)

	qRequiredSize := QueueHeaderSize + queueSize
	ptr := baseAddr

	for i := 0; i < numLanes; i++ {
		// Req Queue
		eventNameReq := fmt.Sprintf("%s_req_%d", shmName, i)
		hReq, err := OpenEvent(eventNameReq)
		if err != nil {
			return nil, nil, fmt.Errorf("failed to open event %s: %w", eventNameReq, err)
		}

		reqQs[i] = createQueueFunc(ptr, queueSize, hReq)
		ptr += uintptr(qRequiredSize)

		// Resp Queue
		eventNameResp := fmt.Sprintf("%s_resp_%d", shmName, i)
		hResp, err := OpenEvent(eventNameResp)
		if err != nil {
			return nil, nil, fmt.Errorf("failed to open event %s: %w", eventNameResp, err)
		}

		respQs[i] = createQueueFunc(ptr, queueSize, hResp)
		ptr += uintptr(qRequiredSize)
	}

	return reqQs, respQs, nil
}
