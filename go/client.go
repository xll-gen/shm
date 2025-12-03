package shm

import (
	"fmt"
	"time"
)

// Mode represents the IPC operating mode.
type Mode int

const (
	// ModeQueue uses SPSC Ring Buffers for streaming data.
	ModeQueue Mode = iota
	// ModeDirect uses fixed slots for low-latency request-response.
	ModeDirect
)

// Client is the high-level API for the Guest (Go).
//
// It wraps the underlying transport (Queue or Direct) and handles
// connection initialization and retries.
//
// Usage:
//
//	client, _ := shm.Connect("MyIPC", shm.ModeDirect)
//	client.Handle(func(req []byte) []byte {
//	    return process(req)
//	})
//	client.Start()
//	client.Wait()
type Client struct {
	transport Transport
	handler   func([]byte) []byte
}

// Transport interface defines the contract for IPC mechanisms.
//
// It abstracts the differences between Queue and Direct modes.
type Transport interface {
	// Start begins the worker loop, processing requests using the provided handler.
	Start(func([]byte) []byte)

	// Close cleans up resources (Shared Memory, Events).
	Close()

	// Wait blocks until the transport is shut down (e.g., via Shutdown message).
	Wait()
}

// Connect establishes a connection to the Host.
//
// It automatically retries for up to 5 seconds if the Host has not yet
// created the shared memory region.
//
// Parameters:
//   - name: The unique name of the IPC channel (must match Host).
//   - mode: The operating mode (Queue or Direct).
//
// Returns:
//   - *Client: A new Client instance.
//   - error: Error if connection fails after retries.
func Connect(name string, mode Mode) (*Client, error) {
	var t Transport
	var err error

	// Retry loop to handle race condition where Host is starting up
	for i := 0; i < 50; i++ {
		if mode == ModeDirect {
			// Direct Mode
            // Defaults: 16 slots, 1MB per slot.
            // Ideally should match Host config, but this is safe for now as long as
            // Host uses <= 16 slots.
			t, err = NewDirectGuest(name, 16, 1024*1024)
		} else {
			// Queue Mode
			// Defaults: 32MB Queue Size.
			qTotalSize := uint64(QueueHeaderSize + 32*1024*1024)
			totalSize := uint64(qTotalSize * 2)

			var hMap ShmHandle
			var addr uintptr
			hMap, addr, err = OpenShm(name, totalSize)
			if err == nil {
				hTo, err2 := OpenEvent(name + "_event_req")
				hFrom, err3 := OpenEvent(name + "_event_resp")
				if err2 == nil && err3 == nil {
					toQ := NewLockedSPSCQueue(addr, 32*1024*1024, hTo)
					fromQ := NewLockedSPSCQueue(addr+uintptr(qTotalSize), 32*1024*1024, hFrom)

					if toQ.Header.Capacity > 0 {
						t = NewIPCGuest(toQ, fromQ)
                        _ = hMap // Keep handle alive if needed by GC, though not strictly used here
					} else {
						err = fmt.Errorf("queue not initialized")
					}
				} else {
					err = fmt.Errorf("events not ready")
				}
			}
		}

		if err == nil {
			return &Client{transport: t}, nil
		}
		time.Sleep(100 * time.Millisecond)
	}
	return nil, fmt.Errorf("failed to connect after retries: %v", err)
}

// Handle registers the callback function for processing requests.
//
// The handler receives the raw request payload and must return a response payload.
//
// Parameters:
//   - h: The function to handle requests.
func (c *Client) Handle(h func([]byte) []byte) {
	c.handler = h
}

// Start begins the processing loop.
//
// It spawns background goroutines to handle incoming requests.
// Panics if no handler has been set.
func (c *Client) Start() {
	if c.handler == nil {
		panic("Handler not set")
	}
	c.transport.Start(c.handler)
}

// Wait blocks the current goroutine until the Client shuts down.
//
// Shutdown is usually triggered by the Host sending a Shutdown signal.
func (c *Client) Wait() {
	c.transport.Wait()
}

// Close releases all underlying resources.
func (c *Client) Close() {
	c.transport.Close()
}
