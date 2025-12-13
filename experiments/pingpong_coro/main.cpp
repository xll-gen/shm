#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <cstring>
#include <atomic>
#include <semaphore.h>
#include <string>
#include <chrono>

#include "common.h"
#include "CoroutineUtils.h"
#include "Scheduler.h"
#include "ShmAwaiter.h"

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

Task CoWorker(int id, Packet* packet, int iterations, long long* out_ops) {
    std::string h_name = "/pp_h_" + std::to_string(id);
    std::string g_name = "/pp_g_" + std::to_string(id);

    sem_t* sem_host = sem_open(h_name.c_str(), O_CREAT, 0666, 0);
    sem_t* sem_guest = sem_open(g_name.c_str(), O_CREAT, 0666, 0);

    if (sem_host == SEM_FAILED || sem_guest == SEM_FAILED) {
        perror("sem_open");
        std::terminate();
    }

    packet->state.store(STATE_WAIT_REQ, std::memory_order_relaxed);
    packet->host_sleeping.store(0);
    packet->guest_sleeping.store(0);

    for (int i = 0; i < iterations; ++i) {
        packet->val_a = i;
        packet->val_b = i * 2;
        packet->req_id = i;

        packet->state.store(STATE_REQ_READY, std::memory_order_seq_cst);

        if (packet->guest_sleeping.load(std::memory_order_seq_cst) == 1) {
            sem_post(sem_guest);
        }

        while (packet->state.load(std::memory_order_acquire) != STATE_RESP_READY) {
            co_await std::suspend_always{};
        }

        if (packet->sum != (packet->val_a + packet->val_b)) {
            std::cerr << "Mismatch " << id << std::endl;
            std::terminate();
        }

        packet->state.store(STATE_WAIT_REQ, std::memory_order_relaxed);
    }

    packet->state.store(STATE_DONE, std::memory_order_seq_cst);
    if (packet->guest_sleeping.load(std::memory_order_seq_cst) == 1) {
        sem_post(sem_guest);
    }

    *out_ops = iterations;

    sem_close(sem_host);
    sem_close(sem_guest);

    co_return;
}

int main(int argc, char* argv[]) {
    int num_threads = DEFAULT_THREADS;
    if (argc > 1) num_threads = std::atoi(argv[1]);

    cleanup(num_threads);

    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) { perror("shm_open"); return 1; }

    ftruncate(fd, SHM_SIZE);
    void* ptr = mmap(0, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    Packet* packets = (Packet*)ptr;
    memset(ptr, 0, SHM_SIZE);

    for(int i=0; i<num_threads; ++i) new (&packets[i]) Packet();

    std::cout << "[Host] Coroutine Experiment. Threads: " << num_threads << std::endl;
    std::cout << "[Host] Waiting for guest..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "[Host] Starting..." << std::endl;

    Scheduler scheduler;
    std::vector<long long> ops(num_threads);
    const int ITERATIONS = 100000;

    auto start = std::chrono::high_resolution_clock::now();

    for(int i=0; i<num_threads; ++i) {
        scheduler.addTask(CoWorker(i, &packets[i], ITERATIONS, &ops[i]));
    }

    scheduler.run();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    long long total_ops = 0;
    for(long long o : ops) total_ops += o;

    std::cout << "[Host] Done. Time: " << diff.count() << "s. OPS: " << (total_ops / diff.count()) << std::endl;

    munmap(ptr, SHM_SIZE);
    close(fd);
    cleanup(num_threads);
    return 0;
}
