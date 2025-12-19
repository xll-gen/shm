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
#include <iomanip>
#include "common.h"

using namespace pingpong_multiring;

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
        std::memset(mem, 0, sizeof(SharedMemory));
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

void worker(int thread_id, SharedMemory* mem, std::atomic<bool>* running, std::atomic<long>* ops) {
    long local_ops = 0;
    int i = 0;
    RingBuffer& q_out = mem->to_guest[thread_id];
    RingBuffer& q_in = mem->to_host[thread_id];

    while (running->load(std::memory_order_relaxed)) {
        // --- SEND ---
        uint64_t head = q_out.head.load(std::memory_order_relaxed);
        uint64_t tail = q_out.tail.load(std::memory_order_acquire);

        if ((head - tail) >= QUEUE_CAPACITY) {
            cpu_relax();
            continue;
        }

        Message& msg = q_out.data[head & QUEUE_MASK];
        msg.id = i;
        msg.val_a = i;
        msg.val_b = i * 2;
        msg.sum = 0;

        q_out.head.store(head + 1, std::memory_order_release);

        // --- RECV ---
        uint64_t resp_head = q_in.head.load(std::memory_order_acquire);
        uint64_t resp_tail = q_in.tail.load(std::memory_order_relaxed);

        while (resp_tail == resp_head && running->load(std::memory_order_relaxed)) {
            cpu_relax();
            resp_head = q_in.head.load(std::memory_order_acquire);
        }

        if (!running->load(std::memory_order_relaxed)) break;

        Message& resp = q_in.data[resp_tail & QUEUE_MASK];
        if (resp.sum != (msg.val_a + msg.val_b)) {
             // Error
        }

        q_in.tail.store(resp_tail + 1, std::memory_order_release);

        local_ops++;
        i++;
    }
    *ops += local_ops;
}

int main(int argc, char** argv) {
    int num_threads = 1;
    if (argc > 1) num_threads = std::atoi(argv[1]);
    if (num_threads > MAX_THREADS) num_threads = MAX_THREADS;

    ShmHost host;
    std::cout << "Shared Memory initialized. Size: " << sizeof(SharedMemory) << " bytes." << std::endl;
    std::cout << "Running with " << num_threads << " threads." << std::endl;

    std::cout << "Waiting 2s for Guest..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::atomic<bool> running(true);
    std::vector<std::thread> threads;
    std::vector<std::atomic<long>> ops(num_threads);

    for(int i=0; i<num_threads; ++i) {
        ops[i] = 0;
        threads.emplace_back(worker, i, host.mem, &running, &ops[i]);
    }

    auto start = std::chrono::high_resolution_clock::now();
    std::this_thread::sleep_for(std::chrono::seconds(5)); // Run for 5 seconds
    running.store(false);

    long total_ops = 0;
    for(int i=0; i<num_threads; ++i) {
        threads[i].join();
        total_ops += ops[i].load();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    std::cout << "Total Ops: " << total_ops << std::endl;
    std::cout << "Time: " << diff.count() << "s" << std::endl;
    std::cout << "Throughput: " << (long)(total_ops / diff.count()) << " ops/sec" << std::endl;
    std::cout << "Avg Latency: " << (diff.count() * 1e9 / total_ops) * num_threads << " ns (RTT)" << std::endl;

    return 0;
}
