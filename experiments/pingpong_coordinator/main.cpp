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

// Global vector for host semaphores so Coordinator can access them
std::vector<sem_t*> global_host_sems;
std::atomic<bool> benchmark_running{true};

void cleanup(int num_threads) {
    shm_unlink(SHM_NAME);
    for (int i = 0; i < num_threads; ++i) {
        std::string h_name = "/pp_c_h_" + std::to_string(i);
        std::string g_name = "/pp_c_g_" + std::to_string(i);
        sem_unlink(h_name.c_str());
        sem_unlink(g_name.c_str());
    }
}

struct WorkerResult {
    double ops;
    long long operations;
};

// Coordinator Thread
// Polls all slots. If a slot is RESP_READY, wakes the corresponding worker.
void coordinator_thread(int num_threads, Packet* packets) {
    std::vector<uint32_t> last_processed_req(num_threads, -1);

    while (benchmark_running.load(std::memory_order_relaxed)) {
        bool idle = true;

        for (int i = 0; i < num_threads; ++i) {
            uint32_t state = packets[i].state.load(std::memory_order_acquire);

            if (state == STATE_RESP_READY) {
                uint32_t req = packets[i].req_id;
                if (req != last_processed_req[i]) {
                    // Wake up the worker
                    sem_post(global_host_sems[i]);
                    last_processed_req[i] = req;
                    idle = false;
                }
            }
        }

        if (idle) {
            _mm_pause();
        }
    }
}

void worker(int id, Packet* packet, int iterations, WorkerResult& result) {
    // Open Guest Semaphore
    std::string g_name = "/pp_c_g_" + std::to_string(id);
    sem_t* sem_guest = sem_open(g_name.c_str(), O_CREAT, 0666, 0);

    if (sem_guest == SEM_FAILED) {
        perror("sem_open worker guest");
        exit(1);
    }

    // Get my host semaphore from global list
    sem_t* sem_host = global_host_sems[id];

    auto start = std::chrono::high_resolution_clock::now();

    // Initialize packet state
    packet->state.store(STATE_WAIT_REQ, std::memory_order_relaxed);
    packet->host_sleeping.store(0); // Always 0 so Guest never calls sem_post on us directly
    packet->guest_sleeping.store(0);

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

        // Wait for Coordinator to wake us
        while (packet->state.load(std::memory_order_acquire) != STATE_RESP_READY) {
            sem_wait(sem_host);
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
    packet->state.store(STATE_DONE, std::memory_order_seq_cst);

    // Wake guest so it can see STATE_DONE
    if (packet->guest_sleeping.load(std::memory_order_seq_cst) == 1) {
        sem_post(sem_guest);
    }

    std::chrono::duration<double> diff = end - start;
    result.operations = iterations;
    result.ops = iterations / diff.count();

    sem_close(sem_guest);
}

int main(int argc, char* argv[]) {
    int num_threads = DEFAULT_THREADS;
    if (argc > 1) {
        num_threads = std::atoi(argv[1]);
    }

    cleanup(num_threads);

    std::cout << "[Host-Coordinator] Threads: " << num_threads << std::endl;

    // 1. Create SHM
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) { perror("shm_open"); return 1; }
    if (ftruncate(fd, SHM_SIZE) == -1) { perror("ftruncate"); return 1; }
    void* ptr = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) { perror("mmap"); return 1; }

    Packet* packets = (Packet*)ptr;
    memset(ptr, 0, SHM_SIZE);

    for(int i=0; i<num_threads; ++i) {
        new (&packets[i]) Packet();
        packets[i].state.store(STATE_WAIT_REQ);

        // Open Host Semaphores here for Coordinator
        std::string h_name = "/pp_c_h_" + std::to_string(i);
        sem_t* s = sem_open(h_name.c_str(), O_CREAT, 0666, 0);
        if (s == SEM_FAILED) { perror("sem_open host"); exit(1); }
        global_host_sems.push_back(s);
    }

    std::cout << "[Host] Waiting for guest..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "[Host] Starting benchmark with Coordinator..." << std::endl;

    const int ITERATIONS = 100000;
    std::vector<std::thread> threads;
    std::vector<WorkerResult> results(num_threads);
    benchmark_running = true;

    // Start Coordinator
    std::thread coord(coordinator_thread, num_threads, packets);

    auto total_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(worker, i, &packets[i], ITERATIONS, std::ref(results[i]));
    }

    for (auto& t : threads) {
        t.join();
    }

    // Stop coordinator
    benchmark_running = false;
    coord.join();

    auto total_end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> total_diff = total_end - total_start;

    long long total_operations = 0;
    for (const auto& r : results) {
        total_operations += r.operations;
    }

    double system_effective_ops = total_operations / total_diff.count();

    std::cout << "[Host] Done. Total Time: " << total_diff.count() << "s. System Effective OPS: " << system_effective_ops << std::endl;

    for(auto s : global_host_sems) sem_close(s);
    munmap(ptr, SHM_SIZE);
    close(fd);
    cleanup(num_threads);
    return 0;
}
