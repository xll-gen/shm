#pragma once

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

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
     * spurious reclamation candidate (caught by the v0.7.2 CAS guard,
     * which re-checks state before reclaiming); a forward step delays
     * reclamation. Neither corrupts data, both are rare.
     *
     * Despite the name "MonotonicNanos" we are NOT using a monotonic
     * clock; kept this way so v0.7.2's `TryReclaimAbandonedSlot` API
     * has a stable identifier.
     *
     * Resolution: `GetSystemTimeAsFileTime` reads the KUSER_SHARED_DATA
     * tick (0.5–15.6 ms granularity) instead of the QPC-backed precise
     * variant. This sits on the slot-claim hot path (one lease stamp per
     * claim, see SPECIFICATION.md §3.6), where the precise call costs
     * ~26 ns vs ~5 ns for the coarse read (measured 2026-07-02, Ryzen 9
     * 3900X). Millisecond granularity is sufficient for every consumer:
     * reclaim thresholds are seconds, and the §3.6.1 ABA guard is the
     * `gen` CAS handshake — the lease equality re-check only compares a
     * fresh stamp against one that is already seconds stale, so tick
     * granularity can never make them collide.
     */
    static uint64_t MonotonicNanos() {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
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
     * @brief Enumerates shared-L3 logical-processor masks.
     *
     * One KAFFINITY mask per L3 cache slice reported by
     * GetLogicalProcessorInformationEx(RelationCache). On chiplet CPUs
     * (Ryzen / Threadripper / Epyc) each entry covers the LPs of one
     * CCX / CCD that share an L3 slice — pinning host worker thread N
     * and the matching guest goroutine to the same mask co-locates the
     * SlotHeader cache traffic in one L3 region, eliminating cross-CCD
     * Infinity Fabric bounces.
     *
     * Returns an empty vector when no L3 cache is reported (very old
     * Windows or constrained VM); callers should treat that as
     * "affinity unsupported" and fall through to no pinning.
     *
     * @note Layout note: the GROUP_AFFINITY inside CACHE_RELATIONSHIP
     *       moved to offset 32 (40 from the outer struct base) after
     *       Windows 10 1709 (Reserved grew to BYTE[18] + GroupCount).
     *       This implementation requires single-group entries
     *       (GroupCount == 1) — multi-group >64-LP partitions are out
     *       of scope for the AffinityLocal trial.
     */
    static std::vector<uint64_t> EnumerateCcxMasks() {
        std::vector<uint64_t> masks;
        DWORD need = 0;
        GetLogicalProcessorInformationEx(RelationCache, nullptr, &need);
        if (need == 0) return masks;
        std::vector<uint8_t> buffer(need);
        if (!GetLogicalProcessorInformationEx(
                RelationCache,
                reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()),
                &need)) {
            return masks;
        }
        size_t off = 0;
        while (off + 8 <= need) {
            uint8_t* p = buffer.data() + off;
            uint32_t rel  = *reinterpret_cast<uint32_t*>(p + 0);
            uint32_t size = *reinterpret_cast<uint32_t*>(p + 4);
            if (size == 0) break; // malformed entry
            if (rel == RelationCache && size >= 56 && off + size <= need) {
                uint8_t level = p[8];
                uint32_t cacheType = *reinterpret_cast<uint32_t*>(p + 16);
                uint16_t groupCount = *reinterpret_cast<uint16_t*>(p + 38);
                // PROCESSOR_CACHE_TYPE::CacheUnified == 0
                if (level == 3 && cacheType == 0 && groupCount == 1) {
                    uint64_t mask = *reinterpret_cast<uint64_t*>(p + 40);
                    if (mask != 0) masks.push_back(mask);
                }
            }
            off += size;
        }
        return masks;
    }

    /**
     * @brief One physical core's two SMT logical-processor masks.
     *
     * `host` and `guest` are single-bit KAFFINITY masks targeting the
     * two LPs that share L1d/L2 on a single physical core. The C++ host
     * binary should pin slot N's worker thread to `pairs[N % pairs.size()].host`;
     * the Go guest pins its slot-N goroutine to `.guest`, putting the
     * two endpoints of the ping-pong on shared L1d/L2.
     */
    struct SmtPair {
        uint64_t host;
        uint64_t guest;
    };

    /**
     * @brief Enumerates SMT-sibling LP pairs (one per SMT-capable
     *        physical core).
     *
     * Returns one (host, guest) entry per PROCESSOR_RELATIONSHIP whose
     * Flags has LTP_PC_SMT set and whose GroupMask reports exactly two
     * LPs in a single processor group. The lowest set bit becomes
     * `host`, the next-lowest becomes `guest` — the Go side derives the
     * same ordering (see shm/go/platform_windows.go::enumerateSmtPairs),
     * so the two binaries land on opposite SMT siblings of the same
     * physical core for the same slot index.
     *
     * Skipped:
     *   - cores without LTP_PC_SMT (efficiency cores, no-SMT parts)
     *   - GroupCount != 1 (multi-group >64-LP partitions)
     *   - cores not reporting exactly 2 SMT siblings (SMT-4 POWER etc.)
     *
     * Returns an empty vector when no SMT pair is reported; callers
     * should treat that as "AffinitySibling unavailable" and skip
     * pinning (falling back to a wider mask would silently break the
     * shared-L1d invariant of sibling mode).
     */
    static std::vector<SmtPair> EnumerateSmtPairs() {
        std::vector<SmtPair> pairs;
        constexpr LOGICAL_PROCESSOR_RELATIONSHIP kRelProcCore =
            RelationProcessorCore; // value 0
        DWORD need = 0;
        GetLogicalProcessorInformationEx(kRelProcCore, nullptr, &need);
        if (need == 0) return pairs;
        std::vector<uint8_t> buffer(need);
        if (!GetLogicalProcessorInformationEx(
                kRelProcCore,
                reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data()),
                &need)) {
            return pairs;
        }
        size_t off = 0;
        while (off + 8 <= need) {
            uint8_t* p = buffer.data() + off;
            uint32_t rel  = *reinterpret_cast<uint32_t*>(p + 0);
            uint32_t size = *reinterpret_cast<uint32_t*>(p + 4);
            if (size == 0) break;
            if (rel == kRelProcCore && size >= 48 && off + size <= need) {
                uint8_t flags = p[8];
                uint16_t groupCount = *reinterpret_cast<uint16_t*>(p + 30);
                // LTP_PC_SMT == 0x1
                if ((flags & 0x1) != 0 && groupCount == 1) {
                    uint64_t mask = *reinterpret_cast<uint64_t*>(p + 32);
                    // popcount via Kernighan: must be exactly 2 bits.
                    uint64_t m = mask;
                    int bits = 0;
                    while (m) { m &= (m - 1); ++bits; if (bits > 2) break; }
                    if (bits == 2) {
                        uint64_t host  = mask & (~mask + 1); // lowest set bit
                        uint64_t guest = mask & ~host;
                        pairs.push_back({host, guest});
                    }
                }
            }
            off += size;
        }
        return pairs;
    }

    /**
     * @brief Pins the current thread to the given LP mask.
     *
     * Returns the previous affinity mask (0 on failure). Caller should
     * skip the call when mask == 0 to avoid the Win32 "all LPs"
     * silent-widening interpretation.
     */
    static uint64_t PinCurrentThreadAffinity(uint64_t mask) {
        if (mask == 0) return 0;
        DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)mask);
        return (uint64_t)prev;
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
