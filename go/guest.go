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
type IPCGuest struct {
	ReqQueue  *SPSCQueue // Read from here (Host->Guest)
	RespQueue *SPSCQueue // Write to here (Guest->Host)

	pool *sync.Pool

	running int32
	wg      sync.WaitGroup

	// Mutex for writing.
	// Previously used spinlock, but sync.Mutex proved more stable in benchmarks.
	mu sync.Mutex
}

func NewIPCGuest(reqQ *SPSCQueue, respQ *SPSCQueue) *IPCGuest {
	c := &IPCGuest{
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

func (c *IPCGuest) Start() {
	// No background threads needed for writing anymore
}

func (c *IPCGuest) Stop() {
	atomic.StoreInt32(&c.running, 0)
	c.wg.Wait()
}

// Send queues a message for sending.
// The message buffer *must* be one allocated from c.GetBuffer() or compatible.
// It writes directly to the queue.
func (c *IPCGuest) Send(msg *[]byte) {
	if atomic.LoadInt32(&c.running) == 0 {
		return
	}

	if atomic.LoadUint32(&c.RespQueue.Header.ConsumerWaiting) == 1 {
		SignalEvent(c.RespQueue.Event)
	}

	c.mu.Lock()
	c.RespQueue.Enqueue(*msg, MsgIdNormal)
	c.mu.Unlock()

	// Recycle buffer
	*msg = (*msg)[:0]
	c.pool.Put(msg)
}

func (c *IPCGuest) GetBuffer() *[]byte {
	return c.pool.Get().(*[]byte)
}

func (c *IPCGuest) StartReader(handler func([]byte)) {
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

func (c *IPCGuest) SendControl(msgId uint32) {
	c.mu.Lock()
	c.RespQueue.Enqueue(nil, msgId)
	c.mu.Unlock()
}

// SendBytes sends a raw byte slice without pooling.
func (c *IPCGuest) SendBytes(data []byte) {
	if atomic.LoadInt32(&c.running) == 0 {
		return
	}

	if atomic.LoadUint32(&c.RespQueue.Header.ConsumerWaiting) == 1 {
		SignalEvent(c.RespQueue.Event)
	}

	c.mu.Lock()
	c.RespQueue.Enqueue(data, MsgIdNormal)
	c.mu.Unlock()
}

func (c *IPCGuest) Wait() {
	c.wg.Wait()
}
