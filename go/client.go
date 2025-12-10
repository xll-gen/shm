package shm

import (
	"fmt"
	"time"
)

// ClientConfig holds configuration parameters for the Client connection.
type ClientConfig struct {
	// ShmName is the name of the shared memory region (e.g., "MyIPC").
	ShmName string
	// ConnectionTimeout is the maximum duration to wait for the Host to initialize shared memory.
	// Default: 10 seconds.
	ConnectionTimeout time.Duration
	// RetryInterval is the interval between connection attempts.
	// Default: 100 milliseconds.
	RetryInterval time.Duration
}

// Client is the high-level API for the Guest side of the IPC.
// It wraps DirectGuest and handles connection retries and lifecycle management.
type Client struct {
	guest   *DirectGuest
	handler func([]byte, []byte, MsgType) (int32, MsgType)
}

// Connect attempts to establish a connection to the Host using the provided configuration.
// It assumes Direct Mode and retries until the timeout expires.
//
// config: The configuration object.
//
// Returns a Client instance or an error if connection fails after retries.
func Connect(config ClientConfig) (*Client, error) {
	var g *DirectGuest
	var err error

	timeout := config.ConnectionTimeout
	if timeout == 0 {
		timeout = 10 * time.Second
	}

	interval := config.RetryInterval
	if interval == 0 {
		interval = 100 * time.Millisecond
	}

	start := time.Now()

	for {
		// Direct Mode: Auto-discover configuration from SHM Header.
		g, err = NewDirectGuest(config.ShmName)

		if err == nil {
			return &Client{guest: g}, nil
		}

		if time.Since(start) >= timeout {
			break
		}
		time.Sleep(interval)
	}
	return nil, fmt.Errorf("failed to connect to %s after %v: %v", config.ShmName, timeout, err)
}

// ConnectDefault is a helper for backward compatibility or simple usage.
// It connects with default settings (10s timeout).
func ConnectDefault(name string) (*Client, error) {
    return Connect(ClientConfig{
        ShmName: name,
    })
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
// It panics if the Handler is not set.
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
//
// d: The timeout duration.
func (c *Client) SetTimeout(d time.Duration) {
	c.guest.SetTimeout(d)
}

// SendGuestCall sends a message to the Host (Guest Call).
// It uses the default timeout configured via SetTimeout (default 10s).
//
// data: The payload to send.
// msgType: The message type.
//
// Returns the response payload or an error.
func (c *Client) SendGuestCall(data []byte, msgType MsgType) ([]byte, error) {
	return c.guest.SendGuestCall(data, msgType)
}

// SendGuestCallWithTimeout sends a message to the Host (Guest Call) with a custom timeout.
//
// data: The payload to send.
// msgType: The message type.
// timeout: The custom timeout duration.
//
// Returns the response payload or an error.
func (c *Client) SendGuestCallWithTimeout(data []byte, msgType MsgType, timeout time.Duration) ([]byte, error) {
	return c.guest.SendGuestCallWithTimeout(data, msgType, timeout)
}

// AcquireGuestSlot acquires a free guest slot for Zero-Copy operations.
//
// This allows the caller to write directly to the shared memory buffer
// and read the response directly, avoiding extra allocations.
//
// Returns a GuestSlot object or an error if no slots are available.
// The caller must call Release() on the slot when finished.
func (c *Client) AcquireGuestSlot() (*GuestSlot, error) {
	return c.guest.AcquireGuestSlot()
}

// Close releases all resources associated with the client.
// It closes shared memory handles and event handles.
// Note: This does not stop background workers if they are blocked on OS events.
func (c *Client) Close() {
	c.guest.Close()
}
