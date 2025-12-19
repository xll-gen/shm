package shm

import (
	"bytes"
	"testing"
	"unsafe"
)

func TestRingBufferReceiver(t *testing.T) {
	// 1. Setup Mock Memory
	// Header (256 bytes) + Data (1024 bytes)
	totalSize := 256 + 1024
	mem := make([]byte, totalSize)

	// Initialize Header
	header := (*RingBufferHeader)(unsafe.Pointer(&mem[0]))
	header.WriteOffset = 0
	header.ReadOffset = 0

	// Create Receiver
	rb := &RingBufferReceiver{
		slotIdx:  0,
		header:   header,
		data:     mem[256:],
		capacity: 1024,
		guest:    nil, // Not needed for this unit test
	}

	// 2. Simulate Host Writing
	inputData := make([]byte, 500)
	for i := range inputData {
		inputData[i] = byte(i)
	}

	// Host writes 500 bytes (0..499)
	// Circular write logic mock
	copy(rb.data[0:500], inputData)
	header.WriteOffset = 500

	// 3. Guest Reads
	out := make([]byte, 200)
	n := rb.Read(out)

	if n != 200 {
		t.Fatalf("Expected read 200 bytes, got %d", n)
	}
	if header.ReadOffset != 200 {
		t.Fatalf("Expected ReadOffset 200, got %d", header.ReadOffset)
	}
	if !bytes.Equal(out, inputData[:200]) {
		t.Fatalf("Data mismatch in first read")
	}

	// 4. Guest Reads remaining 300
	out2 := make([]byte, 300)
	n = rb.Read(out2)
	if n != 300 {
		t.Fatalf("Expected read 300 bytes, got %d", n)
	}
	if header.ReadOffset != 500 {
		t.Fatalf("Expected ReadOffset 500, got %d", header.ReadOffset)
	}
	if !bytes.Equal(out2, inputData[200:]) {
		t.Fatalf("Data mismatch in second read")
	}

	// 5. Host Wrap Around
	// Host writes 800 more bytes. Total w=1300.
	// Capacity 1024. w=500 -> 1300.
	// Write from 500..1024 (524 bytes), then 0..276.
	moreData := make([]byte, 800)
	for i := range moreData {
		moreData[i] = byte(i + 500) // Continue sequence
	}

	// Manual write simulation
	// Write 500 to 1024
	chunk1 := 1024 - 500 // 524
	copy(rb.data[500:], moreData[:chunk1])
	// Write 0 to ...
	// chunk2 := 800 - chunk1 // 276 (Implicitly used)
	copy(rb.data[0:], moreData[chunk1:])

	header.WriteOffset = 1300

	// 6. Guest Read Wrapped
	readBuf := make([]byte, 800)
	n = rb.Read(readBuf)
	if n != 800 {
		t.Fatalf("Expected read 800 bytes, got %d", n)
	}
	if header.ReadOffset != 1300 {
		t.Fatalf("Expected ReadOffset 1300, got %d", header.ReadOffset)
	}

	if !bytes.Equal(readBuf, moreData) {
		t.Fatalf("Data mismatch in wrapped read")
	}
}
