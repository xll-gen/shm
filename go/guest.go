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
				b := make([]byte, 0, 1024)
				return &b
			},
		},
		running: 1,
	}
	return c
}

func (c *IPCGuest[Q]) Start() {
	// No background threads needed for writing anymore
}

func (c *IPCGuest[Q]) Stop() {
	atomic.StoreInt32(&c.running, 0)
	c.wg.Wait()
}

// Send queues a message for sending.
func (c *IPCGuest[Q]) Send(msg *[]byte) {
	if atomic.LoadInt32(&c.running) == 0 {
		return
	}

	c.RespQueue.Enqueue(*msg, MsgIdNormal)

	// Recycle buffer
	*msg = (*msg)[:0]
	c.pool.Put(msg)
}

func (c *IPCGuest[Q]) GetBuffer() *[]byte {
	return c.pool.Get().(*[]byte)
}

// StartReader starts the read loop using the provided handler.
// Handler runs on the reader goroutine, so it should be fast or spawn its own goroutine.
func (c *IPCGuest[Q]) StartReader(handler func([]byte)) {
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
				handler(data)
			}
		}
	}()
}

func (c *IPCGuest[Q]) SendControl(msgId uint32) {
	c.RespQueue.Enqueue(nil, msgId)
}

// SendBytes sends a raw byte slice without pooling.
func (c *IPCGuest[Q]) SendBytes(data []byte) {
	if atomic.LoadInt32(&c.running) == 0 {
		return
	}
	c.RespQueue.Enqueue(data, MsgIdNormal)
}

func (c *IPCGuest[Q]) Wait() {
	c.wg.Wait()
}
