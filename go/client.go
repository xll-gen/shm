package shm

import (
	"fmt"
	"time"
)

type Mode int

const (
	ModeQueue Mode = iota
	ModeDirect
)

// Client is the high-level API for Guest.
type Client struct {
	transport Transport
	handler   func([]byte) []byte
}

type Transport interface {
	Start(func([]byte) []byte)
	Close()
	Wait()
}

// Connect creates a connection to the Host.
// It automatically retries until connection is established.
func Connect(name string, mode Mode) (*Client, error) {
	var t Transport
	var err error

	// Retry loop
	for i := 0; i < 50; i++ {
		if mode == ModeDirect {
			// Assuming defaults for now.
			t, err = NewDirectGuest(name, 4, 1024*1024)
		} else {
			// Queue Mode
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
                        _ = hMap
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

func (c *Client) Handle(h func([]byte) []byte) {
	c.handler = h
}

func (c *Client) Start() {
	if c.handler == nil {
		panic("Handler not set")
	}

	c.transport.Start(func(data []byte) []byte {
		// 1. Unpack Header
		if len(data) < TransportHeaderSize {
			return nil
		}
		reqID := UnpackTransportHeader(data)
		payload := data[TransportHeaderSize:]

		// 2. Call Handler
		respPayload := c.handler(payload)

		// 3. Pack Response
		respTotal := TransportHeaderSize + len(respPayload)
		// We should recycle this buffer if possible, but for now allocate.
		// IPCGuest.pool could be used if exposed.
		respBuf := make([]byte, respTotal)
		PackTransportHeader(reqID, respBuf)
		copy(respBuf[TransportHeaderSize:], respPayload)

		return respBuf
	})
}

func (c *Client) Wait() {
	c.transport.Wait()
}

func (c *Client) Close() {
	c.transport.Close()
}
