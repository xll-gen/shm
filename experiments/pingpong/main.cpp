#include <iostream>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <vector>
#include <cstring>
#include "common.h"

using namespace pingpong;

void cleanup() {
    shm_unlink(SHM_NAME);
}

int main() {
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

    // Initialize memory
    Packet* packet = new (ptr) Packet();
    packet->state.store(STATE_WAIT_REQ);

    std::cout << "[Host] Waiting for guest..." << std::endl;
    // Simple handshake: Wait for guest to start?
    // Actually, in this simple setup, we can just start and assume guest will join.
    // But to ensure we don't start before guest is ready, let's wait a bit or use a flag.
    // For "minimal", let's just sleep 1 sec.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    std::cout << "[Host] Starting benchmark..." << std::endl;

    const int ITERATIONS = 100000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        packet->val_a = i;
        packet->val_b = i * 2;
        packet->req_id = i;

        // Signal request
        packet->state.store(STATE_REQ_READY, std::memory_order_seq_cst);

        // Busy wait
        while (packet->state.load(std::memory_order_relaxed) != STATE_RESP_READY) {
            // Busy loop
            // _mm_pause(); // Optional optimization, keeping it pure C++ for now
        }

        // Verify
        if (packet->sum != (packet->val_a + packet->val_b)) {
            std::cerr << "Mismatch at " << i << ": " << packet->sum << " != " << (packet->val_a + packet->val_b) << std::endl;
            return 1;
        }
        if (packet->req_id != (uint32_t)i) {
             std::cerr << "ReqID mismatch at " << i << std::endl;
             return 1;
        }

        // Reset for next (not strictly needed since we overwrite, but good for clarity)
        packet->state.store(STATE_WAIT_REQ, std::memory_order_relaxed);
        // Actually, the guest will be waiting for REQ_READY, so we don't strictly need to go to WAIT_REQ
        // if the guest logic is "Wait for REQ_READY -> Process -> Set RESP_READY -> Wait for REQ_READY".
        // BUT, the guest needs to know we consumed the response.
        // Let's assume the protocol is:
        // H: Write REQ -> State=REQ
        // G: Wait REQ -> Read -> Write RESP -> State=RESP
        // H: Wait RESP -> Read -> State=WAIT (Ack) -> Write new REQ...
        // Wait, if G is fast, it might see REQ immediately.
        // The simplest "pingpong" is:
        // H sets REQ. G sets RESP.
        // H sees RESP. H sets REQ (for next ID).
        // If G sees REQ (ID N+1), it processes.
        // The only risk is if G is super fast and sees the OLD REQ?
        // No, because G sets RESP and waits for REQ.
        // H sets REQ only after seeing RESP.
        // So state transition: REQ -> RESP -> REQ -> RESP is fine.
    }

    auto end = std::chrono::high_resolution_clock::now();
    packet->state.store(STATE_DONE, std::memory_order_seq_cst);

    std::chrono::duration<double> diff = end - start;
    double ops = ITERATIONS / diff.count();

    std::cout << "[Host] Done. " << ITERATIONS << " ops in " << diff.count() << "s. OPS: " << ops << std::endl;

    munmap(ptr, SHM_SIZE);
    close(fd);
    cleanup();
    return 0;
}
