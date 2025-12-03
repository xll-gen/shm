package shm

import (
	"sync"
)

// LockedSPSCQueue wraps an SPSCQueue with a Mutex.
//
// While the SPSCQueue is lock-free for a Single Producer and Single Consumer,
// the Guest side might have multiple goroutines attempting to write to the
// response queue (if using MPMC logic, though standard Guest is single-threaded per queue).
//
// This wrapper ensures thread safety for the Producer (Enqueue) side.
type LockedSPSCQueue struct {
	*SPSCQueue
	mu sync.Mutex
}

// NewLockedSPSCQueue creates a thread-safe wrapper around an SPSCQueue.
func NewLockedSPSCQueue(shmBase uintptr, capacity uint64, event EventHandle) *LockedSPSCQueue {
	return &LockedSPSCQueue{
		SPSCQueue: NewSPSCQueue(shmBase, capacity, event),
	}
}

// Enqueue safely writes a message to the queue.
func (q *LockedSPSCQueue) Enqueue(data []byte, msgId uint32) {
	q.mu.Lock()
	q.SPSCQueue.Enqueue(data, msgId)
	q.mu.Unlock()
}

// EnqueueBatch safely writes a batch of messages.
func (q *LockedSPSCQueue) EnqueueBatch(msgs [][]byte) {
	q.mu.Lock()
	q.SPSCQueue.EnqueueBatch(msgs)
	q.mu.Unlock()
}
