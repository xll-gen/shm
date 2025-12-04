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
#include <semaphore.h>
#include <string>
#include <immintrin.h> // For _mm_pause
#include "common.h"

using namespace pingpong;

const int DEFAULT_THREADS = 3;

void cleanup(int num_threads) {
    shm_unlink(SHM_NAME);
    for (int i = 0; i < num_threads; ++i) {
        std::string h_name = "/pp_h_" + std::to_string(i);
        std::string g_name = "/pp_g_" + std::to_string(i);
        sem_unlink(h_name.c_str());
        sem_unlink(g_name.c_str());
    }
}

struct WorkerResult {
    double ops;
    long long operations;
};

void worker(int id, Packet* packet, int iterations, WorkerResult& result) {
    // Open Semaphores
    std::string h_name = "/pp_h_" + std::to_string(id);
    std::string g_name = "/pp_g_" + std::to_string(id);

    // Host creates semaphores
    sem_t* sem_host = sem_open(h_name.c_str(), O_CREAT, 0666, 0);
    sem_t* sem_guest = sem_open(g_name.c_str(), O_CREAT, 0666, 0);

    if (sem_host == SEM_FAILED || sem_guest == SEM_FAILED) {
        perror("sem_open worker");
        exit(1);
    }

    auto start = std::chrono::high_resolution_clock::now();

    // Initialize packet state
    packet->state.store(STATE_WAIT_REQ, std::memory_order_relaxed);
    packet->host_sleeping.store(0);
    packet->guest_sleeping.store(0);

    // Dynamic adaptation state
    int spin_limit = 2000;
    const int MIN_SPIN = 1;
    const int MAX_SPIN = 2000;

    for (int i = 0; i < iterations; ++i) {
        packet->val_a = i;
        packet->val_b = i * 2;
        packet->req_id = i;

        // Signal request
        packet->state.store(STATE_REQ_READY, std::memory_order_seq_cst);

        // Wake guest if sleeping
        if (packet->guest_sleeping.load(std::memory_order_seq_cst) == 1) {
            sem_post(sem_guest);
        }

        // Adaptive Wait for Response
        bool ready = false;

        // 1. Spin Phase
        for (int spin = 0; spin < spin_limit; ++spin) {
            if (packet->state.load(std::memory_order_acquire) == STATE_RESP_READY) {
                ready = true;
                break;
            }
            _mm_pause();
        }

        if (ready) {
            // Case A: Success - Increase spin limit (reward)
            if (spin_limit < MAX_SPIN) spin_limit += 100;
            if (spin_limit > MAX_SPIN) spin_limit = MAX_SPIN;
        } else {
            // Case B: Failure - Decrease spin limit (punish)
            if (spin_limit > MIN_SPIN) spin_limit -= 500;
            if (spin_limit < MIN_SPIN) spin_limit = MIN_SPIN;

            // 2. Sleep Phase
            packet->host_sleeping.store(1, std::memory_order_seq_cst);

            // Double check (Critical to avoid lost wakeups)
            if (packet->state.load(std::memory_order_seq_cst) == STATE_RESP_READY) {
                ready = true;
                packet->host_sleeping.store(0, std::memory_order_relaxed);
            } else {
                // Wait loop to handle spurious wakeups
                while (packet->state.load(std::memory_order_acquire) != STATE_RESP_READY) {
                    sem_wait(sem_host);
                }
                packet->host_sleeping.store(0, std::memory_order_relaxed);
            }
        }

        // Verify
        if (packet->sum != (packet->val_a + packet->val_b)) {
            std::cerr << "Mismatch in thread " << id << " at " << i << std::endl;
            exit(1);
        }

        // Reset state for next iteration
        packet->state.store(STATE_WAIT_REQ, std::memory_order_relaxed);
    }

    auto end = std::chrono::high_resolution_clock::now();
    packet->state.store(STATE_DONE, std::memory_order_seq_cst); // Signal done

    // Wake guest so it can see STATE_DONE
    if (packet->guest_sleeping.load(std::memory_order_seq_cst) == 1) {
        sem_post(sem_guest);
    }

    std::chrono::duration<double> diff = end - start;
    result.operations = iterations;
    result.ops = iterations / diff.count();

    sem_close(sem_host);
    sem_close(sem_guest);
}

int main(int argc, char* argv[]) {
    int num_threads = DEFAULT_THREADS;
    if (argc > 1) {
        num_threads = std::atoi(argv[1]);
    }

    // Cleanup old artifacts
    cleanup(num_threads);

    std::cout << "[Host] Threads: " << num_threads << std::endl;

    // 1. Create SHM
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open");
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

    // Zero out memory
    memset(ptr, 0, SHM_SIZE);

    // Initialize all packets
    for(int i=0; i<num_threads; ++i) {
        new (&packets[i]) Packet(); // placement new
        packets[i].state.store(STATE_WAIT_REQ);
    }

    std::cout << "[Host] Waiting for guest..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1)); // Simple sync

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

    long long total_operations = 0;
    for (const auto& r : results) {
        total_operations += r.operations;
    }

    double system_effective_ops = total_operations / total_diff.count();

    std::cout << "[Host] Done. Total Time: " << total_diff.count() << "s. System Effective OPS: " << system_effective_ops << std::endl;

    munmap(ptr, SHM_SIZE);
    close(fd);
    cleanup(num_threads);
    return 0;
}
