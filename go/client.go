package shm

import (
	"fmt"
	"time"
)

// Client is the high-level API for the Guest side of the IPC.
// It wraps DirectGuest and handles connection retries and lifecycle management.
type Client struct {
	guest   *DirectGuest
	handler func([]byte, []byte, MsgType) (int32, MsgType)
}

// Connect attempts to establish a connection to the Host with the given shared memory name.
// It assumes Direct Mode and retries up to 50 times (5 seconds) for the Host to initialize the memory.
//
// name: The name of the shared memory region (e.g., "MyIPC").
//
// Returns a Client instance or an error if connection fails after retries.
func Connect(name string) (*Client, error) {
	var g *DirectGuest
	var err error

	// Retry loop
	for i := 0; i < 50; i++ {
		// Direct Mode: Auto-discover configuration from SHM Header.
		// Extra params ignored as DirectGuest discovers size from header.
		g, err = NewDirectGuest(name, 0, 0)

		if err == nil {
			return &Client{guest: g}, nil
		}
		time.Sleep(100 * time.Millisecond)
	}
	return nil, fmt.Errorf("failed to connect after retries: %v", err)
}

// Handle registers the request handler function.
//
// h: A function that takes a request buffer, response buffer, and message Type.
//    It should process the request, write the response to the response buffer,
//    and return the size of the response written (negative for End-Aligned) and the response MsgType.
//
// The handler must be thread-safe as it may be called concurrently by multiple workers.
func (c *Client) Handle(h func(req []byte, respBuf []byte, msgType MsgType) (int32, MsgType)) {
	c.handler = h
}

// Start initiates the worker routines.
// This method spawns goroutines and returns immediately.
// Panic if Handler is not set.
func (c *Client) Start() {
	if c.handler == nil {
		panic("Handler not set")
	}
	c.guest.Start(c.handler)
}

// Wait blocks until all worker routines have exited.
// Workers typically exit upon receiving a Shutdown message from the Host.
func (c *Client) Wait() {
	c.guest.Wait()
}

// SetTimeout sets the timeout for waiting for a response (Guest Call).
func (c *Client) SetTimeout(d time.Duration) {
	c.guest.SetTimeout(d)
}

// SendGuestCall sends a message to the Host (Guest Call).
func (c *Client) SendGuestCall(data []byte, msgType MsgType) ([]byte, error) {
	return c.guest.SendGuestCall(data, msgType)
}

// Close releases all resources associated with the client.
// It closes shared memory handles and event handles.
func (c *Client) Close() {
	c.guest.Close()
}
