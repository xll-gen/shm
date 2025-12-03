package shm

// IPCQueue defines the interface for a shared memory queue.
//
// Implementations (like SPSCQueue) provide the mechanisms for
// enqueueing and dequeueing data across the process boundary.
type IPCQueue interface {
	// Enqueue writes data to the queue.
	// Blocks if full.
	Enqueue(data []byte, msgId uint32)

	// Dequeue reads data from the queue.
	// Blocks if empty.
	// Returns payload and message ID.
	Dequeue() ([]byte, uint32)

	// EnqueueBatch writes multiple messages efficiently.
	EnqueueBatch(msgs [][]byte)

	// DequeueBatch reads multiple messages efficiently.
	// Returns a slice of pointers to byte slices (reused from pool).
	DequeueBatch(maxCount int) []*[]byte

	// Recycle returns buffers to the internal pool.
	Recycle(msgs []*[]byte)
}
