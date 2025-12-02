package shm

import (
	"errors"
	"sync"
	"sync/atomic"
)

const (
	MsgIdNormal        = 0
	MsgIdHeartbeatReq  = 1
	MsgIdHeartbeatResp = 2
	MsgIdShutdown      = 3
)

// ResponseBuffer wraps a pooled response buffer and provides methods to append/send.
type ResponseBuffer struct {
	guest *IPCGuest
	buf   *[]byte
}

// Append appends data to the underlying buffer.
func (rb *ResponseBuffer) Append(data []byte) {
	*rb.buf = append(*rb.buf, data...)
}

// Send sends the accumulated data.
func (rb *ResponseBuffer) Send() error {
	err := rb.guest.Send(rb.buf)
	rb.buf = nil // Mark as consumed
	return err
}

// Context wraps a request buffer and a response buffer provider.
// It is designed to be pooled.
type Context struct {
	guest *IPCGuest
	req   *[]byte          // The pooled request buffer
	resp  *ResponseBuffer  // The pooled response buffer wrapper
}

// Request returns the request data.
func (c *Context) Request() []byte {
	if c.req == nil {
		return nil
	}
	return *c.req
}

// Response returns the ResponseBuffer.
func (c *Context) Response() *ResponseBuffer {
	return c.resp
}

// IPCGuest manages the Go-side Guest connection.
type IPCGuest struct {
	ReqQueue  *SPSCQueue // Read from here (Host->Guest)
	RespQueue *SPSCQueue // Write to here (Guest->Host)

	contextPool *sync.Pool
	bufferPool  *sync.Pool // Pool for response buffers

	running int32
	wg      sync.WaitGroup

	// Mutex for writing.
	mu sync.Mutex
}

func NewIPCGuest(reqQ *SPSCQueue, respQ *SPSCQueue) *IPCGuest {
	c := &IPCGuest{
		ReqQueue:  reqQ,
		RespQueue: respQ,
		running:   1,
	}
	c.bufferPool = &sync.Pool{
		New: func() any {
			b := make([]byte, 0, 1024)
			return &b
		},
	}
	c.contextPool = &sync.Pool{
		New: func() any {
			return &Context{
				guest: c,
				req:   nil,
				resp: &ResponseBuffer{
					guest: c,
					buf:   nil,
				},
			}
		},
	}
	return c
}

func (c *IPCGuest) Start() {
	// No background threads needed for writing
}

func (c *IPCGuest) Stop() {
	atomic.StoreInt32(&c.running, 0)
	c.wg.Wait()
}

// StartReader starts the read loop with a fiber-like handler.
func (c *IPCGuest) StartReader(handler func(*Context) error) {
	c.wg.Add(1)
	go func() {
		defer c.wg.Done()
		for atomic.LoadInt32(&c.running) == 1 {
			// Dequeue returns a pooled buffer pointer
			dataPtr, msgId := c.ReqQueue.Dequeue()

			if msgId == MsgIdHeartbeatReq {
				c.ReqQueue.RecycleBuffer(dataPtr)
				c.SendControl(MsgIdHeartbeatResp)
				continue
			}

			if msgId == MsgIdShutdown {
				c.ReqQueue.RecycleBuffer(dataPtr)
				atomic.StoreInt32(&c.running, 0)
				return
			}

			if msgId == MsgIdNormal {
				// Get Context from pool
				ctx := c.contextPool.Get().(*Context)
				ctx.req = dataPtr

				// Prepare Response Buffer
				respBufPtr := c.bufferPool.Get().(*[]byte)
				*respBufPtr = (*respBufPtr)[:0] // Reset
				ctx.resp.buf = respBufPtr

				// Execute Handler
				err := handler(ctx)
				if err != nil {
					// Handle error?
				}

				// Lifecycle management:
				// 1. Recycle Request Buffer
				c.ReqQueue.RecycleBuffer(ctx.req)
				ctx.req = nil

				// 2. Recycle Response Buffer if not sent
				// Send() sets buf to nil. If not nil, user didn't send.
				if ctx.resp.buf != nil {
					*respBufPtr = (*respBufPtr)[:0]
					c.bufferPool.Put(respBufPtr)
					ctx.resp.buf = nil
				}

				c.contextPool.Put(ctx)
			}
		}
	}()
}

func (c *IPCGuest) SendControl(msgId uint32) {
	c.mu.Lock()
	c.RespQueue.Enqueue(nil, msgId)
	c.mu.Unlock()
}

// Send sends a pooled message buffer and recycles it.
func (c *IPCGuest) Send(msg *[]byte) error {
	if atomic.LoadInt32(&c.running) == 0 {
		return errors.New("server stopped")
	}

	if atomic.LoadUint32(&c.RespQueue.Header.ConsumerActive) == 0 {
		SignalEvent(c.RespQueue.Event)
	}

	c.mu.Lock()
	c.RespQueue.Enqueue(*msg, MsgIdNormal)
	c.mu.Unlock()

	// Recycle buffer
	*msg = (*msg)[:0]
	c.bufferPool.Put(msg)
	return nil
}

// SendBytes sends a raw byte slice (copies it).
func (c *IPCGuest) SendBytes(data []byte) error {
	if atomic.LoadInt32(&c.running) == 0 {
		return errors.New("server stopped")
	}

	if atomic.LoadUint32(&c.RespQueue.Header.ConsumerActive) == 0 {
		SignalEvent(c.RespQueue.Event)
	}

	c.mu.Lock()
	c.RespQueue.Enqueue(data, MsgIdNormal)
	c.mu.Unlock()
	return nil
}

func (c *IPCGuest) Wait() {
	c.wg.Wait()
}
