package shm

type EventHandle uintptr
type ShmHandle uintptr

func CreateEvent(name string) (EventHandle, error) {
	return createEvent(name)
}

func OpenEvent(name string) (EventHandle, error) {
	return openEvent(name)
}

func WaitForEvent(h EventHandle, timeoutMs uint32) {
	waitForEvent(h, timeoutMs)
}

func SignalEvent(h EventHandle) {
	signalEvent(h)
}

func CloseEvent(h EventHandle) {
	closeEvent(h)
}

func CreateShm(name string, size uint64) (ShmHandle, uintptr, error) {
	return createShm(name, size)
}

func OpenShm(name string, size uint64) (ShmHandle, uintptr, error) {
	return openShm(name, size)
}

func CloseShm(h ShmHandle, addr uintptr, size uint64) {
	closeShm(h, addr, size)
}
