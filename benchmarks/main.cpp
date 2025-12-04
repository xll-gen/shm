#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <iomanip>
#include "DirectHost.h"

using namespace shm;

void worker(DirectHost* host, int id, int iterations, long long* outOps) {
    std::vector<uint8_t> req(64); // Small payload
    std::vector<uint8_t> resp;
    // Fill req
    memset(req.data(), id, req.size());

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        // Send To Slot Explicitly (1:1 Affinity)
        int read = host->SendToSlot(id, req.data(), (uint32_t)req.size(), MSG_ID_NORMAL, resp);
        if (read < 0) {
            std::cerr << "Send failed at " << i << std::endl;
            break;
        }
        // Verify response (Echo)
        if (resp.size() != req.size()) {
             std::cerr << "Size mismatch: " << resp.size() << " vs " << req.size() << std::endl;
        }
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    *outOps = (long long)(iterations / diff.count());
}

int main(int argc, char* argv[]) {
    int numThreads = 1;
    if (argc > 1) {
        // Parse -t
        for(int i=1; i<argc; i++) {
            if (std::string(argv[i]) == "-t" && i+1 < argc) {
                numThreads = std::atoi(argv[i+1]);
            }
        }
    }

    std::cout << "Starting Benchmark with " << numThreads << " threads..." << std::endl;

    DirectHost host;
    // Align slots with threads for 1:1
    if (!host.Init("/SimpleIPC", numThreads, 4096)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    // Warmup / Wait for guest
    std::cout << "Warmup..." << std::endl;
    std::vector<uint8_t> resp;
    uint8_t data = 0;
    // Try to send one message to verify connection
    if (host.Send(&data, 1, MSG_ID_NORMAL, resp) < 0) {
         std::cerr << "Warmup failed (Guest not ready?)" << std::endl;
         return 1;
    }

    std::cout << "Running..." << std::endl;

    int iterations = 100000;
    std::vector<std::thread> threads;
    std::vector<long long> threadOps(numThreads);

    auto startTotal = std::chrono::high_resolution_clock::now();

    for(int i=0; i<numThreads; ++i) {
        threads.emplace_back(worker, &host, i, iterations, &threadOps[i]);
    }

    long long totalOpsSum = 0;
    for(int i=0; i<numThreads; ++i) {
        threads[i].join();
        totalOpsSum += threadOps[i];
    }

    auto endTotal = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> totalTime = endTotal - startTotal;

    // System Effective OPS = Total Operations / Total Time
    long long totalOperations = (long long)iterations * numThreads;
    double systemOps = totalOperations / totalTime.count();

    std::cout << "Done. Total Time: " << totalTime.count() << "s" << std::endl;
    std::cout << "System Effective OPS: " << std::fixed << std::setprecision(2) << systemOps << std::endl;

    host.Shutdown();
    return 0;
}
