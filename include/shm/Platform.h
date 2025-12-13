#pragma once

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <string>
    typedef HANDLE EventHandle;
    typedef HANDLE ShmHandle;
#else
    #include <fcntl.h>
    #include <sys/stat.h>
    #include <semaphore.h>
    #include <sys/mman.h>
    #include <sys/file.h>
    #include <unistd.h>
    #include <time.h>
    #include <errno.h>
    #include <string>
    #include <cstring>
    #include <iostream>
    #include <immintrin.h>
    #include "Logger.h"

    typedef sem_t* EventHandle;
    typedef int ShmHandle; // File Descriptor
#endif

namespace shm {

/**
 * @class Platform
 * @brief Abstraction layer for OS-specific synchronization and memory mapping.
 *
 * Provides a unified API for Windows and Linux to handle:
 * - Named Events (Semaphores on Linux, Events on Windows)
 * - Shared Memory (shm_open/mmap on Linux, CreateFileMapping on Windows)
 * - CPU relaxation and thread yielding
 */
class Platform {
public:
    /**
     * @brief Creates or opens a named synchronization event.
     *
     * @param name The name of the event. On Linux, a leading '/' is prepended if missing.
     * @return EventHandle The handle to the event, or valid handle/pointer on success.
     */
    static EventHandle CreateNamedEvent(const char* name) {
#ifdef _WIN32
        std::string s_name(name);
        std::wstring w_name(s_name.begin(), s_name.end());
        std::wstring final_ev_name = L"Local\\" + w_name;
        return CreateEventW(NULL, FALSE, FALSE, final_ev_name.c_str());
#else
        std::string evName = "/" + std::string(name);
        EventHandle sem = sem_open(evName.c_str(), O_CREAT, 0644, 0);
        if (sem == SEM_FAILED) {
            SHM_LOG_ERROR("sem_open failed: ", strerror(errno));
            return nullptr;
        }
        return sem;
#endif
    }

    /**
     * @brief Signals the event (sets to signaled state or posts semaphore).
     *
     * @param h The event handle.
     */
    static void SignalEvent(EventHandle h) {
        if (!h) return;
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
        if (!h) return;
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
     * @brief Closes the event handle.
     *
     * @param h The event handle.
     */
    static void CloseEvent(EventHandle h) {
        if (!h) return;
#ifdef _WIN32
        CloseHandle(h);
#else
        sem_close(h);
#endif
    }

    /**
     * @brief Unlinks the named event (removes it from the system).
     *
     * @param name The name of the event.
     */
    static void UnlinkNamedEvent(const char* name) {
#ifdef _WIN32
        // Windows objects are ref-counted and destroyed when last handle is closed.
        // No explicit unlink needed for Local\ events.
#else
        std::string evName = "/" + std::string(name);
        sem_unlink(evName.c_str());
#endif
    }

    /**
     * @brief Creates or opens a named shared memory region.
     *
     * @param name The name of the shared memory region.
     * @param size The size of the region in bytes.
     * @param[out] outHandle The native handle for the shared memory (File Mapping or FD).
     * @param[out] outExists Set to true if the region already existed.
     * @return void* Pointer to the mapped memory, or nullptr on failure.
     */
    static void* CreateNamedShm(const char* name, uint64_t size, ShmHandle& outHandle, bool& outExists) {
#ifdef _WIN32
        std::string s_name(name);
        std::wstring w_name(s_name.begin(), s_name.end());
        std::wstring final_shm_name = L"Local\\" + w_name;
        outHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            (DWORD)(size >> 32), (DWORD)size, final_shm_name.c_str());

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
     * @brief Unmaps and closes shared memory resources.
     *
     * @param h The shared memory handle.
     * @param addr The mapped address.
     * @param size The size of the mapping.
     */
    static void CloseShm(ShmHandle h, void* addr, uint64_t size) {
#ifdef _WIN32
        if (addr) UnmapViewOfFile(addr);
        if (h) CloseHandle(h);
#else
        if (addr) munmap(addr, size);
        if (h >= 0) close(h);
#endif
    }

    /**
     * @brief Unlinks the shared memory region (removes it from the system).
     *
     * @param name The name of the shared memory region.
     */
    static void UnlinkShm(const char* name) {
#ifdef _WIN32
        // Windows pagefile-backed SHM is destroyed when last handle is closed.
#else
        std::string shmName = "/" + std::string(name);
        shm_unlink(shmName.c_str());
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
     * @brief Executes a CPU pause instruction (REP NOP / PAUSE).
     * Used in spin loops to reduce power consumption and pipeline flushing.
     */
    static void CpuRelax() {
#ifdef _WIN32
        YieldProcessor();
#else
        _mm_pause();
#endif
    }

    /**
     * @brief Returns the current process ID.
     * @return int PID.
     */
    static int GetPid() {
#ifdef _WIN32
        return (int)GetCurrentProcessId();
#else
        return (int)getpid();
#endif
    }

    /**
     * @brief Tries to acquire an exclusive lock on the SHM region.
     *
     * @param hShm The shared memory handle (Linux FD).
     * @param name The shared memory name (Windows Mutex Name).
     * @param[out] outLockHandle The lock handle (Windows Mutex). Unused on Linux.
     * @return true if acquired, false if already locked.
     */
    static bool LockShm(ShmHandle hShm, const char* name, ShmHandle& outLockHandle) {
#ifdef _WIN32
        std::string s_name(name);
        std::wstring w_name(s_name.begin(), s_name.end());
        std::wstring mutex_name = L"Local\\" + w_name + L"_lock";

        HANDLE hMutex = CreateMutexW(NULL, TRUE, mutex_name.c_str());
        if (!hMutex) return false;

        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            // Mutex existed. Did we get ownership?
            // "The bInitialOwner parameter is ignored if the mutex already exists."
            // So we must try to wait for it.
            DWORD res = WaitForSingleObject(hMutex, 0);
            if (res == WAIT_OBJECT_0) {
                outLockHandle = hMutex;
                return true;
            } else {
                CloseHandle(hMutex);
                return false;
            }
        }

        // We created it and own it.
        outLockHandle = hMutex;
        return true;
#else
        // Linux: Lock the file descriptor
        if (flock(hShm, LOCK_EX | LOCK_NB) == 0) {
            return true;
        }
        return false;
#endif
    }

    /**
     * @brief Releases the SHM lock.
     * @param hShm The shared memory handle.
     * @param hLock The lock handle (Windows).
     */
    static void UnlockShm(ShmHandle hShm, ShmHandle hLock) {
#ifdef _WIN32
        if (hLock) {
            ReleaseMutex(hLock);
            CloseHandle(hLock);
        }
#else
        flock(hShm, LOCK_UN);
#endif
    }
};

}
