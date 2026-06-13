#pragma once

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <string>

typedef HANDLE EventHandle;
typedef HANDLE ShmHandle;

namespace shm {

/**
 * @class Platform
 * @brief Abstraction layer for Windows OS synchronization and memory mapping.
 *
 * Provides the API used by the rest of the library for:
 * - Named Events (Win32 Event objects)
 * - Shared Memory (CreateFileMapping / MapViewOfFile)
 * - CPU relaxation and thread yielding
 *
 * This library is Windows-only (MSVC 2019+ / MinGW). All primitives map
 * directly onto the Win32 API.
 */
class Platform {
public:
    /**
     * @brief Creates or opens a named synchronization event.
     *
     * @param name The name of the event.
     * @return EventHandle The handle to the event, or NULL on failure.
     */
    static EventHandle CreateNamedEvent(const char* name) {
        std::string s_name(name);
        std::wstring w_name(s_name.begin(), s_name.end());
        std::wstring final_ev_name = L"Local\\" + w_name;
        return CreateEventW(NULL, FALSE, FALSE, final_ev_name.c_str());
    }

    /**
     * @brief Signals the event (sets to signaled state).
     *
     * @param h The event handle.
     */
    static void SignalEvent(EventHandle h) {
        if (!h) return;
        SetEvent(h);
    }

    /**
     * @brief Waits for the event to be signaled.
     *
     * @param h The event handle.
     * @param timeoutMs Timeout in milliseconds. Default 0xFFFFFFFF (Infinite).
     * @return true if signaled, false if timeout or error.
     */
    static bool WaitEvent(EventHandle h, uint32_t timeoutMs = 0xFFFFFFFF) {
        if (!h) return false;
        DWORD res = WaitForSingleObject(h, timeoutMs);
        return (res == WAIT_OBJECT_0);
    }

    /**
     * @brief Closes the event handle.
     *
     * @param h The event handle.
     */
    static void CloseEvent(EventHandle h) {
        if (!h) return;
        CloseHandle(h);
    }

    /**
     * @brief Opens an existing named event.
     *
     * @param name The name of the event.
     * @return EventHandle The handle.
     */
    static EventHandle OpenEvent(const char* name) {
        std::string s_name(name);
        std::wstring w_name(s_name.begin(), s_name.end());
        std::wstring final_ev_name = L"Local\\" + w_name;
        return OpenEventW(EVENT_ALL_ACCESS, FALSE, final_ev_name.c_str());
    }

    /**
     * @brief Unlinks the named event (removes it from the system).
     *
     * @param name The name of the event.
     *
     * @note On Windows, `Local\` Event objects are kernel-object reference
     *       counted and destroyed when the last handle is closed, so this is
     *       a no-op. It is retained for API symmetry; callers (DirectHost
     *       teardown, tests) may invoke it harmlessly.
     */
    static void UnlinkNamedEvent(const char* name) {
        // Windows objects are ref-counted and destroyed when last handle is closed.
        // No explicit unlink needed for Local\ events.
        (void)name;
    }

    /**
     * @brief Creates or opens a named shared memory region.
     *
     * @param name The name of the shared memory region.
     * @param size The size of the region in bytes.
     * @param[out] outHandle The native file-mapping handle.
     * @param[out] outExists Set to true if the region already existed.
     * @return void* Pointer to the mapped memory, or nullptr on failure.
     */
    static void* CreateNamedShm(const char* name, uint64_t size, ShmHandle& outHandle, bool& outExists) {
        std::string s_name(name);
        std::wstring w_name(s_name.begin(), s_name.end());
        std::wstring final_shm_name = L"Local\\" + w_name;
        outHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
            (DWORD)(size >> 32), (DWORD)size, final_shm_name.c_str());

        if (!outHandle) return nullptr;

        outExists = (GetLastError() == ERROR_ALREADY_EXISTS);
        void* addr = MapViewOfFile(outHandle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        return addr;
    }

    /**
     * @brief Unmaps and closes shared memory resources.
     *
     * @param h The shared memory handle.
     * @param addr The mapped address.
     * @param size The size of the mapping.
     */
    static void CloseShm(ShmHandle h, void* addr, uint64_t size) {
        (void)size;
        if (addr) UnmapViewOfFile(addr);
        if (h) CloseHandle(h);
    }

