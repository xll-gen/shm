package shm

type IPCQueue interface {
	Enqueue(data []byte, msgId uint32)
	Dequeue() ([]byte, uint32)
	EnqueueBatch(msgs [][]byte)
	DequeueBatch(maxCount int) []*[]byte
	Recycle(msgs []*[]byte)
}
