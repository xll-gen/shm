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

	// 1. Write 50 bytes (Total 58)
	d1 := make([]byte, 50)
	d1[0] = 1
	q.Enqueue(d1)

	// 2. Read 50
	out := q.Dequeue()
	if len(out) != 50 {
		t.Fatal("Read mismatch")
	}

	// wPos=58, rPos=58. Cap=200.

	// 3. Write 100 bytes (Total 108) -> wPos=166
	d2 := make([]byte, 100)
	d2[0] = 2
	q.Enqueue(d2)

	// Space at end: 200-166 = 34.
	// 4. Batch write 40 bytes (Total 48). Fits? No, 48 > 34.
	// Should Pad 34 (Header=8, Size=26), wrap, write 48 at 0.

	batch := [][]byte{make([]byte, 40)}
	batch[0][0] = 3
	q.EnqueueBatch(batch)

	// Read d2
	out2 := q.Dequeue()
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
