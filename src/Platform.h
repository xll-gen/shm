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
    #include <immintrin.h>

    typedef sem_t* EventHandle;
    typedef int ShmHandle; // File Descriptor
#endif

namespace shm {

class Platform {
public:
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

    static void SignalEvent(EventHandle h) {
#ifdef _WIN32
        SetEvent(h);
#else
        sem_post(h);
#endif
    }

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

    static void CloseEvent(EventHandle h) {
#ifdef _WIN32
        CloseHandle(h);
#else
        sem_close(h);
#endif
    }

    // Returns mapped address or nullptr
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

    static void CloseShm(ShmHandle h, void* addr, uint64_t size) {
#ifdef _WIN32
        if (addr) UnmapViewOfFile(addr);
        if (h) CloseHandle(h);
#else
        if (addr) munmap(addr, size);
        if (h >= 0) close(h);
#endif
    }

    static void ThreadYield() {
#ifdef _WIN32
        SwitchToThread();
#else
        sched_yield();
#endif
    }

    static void CpuRelax() {
#ifdef _WIN32
        YieldProcessor();
#else
        _mm_pause();
#endif
    }

    static int GetPid() {
#ifdef _WIN32
        return (int)GetCurrentProcessId();
#else
        return (int)getpid();
#endif
    }
};

}
