package main

import (
	"flag"
	"fmt"
	"log"
	"time"

	"github.com/xll-gen/shm/go"
)

var (
	shmName = flag.String("name", "StreamIntegration", "SHM Name")
)

func main() {
	flag.Parse()

	// 1. Connect
	client, err := shm.Connect(shm.ClientConfig{
		ShmName:           *shmName,
		ConnectionTimeout: 10 * time.Second,
		RetryInterval:     100 * time.Millisecond,
	})
	if err != nil {
		log.Fatalf("Go: Connect failed: %v", err)
	}
	defer client.Close()

	// Channels to sync test steps
	streamReceived := make(chan []byte, 1)

	// 2. Setup Reassembler (Host -> Guest)
	handler := shm.NewStreamReassembler(func(streamID uint64, data []byte) {
		fmt.Printf("Go: Received stream ID %d, size %d\n", streamID, len(data))
		streamReceived <- data
	}, func(req []byte, resp []byte, msgType shm.MsgType) (int32, shm.MsgType) {
		return 0, shm.MsgTypeNormal
	})

	client.Handle(handler)
	client.Start()

	fmt.Println("Go: Ready")

	// 3. Wait for Stream from Host
	select {
	case data := <-streamReceived:
		// Verify content (expecting 0xAA pattern)
		// Host sends 1MB of 0xAA
		if len(data) != 1024*1024 {
			log.Fatalf("Go: Expected 1MB, got %d", len(data))
		}
		for i, b := range data {
			if b != 0xAA {
				log.Fatalf("Go: Byte mismatch at %d: expected 0xAA, got 0x%X", i, b)
			}
		}
		fmt.Println("Go: Host->Guest Stream Verified")
	case <-time.After(10 * time.Second):
		log.Fatal("Go: Timeout waiting for stream")
	}

	// 4. Send Stream to Host (Guest -> Host)
	// Create 512KB of 0xBB
	payloadSize := 512 * 1024
	payload := make([]byte, payloadSize)
	for i := range payload {
		payload[i] = 0xBB
	}

	fmt.Println("Go: Sending stream...")
	sender := shm.NewStreamSenderFromClient(client)
	if err := sender.Send(payload, 999); err != nil {
		log.Fatalf("Go: Send failed: %v", err)
	}
    fmt.Println("Go: Stream Sent")

	// Sleep to allow Host to process
	time.Sleep(1 * time.Second)
}
