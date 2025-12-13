#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <immintrin.h>
#include "common.h"

using namespace pingpong_queue;

// Simple RAII for Shared Memory
class ShmHost {
public:
    ShmHost() {
        fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
        if (fd < 0) {
            perror("shm_open");
            exit(1);
        }
        if (ftruncate(fd, sizeof(SharedMemory)) < 0) {
            perror("ftruncate");
            exit(1);
        }
        ptr = mmap(0, sizeof(SharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) {
            perror("mmap");
            exit(1);
        }
        mem = static_cast<SharedMemory*>(ptr);

        // Initialize
        mem->to_guest.head.store(0);
        mem->to_guest.tail.store(0);
        mem->to_host.head.store(0);
        mem->to_host.tail.store(0);
    }

    ~ShmHost() {
        munmap(ptr, sizeof(SharedMemory));
        close(fd);
        shm_unlink(SHM_NAME);
    }

    SharedMemory* mem;
    int fd;
    void* ptr;
};

void cpu_relax() {
    _mm_pause();
}

int main(int argc, char** argv) {
    // 1. Setup Shared Memory
    ShmHost host;
    std::cout << "Shared Memory initialized. Size: " << sizeof(SharedMemory) << " bytes." << std::endl;

    // 2. Wait for Guest (Simple check: Guest will update its tail on start if needed,
    //    or we just assume Guest starts running. For sync, we can wait 1 sec).
    std::cout << "Waiting 2s for Guest to attach..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    const int iterations = 1000000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        // --- SEND REQUEST ---
        uint64_t head = host.mem->to_guest.head.load(std::memory_order_relaxed);
        uint64_t tail = host.mem->to_guest.tail.load(std::memory_order_acquire);

        // Check full
        while ((head - tail) >= QUEUE_CAPACITY) {
            cpu_relax();
            tail = host.mem->to_guest.tail.load(std::memory_order_acquire);
        }

        // Write Data
        Message& msg = host.mem->to_guest.data[head & QUEUE_MASK];
        msg.id = i;
        msg.val_a = i;
        msg.val_b = i * 2;
        msg.sum = 0;

        // Push
        host.mem->to_guest.head.store(head + 1, std::memory_order_release);

        // --- WAIT RESPONSE ---
        uint64_t resp_head = host.mem->to_host.head.load(std::memory_order_acquire);
        uint64_t resp_tail = host.mem->to_host.tail.load(std::memory_order_relaxed);

        // Check empty
        while (resp_tail == resp_head) {
            cpu_relax();
            resp_head = host.mem->to_host.head.load(std::memory_order_acquire);
        }

        // Read Data
        Message& resp = host.mem->to_host.data[resp_tail & QUEUE_MASK];
        if (resp.id != (uint32_t)i) {
            std::cerr << "Mismatch! Expected " << i << " got " << resp.id << std::endl;
            exit(1);
        }
        if (resp.sum != (msg.val_a + msg.val_b)) {
            std::cerr << "Math error!" << std::endl;
            exit(1);
        }

        // Pop
        host.mem->to_host.tail.store(resp_tail + 1, std::memory_order_release);
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;
    double ops = iterations / diff.count();

    std::cout << "Completed " << iterations << " iterations in " << diff.count() << " seconds." << std::endl;
    std::cout << "OPS: " << (long)ops << std::endl;
    std::cout << "Avg Latency: " << (diff.count() * 1e9 / iterations) << " ns" << std::endl;

    return 0;
}
