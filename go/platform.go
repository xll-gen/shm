package shm

// EventHandle represents an OS-specific handle for a synchronization event.
type EventHandle uintptr

// ShmHandle represents an OS-specific handle for a shared memory region.
type ShmHandle uintptr

// CreateEvent creates a new named event.
// Returns an error if the event creation fails.
func CreateEvent(name string) (EventHandle, error) {
	return createEvent(name)
}

// OpenEvent opens an existing named event.
// Returns an error if the event does not exist or cannot be opened.
func OpenEvent(name string) (EventHandle, error) {
	return openEvent(name)
}

// WaitForEvent blocks the current thread until the event is signaled or the timeout expires.
// timeoutMs: Timeout in milliseconds.
func WaitForEvent(h EventHandle, timeoutMs uint32) {
	waitForEvent(h, timeoutMs)
}

// SignalEvent signals the event, waking up any waiting threads.
func SignalEvent(h EventHandle) {
	signalEvent(h)
}

// CloseEvent closes the event handle.
func CloseEvent(h EventHandle) {
	closeEvent(h)
}

// UnlinkEvent removes the named event from the system.
func UnlinkEvent(name string) {
	unlinkEvent(name)
}

// CreateShm creates a new named shared memory region of the specified size.
// Returns the handle, the mapped address, and any error.
func CreateShm(name string, size uint64) (ShmHandle, uintptr, error) {
	return createShm(name, size)
}

// OpenShm opens an existing named shared memory region.
// It maps the region into the process's address space.
// Returns the handle, the mapped address, and any error.
func OpenShm(name string, size uint64) (ShmHandle, uintptr, error) {
	return openShm(name, size)
}

// CloseShm unmaps the shared memory and closes the handle.
func CloseShm(h ShmHandle, addr uintptr, size uint64) {
	closeShm(h, addr, size)
}

// UnlinkShm removes the named shared memory region from the system.
func UnlinkShm(name string) {
	unlinkShm(name)
}
