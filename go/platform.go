package shm

// EventHandle represents a handle to a system synchronization object.
// (Semaphore on Linux, Event on Windows).
type EventHandle uintptr

// ShmHandle represents a handle to a shared memory object.
// (File Descriptor on Linux, File Mapping Handle on Windows).
type ShmHandle uintptr

// CreateEvent creates a new named synchronization event.
//
// Parameters:
//   - name: Unique name for the event.
//
// Returns:
//   - EventHandle: The handle.
//   - error: System error if creation fails.
func CreateEvent(name string) (EventHandle, error) {
	return createEvent(name)
}

// OpenEvent opens an existing named synchronization event.
func OpenEvent(name string) (EventHandle, error) {
	return openEvent(name)
}

// WaitForEvent blocks until the event is signaled or timeout occurs.
//
// Parameters:
//   - h: The event handle.
//   - timeoutMs: Timeout in milliseconds.
func WaitForEvent(h EventHandle, timeoutMs uint32) {
	waitForEvent(h, timeoutMs)
}

// SignalEvent wakes up a waiting thread/process.
func SignalEvent(h EventHandle) {
	signalEvent(h)
}

// CloseEvent releases the event handle.
func CloseEvent(h EventHandle) {
	closeEvent(h)
}

// CreateShm creates a new named shared memory region.
//
// Parameters:
//   - name: Unique name for the region.
//   - size: Size in bytes.
//
// Returns:
//   - ShmHandle: Handle to the SHM object.
//   - uintptr: Mapped address in the current process.
//   - error: System error if creation fails.
func CreateShm(name string, size uint64) (ShmHandle, uintptr, error) {
	return createShm(name, size)
}

// OpenShm opens an existing named shared memory region.
//
// It maps the region into the process address space.
func OpenShm(name string, size uint64) (ShmHandle, uintptr, error) {
	return openShm(name, size)
}

// CloseShm unmaps and closes the shared memory region.
func CloseShm(h ShmHandle, addr uintptr) {
	closeShm(h, addr)
}
