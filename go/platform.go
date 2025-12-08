package shm

// EventHandle represents an OS-specific handle for a synchronization event.
// On Linux, it wraps a pointer to `sem_t`.
// On Windows, it wraps a `HANDLE`.
type EventHandle uintptr

// ShmHandle represents an OS-specific handle for a shared memory region.
// On Linux, it wraps a file descriptor (int).
// On Windows, it wraps a `HANDLE`.
type ShmHandle uintptr

// CreateEvent creates a new named synchronization event.
//
// name: The name of the event.
//
// Returns the event handle or an error if creation fails.
func CreateEvent(name string) (EventHandle, error) {
	return createEvent(name)
}

// OpenEvent opens an existing named synchronization event.
//
// name: The name of the event.
//
// Returns the event handle or an error if the event does not exist or cannot be opened.
func OpenEvent(name string) (EventHandle, error) {
	return openEvent(name)
}

// WaitForEvent blocks the current thread until the event is signaled or the timeout expires.
//
// h: The event handle.
// timeoutMs: Timeout in milliseconds.
func WaitForEvent(h EventHandle, timeoutMs uint32) {
	waitForEvent(h, timeoutMs)
}

// SignalEvent signals the event, waking up any waiting threads.
//
// h: The event handle.
func SignalEvent(h EventHandle) {
	signalEvent(h)
}

// CloseEvent closes the event handle and releases associated resources.
//
// h: The event handle.
func CloseEvent(h EventHandle) {
	closeEvent(h)
}

// UnlinkEvent removes the named event from the system.
// This is primarily relevant for POSIX semaphores which persist until unlinked.
//
// name: The name of the event.
func UnlinkEvent(name string) {
	unlinkEvent(name)
}

// CreateShm creates a new named shared memory region of the specified size.
//
// name: The name of the shared memory region.
// size: The size of the region in bytes.
//
// Returns the handle, the mapped address (uintptr), and any error.
func CreateShm(name string, size uint64) (ShmHandle, uintptr, error) {
	return createShm(name, size)
}

// OpenShm opens an existing named shared memory region.
// It maps the region into the process's address space.
//
// name: The name of the shared memory region.
// size: The size of the region to map.
//
// Returns the handle, the mapped address (uintptr), and any error.
func OpenShm(name string, size uint64) (ShmHandle, uintptr, error) {
	return openShm(name, size)
}

// CloseShm unmaps the shared memory and closes the handle.
//
// h: The shared memory handle.
// addr: The mapped address.
// size: The size of the mapping.
func CloseShm(h ShmHandle, addr uintptr, size uint64) {
	closeShm(h, addr, size)
}

// UnlinkShm removes the named shared memory region from the system.
//
// name: The name of the shared memory region.
func UnlinkShm(name string) {
	unlinkShm(name)
}
