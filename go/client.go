package shm

import (
	"fmt"
	"time"
)

// Client is the high-level API for Guest.
type Client struct {
	guest   *DirectGuest
	handler func([]byte, []byte) int
}

// Connect creates a connection to the Host (Direct Mode only).
// It automatically retries until connection is established.
func Connect(name string) (*Client, error) {
	var g *DirectGuest
	var err error

	// Retry loop
	for i := 0; i < 50; i++ {
		g, err = NewDirectGuest(name)

		if err == nil {
			return &Client{guest: g}, nil
		}
		time.Sleep(100 * time.Millisecond)
	}
	return nil, fmt.Errorf("failed to connect after retries: %v", err)
}

func (c *Client) Handle(h func([]byte, []byte) int) {
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
