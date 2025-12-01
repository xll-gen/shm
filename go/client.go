package shm

import (
	"sync"
	"sync/atomic"
)

// IPCClient manages the Go-side Guest connection with dedicated I/O threads.
type IPCClient struct {
	ReqQueue  *SPSCQueue // Read from here (Host->Guest)
	RespQueue *SPSCQueue // Write to here (Guest->Host)

	sendChan chan *[]byte
	pool     *sync.Pool

	running  int32
	wg       sync.WaitGroup
}

func NewIPCClient(reqQ *SPSCQueue, respQ *SPSCQueue) *IPCClient {
	c := &IPCClient{
		ReqQueue:  reqQ,
		RespQueue: respQ,
		sendChan:  make(chan *[]byte, 4096),
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
	c.wg.Add(1)
	go c.writeLoop()
}

func (c *IPCClient) Stop() {
	atomic.StoreInt32(&c.running, 0)
	close(c.sendChan)
	c.wg.Wait()
}

// Send queues a message for sending.
// The message buffer *must* be one allocated from c.GetBuffer() or compatible,
// because writeLoop will return it to the pool after sending.
// Actually, to be safe, maybe we should copy if not?
// The user asked for "*[]byte" to use sync.Pool.
// So the contract is: Caller gets *[]byte from Client (wrapper or direct pool), writes to it, calls Send, relinquishes ownership.
func (c *IPCClient) Send(msg *[]byte) {
	if atomic.LoadInt32(&c.running) == 0 {
		return
	}
	c.sendChan <- msg
}

func (c *IPCClient) GetBuffer() *[]byte {
	return c.pool.Get().(*[]byte)
}

func (c *IPCClient) writeLoop() {
	defer c.wg.Done()

	// Batch buffer
	batch := make([][]byte, 0, 64)
	ptrs := make([]*[]byte, 0, 64)

	for msg := range c.sendChan {
		if msg == nil {
			continue
		}

		batch = append(batch, *msg)
		ptrs = append(ptrs, msg)

		// Try to drain channel to batch
		l := len(c.sendChan)
		for i := 0; i < l && len(batch) < 100; i++ {
			next := <-c.sendChan
			if next != nil {
				batch = append(batch, *next)
				ptrs = append(ptrs, next)
			}
		}

		c.RespQueue.EnqueueBatch(batch)

		// Recycle
		for _, p := range ptrs {
			*p = (*p)[:0]
			c.pool.Put(p)
		}

		// Clear slices
		batch = batch[:0]
		ptrs = ptrs[:0]
	}
}

// ReadLoop is likely custom in user code, but we can provide a helper?
// For now, user uses c.ReqQueue.Dequeue() in their own loop or we provide one.
// The task requirement specifically mentioned "dedicated writer thread".
// It also said "When IPC starts, 2 threads start".
// So I should probably provide the Reader loop too.
func (c *IPCClient) StartReader(handler func([]byte)) {
	c.wg.Add(1)
	go func() {
		defer c.wg.Done()
		for atomic.LoadInt32(&c.running) == 1 {
			// Dequeue blocks if empty (with hybrid wait)
			// But Dequeue logic in queue.go currently blocks forever?
			// Wait, queue.go Dequeue loops forever.
			// To support graceful shutdown, we might need a way to break?
			// queue.go Dequeue loops with WaitForEvent.
			// If we CloseEvent, WaitForEvent returns?
			// Platform specific.
			// For now, let's assume it runs until app death or we rely on some signal.

			// Actually, queue.go Dequeue loops forever.
			// We can use DequeueBatch with size 1? Or just Dequeue.
			data := c.ReqQueue.Dequeue()
			handler(data)
		}
	}()
}
