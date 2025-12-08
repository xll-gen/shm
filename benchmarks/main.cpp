#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <iomanip>
#include <shm/DirectHost.h>

using namespace shm;

/**
 * @brief Worker function for a single benchmark thread.
 *
 * Each worker pairs with a specific slot (via ID) and performs a fixed number of
 * request-response cycles.
 *
 * @param host Pointer to the DirectHost instance.
 * @param id The thread/slot ID (0 to N-1).
 * @param iterations Number of operations to perform.
 * @param[out] outOps Pointer to store the calculated OPS (Operations Per Second) for this worker.
 */
void worker(DirectHost* host, int id, int iterations, long long* outOps, bool verbose) {
    std::vector<uint8_t> req(64); // Small payload
    std::vector<uint8_t> resp;
    // Fill req
    memset(req.data(), id, req.size());

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; ++i) {
        if (verbose && (i % 100 == 0)) {
            std::cout << "[Thread " << id << "] Op " << i << std::endl;
        }
        // Send To Slot Explicitly (1:1 Affinity)
        int read = host->SendToSlot(id, req.data(), (int32_t)req.size(), MSG_TYPE_NORMAL, resp);
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

/**
 * @brief Main entry point for the C++ Benchmark Host.
 *
 * Parses command line arguments, initializes the Host, and spawns worker threads.
 * Calculates and prints the total System Effective OPS.
 *
 * Arguments:
 *   -t <num>: Number of threads (default 1).
 *   --guest-call: Enable Guest Call scenario (Host triggers Guest, Guest triggers Host).
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return int Exit code.
 */
int main(int argc, char* argv[]) {
    int numThreads = 1;
    bool verbose = false;
    bool enableGuestCall = false;

    if (argc > 1) {
        // Parse args
        for(int i=1; i<argc; i++) {
            if (std::string(argv[i]) == "-t" && i+1 < argc) {
                numThreads = std::atoi(argv[i+1]);
            } else if (std::string(argv[i]) == "-v") {
                verbose = true;
            } else if (std::string(argv[i]) == "--guest-call") {
                enableGuestCall = true;
            }
        }
    }

    std::cout << "Starting Benchmark with " << numThreads << " threads..." << std::endl;
    if (enableGuestCall) {
        std::cout << "Guest Call scenario enabled." << std::endl;
    }
    if (verbose) {
        std::cout << "Verbose mode enabled (logging every 100 ops)" << std::endl;
    }

    DirectHost host;
    // Align slots with threads for 1:1
    // If Guest Call enabled, we allocate guest slots equal to host slots (threads)
    uint32_t numGuestSlots = enableGuestCall ? numThreads : 0;

    if (!host.Init("SimpleIPC", numThreads, 4096, numGuestSlots)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    // Background thread for processing Guest Calls
    std::atomic<bool> stopGuestCall{false};
    std::thread guestCallThread;
    if (enableGuestCall) {
        guestCallThread = std::thread([&](){
            while(!stopGuestCall) {
                host.ProcessGuestCalls([](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxRespSize, uint32_t msgType) -> int32_t {
                    // Simple ACK/Echo for guest call
                    // We assume small payload
                    const char* ack = "OK";
                    if (maxRespSize < 2) return 0;
                    memcpy(resp, ack, 2);
                    return 2;
                });
                Platform::CpuRelax();
            }
        });
    }

    // Warmup / Wait for guest
    std::cout << "Warmup..." << std::endl;
    std::vector<uint8_t> resp;
    uint8_t data = 0;
    // Try to send one message to verify connection
    if (host.Send(&data, 1, MSG_TYPE_NORMAL, resp) < 0) {
         std::cerr << "Warmup failed (Guest not ready?)" << std::endl;
         stopGuestCall = true;
         if (guestCallThread.joinable()) guestCallThread.join();
         return 1;
    }

    std::cout << "Running..." << std::endl;

    int iterations = 100000;
    std::vector<std::thread> threads;
    std::vector<long long> threadOps(numThreads);

    auto startTotal = std::chrono::high_resolution_clock::now();

    for(int i=0; i<numThreads; ++i) {
        threads.emplace_back(worker, &host, i, iterations, &threadOps[i], verbose);
    }

    long long totalOpsSum = 0;
    for(int i=0; i<numThreads; ++i) {
        threads[i].join();
        totalOpsSum += threadOps[i];
    }

    auto endTotal = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> totalTime = endTotal - startTotal;

    // Stop guest call thread
    stopGuestCall = true;
    if (guestCallThread.joinable()) guestCallThread.join();

    // System Effective OPS = Total Operations / Total Time
    long long totalOperations = (long long)iterations * numThreads;
    double systemOps = totalOperations / totalTime.count();

    std::cout << "Done. Total Time: " << totalTime.count() << "s" << std::endl;
    std::cout << "System Effective OPS: " << std::fixed << std::setprecision(2) << systemOps << std::endl;

    host.SendShutdown();
    host.Shutdown();
    return 0;
}
