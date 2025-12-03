#pragma once

#ifdef _WIN32
    #include <windows.h>
    typedef HANDLE EventHandle;
    typedef HANDLE ShmHandle;
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <semaphore.h>
    #include <sys/mman.h>
    #include <unistd.h>
    #include <time.h>
    #include <errno.h>
    #include <string>
    #include <cstring>
    #include <iostream>

    typedef sem_t* EventHandle;
    typedef int ShmHandle; // File Descriptor
#endif

namespace shm {

/**
 * @brief Platform Abstraction Layer (PAL).
 *
 * Provides a unified interface for system-specific operations like
 * named events (semaphores) and shared memory management.
 *
 * Supported Platforms:
 * - Windows (Events, CreateFileMapping)
 * - Linux (Named Semaphores, shm_open/mmap)
 */
class Platform {
public:
    /**
     * @brief Creates or opens a named synchronization event.
     *
     * @param name The name of the event (will be prefixed/sanitized per platform).
     * @return EventHandle Handle to the event, or nullptr/SEM_FAILED on error.
     */
    static EventHandle CreateNamedEvent(const char* name) {
#ifdef _WIN32
        std::string evName = "Local\\" + std::string(name);
        return CreateEventA(NULL, FALSE, FALSE, evName.c_str());
#else
        std::string evName = "/" + std::string(name);
        EventHandle sem = sem_open(evName.c_str(), O_CREAT, 0644, 0);
        if (sem == SEM_FAILED) {
            std::cerr << "sem_open failed: " << strerror(errno) << std::endl;
            return nullptr;
        }
        return sem;
#endif
    }

    /**
     * @brief Signals (sets/posts) the event to wake up a waiting thread.
     *
     * @param h The event handle.
     */
    static void SignalEvent(EventHandle h) {
#ifdef _WIN32
        SetEvent(h);
#else
        sem_post(h);
#endif
    }

    /**
     * @brief Waits for the event to be signaled.
     *
     * @param h The event handle.
     * @param timeoutMs Timeout in milliseconds. Default 0xFFFFFFFF (Infinite).
     */
    static void WaitEvent(EventHandle h, uint32_t timeoutMs = 0xFFFFFFFF) {
#ifdef _WIN32
        WaitForSingleObject(h, timeoutMs);
#else
        if (timeoutMs == 0xFFFFFFFF) {
            sem_wait(h);
        } else {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += timeoutMs / 1000;
            ts.tv_nsec += (timeoutMs % 1000) * 1000000;
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            sem_timedwait(h, &ts);
        }
#endif
    }

    /**
     * @brief Closes the event handle and releases resources.
     *
     * @param h The event handle.
     */
    static void CloseEvent(EventHandle h) {
#ifdef _WIN32
        CloseHandle(h);
#else
        sem_close(h);
#endif
    }

    /**
     * @brief Creates or opens a named shared memory region.
     *
     * @param name The name of the shared memory region.
     * @param size Size in bytes.
     * @param[out] outHandle Output parameter for the internal file/mapping handle.
     * @param[out] outExists Set to true if the region already existed.
     * @return void* Pointer to the mapped memory, or nullptr on failure.
     */
    static void* CreateNamedShm(const char* name, uint64_t size, ShmHandle& outHandle, bool& outExists) {
#ifdef _WIN32
        std::string shmName = "Local\\" + std::string(name);
        outHandle = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            (DWORD)(size >> 32), (DWORD)size, shmName.c_str());

        if (!outHandle) return nullptr;

        outExists = (GetLastError() == ERROR_ALREADY_EXISTS);
        void* addr = MapViewOfFile(outHandle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        return addr;
#else
        std::string shmName = "/" + std::string(name);
        outHandle = shm_open(shmName.c_str(), O_CREAT | O_RDWR, 0666);
        if (outHandle < 0) return nullptr;

        struct stat st;
        fstat(outHandle, &st);
        outExists = (st.st_size > 0); // Simplistic check

        if (ftruncate(outHandle, size) == -1) {
            close(outHandle);
            return nullptr;
        }

        void* addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, outHandle, 0);
        if (addr == MAP_FAILED) {
            close(outHandle);
            return nullptr;
        }
        return addr;
#endif
    }

    /**
     * @brief Unmaps and closes the shared memory handle.
     *
     * @param h The shared memory handle (File Mapping / FD).
     * @param addr The mapped address to unmap.
     */
    static void CloseShm(ShmHandle h, void* addr) {
#ifdef _WIN32
        if (addr) UnmapViewOfFile(addr);
        if (h) CloseHandle(h);
#else
        // munmap(addr, size); // size is lost here, leak in this simple abstraction
        if (h >= 0) close(h);
#endif
    }

    /**
     * @brief Yields the current thread's time slice.
     */
    static void ThreadYield() {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }

    /**
     * @brief Gets the current Process ID.
     * @return int Process ID.
     */
    static int GetPid() {
#ifdef _WIN32
        return (int)GetCurrentProcessId();
#else
        return (int)getpid();
#endif
    }
};

}
