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

// Context wraps a request buffer and provides methods to send a response.
// It is designed to be pooled.
type Context struct {
	guest *IPCGuest
	req   *[]byte // The pooled request buffer
}

// Body returns the request data.
func (c *Context) Body() []byte {
	if c.req == nil {
		return nil
	}
	return *c.req
}

// Send sends the response data.
func (c *Context) Send(data []byte) error {
	return c.guest.SendBytes(data)
}

// SendString sends a string as response.
func (c *Context) SendString(s string) error {
	return c.guest.SendBytes([]byte(s))
}

// IPCGuest manages the Go-side Guest connection.
type IPCGuest struct {
	ReqQueue  *SPSCQueue // Read from here (Host->Guest)
	RespQueue *SPSCQueue // Write to here (Guest->Host)

	contextPool *sync.Pool

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
	c.contextPool = &sync.Pool{
		New: func() any {
			return &Context{
				guest: c,
				req:   nil,
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
				// Recycle request buffer immediately as we don't need it
				c.ReqQueue.RecycleBuffer(dataPtr)
				// Respond immediately
				c.SendControl(MsgIdHeartbeatResp)
				continue
			}

			if msgId == MsgIdShutdown {
				c.ReqQueue.RecycleBuffer(dataPtr)
				// Stop
				atomic.StoreInt32(&c.running, 0)
				return
			}

			if msgId == MsgIdNormal {
				// Get Context from pool
				ctx := c.contextPool.Get().(*Context)
				ctx.req = dataPtr

				// Execute Handler
				err := handler(ctx)
				if err != nil {
					// Handle error? For now, we assume user handled response or didn't care.
				}

				// Recycle Context and Request Buffer
				c.ReqQueue.RecycleBuffer(ctx.req)
				ctx.req = nil
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

// SendBytes sends a raw byte slice.
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
