package shm

import (
	"syscall"
	"unsafe"
)

var (
	kernel32                = syscall.NewLazyDLL("kernel32.dll")
	procCreateEventW        = kernel32.NewProc("CreateEventW")
	procOpenEventW          = kernel32.NewProc("OpenEventW")
	procSetEvent            = kernel32.NewProc("SetEvent")
	procWaitForSingleObject = kernel32.NewProc("WaitForSingleObject")
	procCloseHandle         = kernel32.NewProc("CloseHandle")
	procCreateFileMappingW  = kernel32.NewProc("CreateFileMappingW")
	procOpenFileMappingW    = kernel32.NewProc("OpenFileMappingW")
	procMapViewOfFile       = kernel32.NewProc("MapViewOfFile")
	procUnmapViewOfFile     = kernel32.NewProc("UnmapViewOfFile")
)

const (
	FILE_MAP_ALL_ACCESS = 0xF001F
	EVENT_ALL_ACCESS    = 0x1F0003
)

// createEvent implementation for Windows.
func createEvent(name string) (EventHandle, error) {
	n, err := syscall.UTF16PtrFromString("Local\\" + name)
	if err != nil {
		return 0, err
	}
	r1, _, err := procCreateEventW.Call(0, 0, 0, uintptr(unsafe.Pointer(n)))
	// CreateEvent returns NULL on failure
	if r1 == 0 {
		return 0, err
	}
	return EventHandle(r1), nil
}

// openEvent implementation for Windows.
func openEvent(name string) (EventHandle, error) {
	n, err := syscall.UTF16PtrFromString("Local\\" + name)
	if err != nil {
		return 0, err
	}
	// OpenEventW(dwDesiredAccess, bInheritHandle, lpName)
	r1, _, err := procOpenEventW.Call(uintptr(EVENT_ALL_ACCESS), 0, uintptr(unsafe.Pointer(n)))
	if r1 == 0 {
		return 0, err
	}
	return EventHandle(r1), nil
}

// signalEvent implementation for Windows.
//
// This goes through the runtime's standard stdcall path (g0 switch). A
// previous custom rawSyscall asm shim skipped that for speed, but it called
// kernel32 on the goroutine stack without the 16-byte RSP alignment the
// Microsoft x64 ABI requires (UB) and without stack headroom guarantees.
// SetEvent only fires when the peer is parked in an OS wait — already the
// slow path, where the kernel transition dwarfs the dispatch overhead.
func signalEvent(h EventHandle) {
	procSetEvent.Call(uintptr(h))
}

// waitForEvent implementation for Windows.
func waitForEvent(h EventHandle, timeoutMs uint32) {
	procWaitForSingleObject.Call(uintptr(h), uintptr(timeoutMs))
}

// closeEvent implementation for Windows.
func closeEvent(h EventHandle) {
	procCloseHandle.Call(uintptr(h))
}

// unlinkEvent implementation for Windows.
func unlinkEvent(name string) {
	// No-op on Windows (objects are ref-counted)
}

// createShm implementation for Windows.
func createShm(name string, size uint64) (ShmHandle, uintptr, error) {
	n, err := syscall.UTF16PtrFromString("Local\\" + name)
	if err != nil {
		return 0, 0, err
	}

	// CreateFileMappingW
	// High-order DWORD of size is size >> 32
	hMap, _, err := procCreateFileMappingW.Call(
		uintptr(syscall.InvalidHandle),
		0,
		uintptr(syscall.PAGE_READWRITE),
		uintptr(size>>32),
		uintptr(size&0xFFFFFFFF),
		uintptr(unsafe.Pointer(n)),
	)
	if hMap == 0 {
		return 0, 0, err
	}

	// MapViewOfFile
	addr, _, err := procMapViewOfFile.Call(
		hMap,
		uintptr(FILE_MAP_ALL_ACCESS),
		0,
		0,
		0,
	)
	if addr == 0 {
		procCloseHandle.Call(hMap)
		return 0, 0, err
	}

	return ShmHandle(hMap), addr, nil
}

// openShm implementation for Windows.
func openShm(name string, size uint64) (ShmHandle, uintptr, error) {
	n, err := syscall.UTF16PtrFromString("Local\\" + name)
	if err != nil {
		return 0, 0, err
	}

	// OpenFileMappingW(dwDesiredAccess, bInheritHandle, lpName)
	hMap, _, err := procOpenFileMappingW.Call(
		uintptr(FILE_MAP_ALL_ACCESS),
		0,
		uintptr(unsafe.Pointer(n)),
	)
	if hMap == 0 {
		return 0, 0, err
	}

	// MapViewOfFile
	addr, _, err := procMapViewOfFile.Call(
		hMap,
		uintptr(FILE_MAP_ALL_ACCESS),
		0,
		0,
		0,
	)
	if addr == 0 {
		procCloseHandle.Call(hMap)
		return 0, 0, err
	}

	return ShmHandle(hMap), addr, nil
}

// closeShm implementation for Windows.
func closeShm(h ShmHandle, addr uintptr, size uint64) {
	if addr != 0 {
		procUnmapViewOfFile.Call(addr)
	}
	if h != 0 {
		procCloseHandle.Call(uintptr(h))
	}
}

// unlinkShm implementation for Windows.
func unlinkShm(name string) {
	// No-op on Windows
}
