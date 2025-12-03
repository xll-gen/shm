#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <string>
#include "../../src/IPCHost.h"
#include "ipc_generated.h"

using namespace shm;

void worker(IPCHost* host, int id, int iterations) {
    flatbuffers::FlatBufferBuilder builder(1024);

    for (int i = 0; i < iterations; ++i) {
        builder.Clear();

        auto addReq = ipc::CreateAddRequest(builder, 1.0 * i, 2.0 * i);
        // Note: Transport now handles reqId internally.
        // But our FlatBuffer Schema also has a reqId field.
        // We can keep using it for application-level ID, or ignore it.
        // Let's set it to 0 or 'i' for app debugging.
        uint32_t appReqId = i;

        auto msg = ipc::CreateMessage(builder, appReqId, ipc::Payload_AddRequest, addReq.Union());
        builder.Finish(msg);

        std::vector<uint8_t> resp;
        // Call handles Header injection/extraction
        if (!host->Call(builder.GetBufferPointer(), builder.GetSize(), resp)) {
            std::cerr << "Call failed!" << std::endl;
            return;
        }

        if (resp.empty()) continue;

        auto respMsg = ipc::GetMessage(resp.data());
        if (respMsg->payload_type() == ipc::Payload_AddResponse) {
            // Success
        }
    }
}

void run_benchmark(int numThreads, int iterations, IPCMode mode) {
    IPCHost host;

    // Init param logic
    // Direct: numQueues = numThreads
    // Queue: queueSize = 32MB
    uint64_t param = (mode == IPCMode::Direct) ? numThreads : (32 * 1024 * 1024);

    if (!host.Init("SimpleIPC", mode, param)) {
        std::cerr << "Failed to init IPC" << std::endl;
        exit(1);
    }

    std::cout << "Starting Benchmark with " << numThreads << " threads..." << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back(worker, &host, i, iterations);
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    double totalOps = (double)numThreads * iterations;
    double ops = totalOps / diff.count();
    double latency = (diff.count() * 1000000.0) / totalOps;

    std::cout << "Threads: " << numThreads << std::endl;
    std::cout << "Total Ops: " << (long long)totalOps << std::endl;
    std::cout << "Time: " << diff.count() << " s" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << ops << " ops/s" << std::endl;
    std::cout << "Avg Latency: " << latency << " us" << std::endl;
    std::cout << "------------------------------------------------" << std::endl;

    host.SendShutdown();
    host.Shutdown();
}

int main(int argc, char* argv[]) {
    int iterations = 10000;
    int specificThreadCount = 0;
    std::string modeStr = "spsc"; // Default
    IPCMode mode = IPCMode::Queue;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-t" && i + 1 < argc) {
            specificThreadCount = std::stoi(argv[++i]);
        } else if (arg == "-i" && i + 1 < argc) {
            iterations = std::stoi(argv[++i]);
        } else if (arg == "-mode" && i + 1 < argc) {
            modeStr = argv[++i];
        }
    }

    if (modeStr == "direct") {
        mode = IPCMode::Direct;
    }

    std::cout << "Running Benchmark in " << modeStr << " mode." << std::endl;

    if (specificThreadCount > 0) {
        run_benchmark(specificThreadCount, iterations, mode);
    } else {
        run_benchmark(1, iterations, mode);
        run_benchmark(4, iterations, mode);
        run_benchmark(8, iterations, mode);
    }

    return 0;
}
