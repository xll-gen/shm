package shm

import (
	"sync"
	"sync/atomic"
)

// Message ID constants.
const (
	MsgIdNormal        = 0 // Normal user payload.
	MsgIdHeartbeatReq  = 1 // Heartbeat request from Host.
	MsgIdHeartbeatResp = 2 // Heartbeat response from Guest.
	MsgIdShutdown      = 3 // Shutdown signal.
)

// IPCGuest manages the Go-side Guest connection for Queue Mode.
//
// It uses two SPSC queues:
// - ReqQueue: Reads requests from Host.
// - RespQueue: Writes responses to Host.
//
// It is generic over the Queue implementation (IPCQueue).
type IPCGuest[Q IPCQueue] struct {
	ReqQueue  Q // Read from here (Host->Guest)
	RespQueue Q // Write to here (Guest->Host)

	pool *sync.Pool

	running int32
	wg      sync.WaitGroup
}

// NewIPCGuest creates a new Guest for Queue Mode.
//
// Parameters:
//   - reqQ: Queue for receiving requests.
//   - respQ: Queue for sending responses.
func NewIPCGuest[Q IPCQueue](reqQ Q, respQ Q) *IPCGuest[Q] {
	c := &IPCGuest[Q]{
		ReqQueue:  reqQ,
		RespQueue: respQ,
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

// Start begins the request processing loop in a background goroutine.
//
// It reads from ReqQueue, invokes the handler, and writes the result to RespQueue.
// Handles Control Messages (Heartbeat, Shutdown) automatically.
//
// Parameters:
//   - handler: Function to process user payloads.
func (c *IPCGuest[Q]) Start(handler func([]byte) []byte) {
	c.wg.Add(1)
	go func() {
		defer c.wg.Done()
		for atomic.LoadInt32(&c.running) == 1 {
			data, msgId := c.ReqQueue.Dequeue()

			if msgId == MsgIdHeartbeatReq {
				// Respond immediately
				c.SendControl(MsgIdHeartbeatResp)
				continue
			}

			if msgId == MsgIdShutdown {
				// Stop
				atomic.StoreInt32(&c.running, 0)
				return
			}

			if msgId == MsgIdNormal {
				resp := handler(data)
                if resp != nil {
				    c.RespQueue.Enqueue(resp, MsgIdNormal)
                }
			}
		}
	}()
}

// Close signals the loop to exit.
func (c *IPCGuest[Q]) Close() {
	atomic.StoreInt32(&c.running, 0)
}

// Wait blocks until the processing loop exits.
func (c *IPCGuest[Q]) Wait() {
	c.wg.Wait()
}

// SendControl sends a control message (no payload) to the Host.
func (c *IPCGuest[Q]) SendControl(msgId uint32) {
	c.RespQueue.Enqueue(nil, msgId)
}
