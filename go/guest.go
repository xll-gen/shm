package shm

import (
	"sync"
	"sync/atomic"
)

const (
	MsgIdNormal        = 0
	MsgIdHeartbeatReq  = 1
	MsgIdHeartbeatResp = 2
	MsgIdShutdown      = 3
)

// IPCGuest manages the Go-side Guest connection.
type IPCGuest[Q IPCQueue] struct {
	ReqQueue  Q // Read from here (Host->Guest)
	RespQueue Q // Write to here (Guest->Host)

	pool *sync.Pool

	running int32
	wg      sync.WaitGroup
}

func NewIPCGuest[Q IPCQueue](reqQ Q, respQ Q) *IPCGuest[Q] {
	c := &IPCGuest[Q]{
		ReqQueue:  reqQ,
		RespQueue: respQ,
		pool: &sync.Pool{
			New: func() any {
				b := make([]byte, 1024) // Default buffer
				return &b
			},
		},
		running: 1,
	}
	return c
}

// Implement Transport Interface

// Start listens for requests, calls handler, and sends back response.
func (c *IPCGuest[Q]) Start(handler func([]byte, []byte) int, ready chan<- struct{}) {
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
                // Compatibility Adapter for Queue Mode
                // Queue Mode is NOT Zero-Copy in the same sense as Direct.
                // We need a buffer for the response.
                respBufPtr := c.pool.Get().(*[]byte)
                respBuf := *respBufPtr

                // Call Handler
				n := handler(data, respBuf)

                if n > 0 {
                    // Enqueue expects []byte. We slice it.
                    // This Enqueue will copy it again into the queue.
				    c.RespQueue.Enqueue(respBuf[:n], MsgIdNormal)
                }
                c.pool.Put(respBufPtr)
			}
		}
	}()
	close(ready)
}

func (c *IPCGuest[Q]) Close() {
	atomic.StoreInt32(&c.running, 0)
}

func (c *IPCGuest[Q]) Wait() {
	c.wg.Wait()
}

func (c *IPCGuest[Q]) SendControl(msgId uint32) {
	c.RespQueue.Enqueue(nil, msgId)
}
