#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>
#include <atomic>
#include "common.h"

using namespace pingpong;

void cleanup() {
    shm_unlink(SHM_NAME);
}

struct WorkerResult {
    double ops;
    long long operations;
};

void worker(int id, Packet* packet, int iterations, WorkerResult& result) {
    auto start = std::chrono::high_resolution_clock::now();

    // Initialize packet state
    packet->state.store(STATE_WAIT_REQ, std::memory_order_relaxed);

    for (int i = 0; i < iterations; ++i) {
        packet->val_a = i;
        packet->val_b = i * 2;
        packet->req_id = i;

        // Signal request
        packet->state.store(STATE_REQ_READY, std::memory_order_seq_cst);

        // Busy wait
        while (packet->state.load(std::memory_order_relaxed) != STATE_RESP_READY) {
            // Busy loop
        }

        // Verify (minimal check to save time in loop, but essential for correctness)
        if (packet->sum != (packet->val_a + packet->val_b)) {
            std::cerr << "Mismatch in thread " << id << " at " << i << std::endl;
            exit(1);
        }

        // Reset state for next iteration (logically)
        packet->state.store(STATE_WAIT_REQ, std::memory_order_relaxed);
    }

    auto end = std::chrono::high_resolution_clock::now();
    packet->state.store(STATE_DONE, std::memory_order_seq_cst); // Signal done to guest worker

    std::chrono::duration<double> diff = end - start;
    result.operations = iterations;
    result.ops = iterations / diff.count();
}

int main(int argc, char* argv[]) {
    int num_threads = 1;
    if (argc > 1) {
        num_threads = std::atoi(argv[1]);
    }

    std::cout << "[Host] Threads: " << num_threads << std::endl;

    // 1. Create SHM
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
        return 1;
    }

    // Ensure size covers all threads
    size_t required_size = sizeof(Packet) * num_threads;
    if (required_size > SHM_SIZE) {
        std::cerr << "SHM_SIZE defined in common.h is too small!" << std::endl;
        return 1;
    }

    if (ftruncate(fd, SHM_SIZE) == -1) {
        perror("ftruncate");
        return 1;
    }

    void* ptr = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    Packet* packets = (Packet*)ptr;

    // Initialize all packets
    for(int i=0; i<num_threads; ++i) {
        new (&packets[i]) Packet();
        packets[i].state.store(STATE_WAIT_REQ);
    }

    std::cout << "[Host] Waiting for guest..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "[Host] Starting benchmark..." << std::endl;

    const int ITERATIONS = 100000;
    std::vector<std::thread> threads;
    std::vector<WorkerResult> results(num_threads);

    auto total_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i, &packets[i], ITERATIONS, std::ref(results[i]));
    }

    for (auto& t : threads) {
        t.join();
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> total_diff = total_end - total_start;

    double total_ops = 0;
    for (const auto& r : results) {
        total_ops += r.ops;
    }

    std::cout << "[Host] Done. Total Time: " << total_diff.count() << "s. Total OPS: " << total_ops << std::endl;

    munmap(ptr, SHM_SIZE);
    close(fd);
    cleanup();
    return 0;
}
