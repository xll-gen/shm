package shm

import (
	"runtime"
	"sync"
	"sync/atomic"
)

// IPCClient manages the Go-side Guest connection.
type IPCClient struct {
	ReqQueue  *SPSCQueue // Read from here (Host->Guest)
	RespQueue *SPSCQueue // Write to here (Guest->Host)

	pool *sync.Pool

	running int32
	wg      sync.WaitGroup

	// spinLock for writing, to avoid syscalls in sync.Mutex
	writeLock int32
}

func NewIPCClient(reqQ *SPSCQueue, respQ *SPSCQueue) *IPCClient {
	c := &IPCClient{
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

func (c *IPCClient) Start() {
	// No background threads needed for writing anymore
}

func (c *IPCClient) Stop() {
	atomic.StoreInt32(&c.running, 0)
	c.wg.Wait()
}

// Send queues a message for sending.
// The message buffer *must* be one allocated from c.GetBuffer() or compatible.
// It writes directly to the queue.
func (c *IPCClient) Send(msg *[]byte) {
	if atomic.LoadInt32(&c.running) == 0 {
		return
	}

	// Spin Lock
	for {
		if atomic.CompareAndSwapInt32(&c.writeLock, 0, 1) {
			break
		}
		// Pause/Yield
		for i := 0; i < 30; i++ {
			// CPU pause hint? Go doesn't expose it directly except internal.
		}
		runtime.Gosched()
	}

	c.RespQueue.Enqueue(*msg)

	atomic.StoreInt32(&c.writeLock, 0)

	// Recycle buffer
	*msg = (*msg)[:0]
	c.pool.Put(msg)
}

func (c *IPCClient) GetBuffer() *[]byte {
	return c.pool.Get().(*[]byte)
}

func (c *IPCClient) StartReader(handler func([]byte)) {
	c.wg.Add(1)
	go func() {
		defer c.wg.Done()
		for atomic.LoadInt32(&c.running) == 1 {
			data := c.ReqQueue.Dequeue()
			handler(data)
		}
	}()
}
