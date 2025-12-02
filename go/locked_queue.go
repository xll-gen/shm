package shm

import (
	"sync"
)

// LockedSPSCQueue wraps SPSCQueue with a Mutex for Enqueue
// to support multiple producers (concurrent Send calls).
type LockedSPSCQueue struct {
	*SPSCQueue
	mu sync.Mutex
}

func NewLockedSPSCQueue(shmBase uintptr, capacity uint64, event EventHandle) *LockedSPSCQueue {
	return &LockedSPSCQueue{
		SPSCQueue: NewSPSCQueue(shmBase, capacity, event),
	}
}

func (q *LockedSPSCQueue) Enqueue(data []byte, msgId uint32) {
	q.mu.Lock()
	q.SPSCQueue.Enqueue(data, msgId)
	q.mu.Unlock()
}

func (q *LockedSPSCQueue) EnqueueBatch(msgs [][]byte) {
	q.mu.Lock()
	q.SPSCQueue.EnqueueBatch(msgs)
	q.mu.Unlock()
}
