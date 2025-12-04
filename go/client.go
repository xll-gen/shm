package shm

import (
	"fmt"
	"time"
)

// Client is the high-level API for Guest.
type Client struct {
	guest   *DirectGuest
	handler func([]byte, []byte) uint32
}

// Connect creates a connection to the Host (Direct Mode only).
// It automatically retries until connection is established.
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

// Handle sets the request handler.
// Handler receives request buffer and response buffer (pre-allocated).
// Handler should write response to respBuf and return the number of bytes written.
func (c *Client) Handle(h func(req []byte, respBuf []byte) uint32) {
	c.handler = h
}

func (c *Client) Start() {
	if c.handler == nil {
		panic("Handler not set")
	}
	c.guest.Start(c.handler)
}

func (c *Client) Wait() {
	c.guest.Wait()
}

func (c *Client) Close() {
	c.guest.Close()
}
