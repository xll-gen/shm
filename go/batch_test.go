package shm

import (
	"bytes"
	"testing"
	"unsafe"
)

func TestBatch(t *testing.T) {
	// 1MB
	capacity := uint64(1024 * 1024)
	shmSize := QueueHeaderSize + capacity
	buffer := make([]byte, shmSize)
	base := uintptr(unsafe.Pointer(&buffer[0]))

    // Create real event
    hEvent, err := CreateEvent("test_go_batch")
    if err != nil {
        t.Fatalf("Failed to create event: %v", err)
    }
    defer CloseEvent(hEvent)

	q := NewSPSCQueue(base, capacity, hEvent)
	q.Header.Capacity = capacity

	inputs := make([][]byte, 10)
	for i := range inputs {
		inputs[i] = make([]byte, 100)
		inputs[i][0] = byte(i)
	}

	q.EnqueueBatch(inputs)

	outputs := q.DequeueBatch(100)

	if len(outputs) != 10 {
		t.Fatalf("Expected 10 items, got %d", len(outputs))
	}

	for i, msgPtr := range outputs {
		msg := *msgPtr
		if len(msg) != 100 {
			t.Errorf("Item %d len mismatch", i)
		}
		if msg[0] != byte(i) {
			t.Errorf("Item %d content mismatch", i)
		}
	}

	// Recycle
	q.Recycle(outputs)
}

func TestWrapping(t *testing.T) {
	capacity := uint64(200)
	shmSize := QueueHeaderSize + capacity
	buffer := make([]byte, shmSize)
	base := uintptr(unsafe.Pointer(&buffer[0]))

    // Create real event
    hEvent, err := CreateEvent("test_go_wrapping")
    if err != nil {
        t.Fatalf("Failed to create event: %v", err)
    }
    defer CloseEvent(hEvent)

	q := NewSPSCQueue(base, capacity, hEvent)
	q.Header.Capacity = capacity

	// 1. Write 50 bytes (Total 50 + 16 = 66)
	d1 := make([]byte, 50)
	d1[0] = 1
	q.Enqueue(d1, 0)

	// 2. Read 50
	out, _ := q.Dequeue()
	if len(out) != 50 {
		t.Fatal("Read mismatch")
	}

	// wPos=66, rPos=66. Cap=200.

	// 3. Write 100 bytes (Total 100 + 16 = 116) -> wPos=182
	d2 := make([]byte, 100)
	d2[0] = 2
	q.Enqueue(d2, 0)

	// Space at end: 200-182 = 18.
	// 4. Batch write 40 bytes (Total 40 + 16 = 56). Fits? No, 56 > 18.
	// Should Pad 18 (Header=16, Size=2), wrap, write 56 at 0.

	batch := [][]byte{make([]byte, 40)}
	batch[0][0] = 3
	q.EnqueueBatch(batch)

	// Read d2
	out2, _ := q.Dequeue()
	if !bytes.Equal(out2, d2) {
		t.Fatal("d2 mismatch")
	}

	// Read batch
	batchOut := q.DequeueBatch(10)
	if len(batchOut) != 1 {
		t.Fatal("Batch empty")
	}
	if !bytes.Equal(*batchOut[0], batch[0]) {
		t.Fatal("Batch content mismatch")
	}
}

func TestMsgId(t *testing.T) {
	capacity := uint64(1024)
	shmSize := QueueHeaderSize + capacity
	buffer := make([]byte, shmSize)
	base := uintptr(unsafe.Pointer(&buffer[0]))

    // Create real event
    hEvent, err := CreateEvent("test_go_msgid")
    if err != nil {
        t.Fatalf("Failed to create event: %v", err)
    }
    defer CloseEvent(hEvent)

	q := NewSPSCQueue(base, capacity, hEvent)
	q.Header.Capacity = capacity

	// 1. Write Normal
	d1 := make([]byte, 10)
	d1[0] = 1
	q.Enqueue(d1, MsgIdNormal)

	// 2. Write Heartbeat
	q.Enqueue(nil, MsgIdHeartbeatReq)

	// 3. Write Data with MsgId 2
	d2 := make([]byte, 10)
	d2[0] = 2
	q.Enqueue(d2, 2)

	// Read 1
	out, msgId := q.Dequeue()
	if msgId != MsgIdNormal {
		t.Errorf("Expected MsgIdNormal, got %d", msgId)
	}
	if len(out) != 10 || out[0] != 1 {
		t.Errorf("Data 1 mismatch")
	}

	// Read 2
	out, msgId = q.Dequeue()
	if msgId != MsgIdHeartbeatReq {
		t.Errorf("Expected MsgIdHeartbeatReq, got %d", msgId)
	}
	if len(out) != 0 {
		t.Errorf("Expected empty payload for heartbeat")
	}

	// Read 3
	out, msgId = q.Dequeue()
	if msgId != 2 {
		t.Errorf("Expected MsgId 2, got %d", msgId)
	}
	if len(out) != 10 || out[0] != 2 {
		t.Errorf("Data 2 mismatch")
	}
}