    /**
     * @brief Opens an existing named shared memory region.
     *
     * @param name The name of the shared memory region.
     * @param size The expected size (for mapping).
     * @param[out] outRealSize The actual size of the region.
     * @param[out] outAddr The mapped address.
     * @return ShmHandle The handle.
     */
    static ShmHandle OpenShm(const char* name, uint64_t size, uint64_t& outRealSize, void*& outAddr) {
        (void)size;
        outAddr = nullptr;
        outRealSize = 0;
        std::string s_name(name);
        std::wstring w_name(s_name.begin(), s_name.end());
        std::wstring final_shm_name = L"Local\\" + w_name;
        ShmHandle h = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, final_shm_name.c_str());
        if (!h) return 0;

        outAddr = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (outAddr) {
             MEMORY_BASIC_INFORMATION info;
             VirtualQuery(outAddr, &info, sizeof(info));
             outRealSize = info.RegionSize;
        }
        return h;
    }

    /**
     * @brief Unlinks the shared memory region (removes it from the system).
     *
     * @param name The name of the shared memory region.
     *
     * @note Windows pagefile-backed shared memory is destroyed when the last
     *       handle is closed, so this is a no-op. Retained for API symmetry.
     */
    static void UnlinkShm(const char* name) {
        // Windows pagefile-backed SHM is destroyed when last handle is closed.
        (void)name;
    }

    /**
     * @brief Yields the current thread's time slice.
     */
    static void ThreadYield() {
        SwitchToThread();
    }

    /**
     * @brief Executes a CPU pause instruction (REP NOP / PAUSE).
     * Used in spin loops to reduce power consumption and pipeline flushing.
     */
    static void CpuRelax() {
        YieldProcessor();
    }

    /**
     * @brief Returns wall-clock nanoseconds since Unix epoch.
     *
     * Used by the v0.7.0 `SlotHeader::lease` field — owners stamp their
     * activity time. We use wall-clock (not QPC) specifically so the value
     * is comparable across processes AND across languages: the Go side
     * stores `time.Now().UnixNano()` and the C++ side stores this. Both sit
     * in the same Unix-epoch timeline.
     *
     * NOT strictly monotonic — an NTP step can move the clock backward.
     * For lease purposes this is acceptable: a backward step causes a
     * spurious reclamation candidate (caught by the v0.7.1 CAS guard,
     * which re-checks state before reclaiming); a forward step delays
     * reclamation. Neither corrupts data, both are rare.
     *
     * Despite the name "MonotonicNanos" we are NOT using a monotonic
     * clock; kept this way so v0.7.1's `TryReclaimAbandonedSlot` API
     * has a stable identifier.
     */
    static uint64_t MonotonicNanos() {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        // FILETIME is 100-ns ticks since 1601-01-01; Unix epoch is
        // 1970-01-01, so subtract the 11644473600-second offset.
        ULARGE_INTEGER ull;
        ull.LowPart = ft.dwLowDateTime;
        ull.HighPart = ft.dwHighDateTime;
        const uint64_t kEpochDeltaTicks = 116444736000000000ULL;
        return (ull.QuadPart - kEpochDeltaTicks) * 100ULL;
    }

    /**
     * @brief Returns the current process ID.
     * @return int PID.
     */
    static int GetPid() {
        return (int)GetCurrentProcessId();
    }

    /**
     * @brief Tries to acquire an exclusive lock on the SHM region.
     *
     * @param hShm The shared memory handle. Unused on Windows.
     * @param name The shared memory name (used to derive the Windows Mutex name).
     * @param[out] outLockHandle The lock handle (Windows Mutex).
     * @return true if acquired, false if already locked.
     */
    static bool LockShm(ShmHandle hShm, const char* name, ShmHandle& outLockHandle) {
        (void)hShm;
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
    }

    /**
     * @brief Releases the SHM lock.
     * @param hShm The shared memory handle. Unused on Windows.
     * @param hLock The lock handle (Windows Mutex).
     */
    static void UnlockShm(ShmHandle hShm, ShmHandle hLock) {
        (void)hShm;
        if (hLock) {
            ReleaseMutex(hLock);
            CloseHandle(hLock);
        }
    }
};

}
