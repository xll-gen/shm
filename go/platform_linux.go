package shm

/*
#include <fcntl.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>

// Helper to open semaphore
sem_t* create_sem(const char* name) {
	// O_CREAT with permissions 0644, init value 0
	return sem_open(name, O_CREAT, 0644, 0);
}

sem_t* open_sem_existing(const char* name) {
    // No O_CREAT
    return sem_open(name, 0);
}

// Helper for timed wait
int wait_sem(sem_t* sem, int ms) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (ms % 1000) * 1000000;
	if (ts.tv_nsec >= 1000000000) {
		ts.tv_sec++;
		ts.tv_nsec -= 1000000000;
	}
	return sem_timedwait(sem, &ts);
}

int create_shm_fd(const char* name) {
	return shm_open(name, O_CREAT | O_RDWR, 0666);
}

int open_shm_fd(const char* name) {
    // No O_CREAT
    return shm_open(name, O_RDWR, 0666);
}

long get_file_size(int fd) {
    struct stat st;
    if (fstat(fd, &st) == -1) return -1;
    return st.st_size;
}

int unlink_sem(const char* name) {
    return sem_unlink(name);
}

int unlink_shm(const char* name) {
    return shm_unlink(name);
}
*/
import "C"
import (
	"fmt"
	"unsafe"
)

// createEvent implementation for Linux (using named semaphores).
func createEvent(name string) (EventHandle, error) {
	cName := C.CString("/" + name)
	defer C.free(unsafe.Pointer(cName))

	sem := C.create_sem(cName)
	if sem == C.SEM_FAILED {
		return 0, fmt.Errorf("sem_open(O_CREAT) failed")
	}
	return EventHandle(unsafe.Pointer(sem)), nil
}

// openEvent implementation for Linux.
func openEvent(name string) (EventHandle, error) {
    cName := C.CString("/" + name)
    defer C.free(unsafe.Pointer(cName))

    sem := C.open_sem_existing(cName)
    if sem == C.SEM_FAILED {
        return 0, fmt.Errorf("sem_open(existing) failed")
    }
    return EventHandle(unsafe.Pointer(sem)), nil
}

// signalEvent implementation for Linux.
func signalEvent(h EventHandle) {
	C.sem_post((*C.sem_t)(unsafe.Pointer(h)))
}

// waitForEvent implementation for Linux.
func waitForEvent(h EventHandle, timeoutMs uint32) {
	C.wait_sem((*C.sem_t)(unsafe.Pointer(h)), C.int(timeoutMs))
}

// closeEvent implementation for Linux.
func closeEvent(h EventHandle) {
	C.sem_close((*C.sem_t)(unsafe.Pointer(h)))
}

// unlinkEvent implementation for Linux.
func unlinkEvent(name string) {
	cName := C.CString("/" + name)
	defer C.free(unsafe.Pointer(cName))
	C.unlink_sem(cName)
}

// createShm implementation for Linux (using shm_open/mmap).
func createShm(name string, size uint64) (ShmHandle, uintptr, error) {
	cName := C.CString("/" + name)
	defer C.free(unsafe.Pointer(cName))

	fd := C.create_shm_fd(cName)
	if fd < 0 {
		return 0, 0, fmt.Errorf("shm_open failed")
	}

	if C.ftruncate(fd, C.long(size)) == -1 {
		C.close(fd)
		return 0, 0, fmt.Errorf("ftruncate failed")
	}

	addr := C.mmap(nil, C.size_t(size), C.PROT_READ|C.PROT_WRITE, C.MAP_SHARED, fd, 0)
	if addr == C.MAP_FAILED {
		C.close(fd)
		return 0, 0, fmt.Errorf("mmap failed")
	}

	return ShmHandle(uintptr(fd)), uintptr(addr), nil
}

// openShm implementation for Linux.
func openShm(name string, size uint64) (ShmHandle, uintptr, error) {
    cName := C.CString("/" + name)
    defer C.free(unsafe.Pointer(cName))

    fd := C.open_shm_fd(cName)
    if fd < 0 {
        return 0, 0, fmt.Errorf("shm_open(existing) failed")
    }

    // Check size to avoid SIGBUS if Host hasn't truncated yet
    curSize := C.get_file_size(fd)
    if curSize < C.long(size) {
        C.close(fd)
        return 0, 0, fmt.Errorf("shm file size too small (host initializing?)")
    }

    addr := C.mmap(nil, C.size_t(size), C.PROT_READ|C.PROT_WRITE, C.MAP_SHARED, fd, 0)
    if addr == C.MAP_FAILED {
        C.close(fd)
        return 0, 0, fmt.Errorf("mmap failed")
    }

    return ShmHandle(uintptr(fd)), uintptr(addr), nil
}

// closeShm implementation for Linux.
func closeShm(h ShmHandle, addr uintptr, size uint64) {
	if addr != 0 {
		C.munmap(unsafe.Pointer(addr), C.size_t(size))
	}
	C.close(C.int(h))
}

// unlinkShm implementation for Linux.
func unlinkShm(name string) {
	cName := C.CString("/" + name)
	defer C.free(unsafe.Pointer(cName))
	C.unlink_shm(cName)
}
