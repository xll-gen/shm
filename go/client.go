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
// It wraps a Transport (Queue or Direct) and handles connection retries.
type Client struct {
	transport Transport
	handler   func([]byte) []byte
}

// Transport interface defines the contract for IPC mechanisms.
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
			// Direct Mode
            // We need to guess or know the params.
            // In benchmark, we use -w for threads. Host uses same count.
            // But NewDirectGuest needs numSlots.
            // Client.Connect signature doesn't take numSlots.
            // We'll assume a default or we need to change Connect signature?
            // For now, let's assume the user of Connect (Benchmark) might need to use NewDirectGuest directly if they want custom slots.
            // BUT, the existing code had defaults "4, 1MB".
            // Let's stick to that for now, or better:
            // The Host creates the SHM. Guest just opens it.
            // Actually, Guest needs to know numSlots to Open events (slot_0, slot_1).
            // This is a limitation of the current Connect API.
            // We will use 4 slots as a fallback if not specified,
            // but the benchmark passes -w.
            // The previous client.go had "4". Let's use 64 to be safe?
            // Or just 16.
			t, err = NewDirectGuest(name, 16, 1024*1024)
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
    // Pass the handler directly. No extra headers.
	c.transport.Start(c.handler)
}

func (c *Client) Wait() {
	c.transport.Wait()
}

func (c *Client) Close() {
	c.transport.Close()
}
