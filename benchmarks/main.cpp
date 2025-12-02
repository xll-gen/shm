#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <string>
#include "../../src/IPCHost.h"
#include "../../src/DirectHost.h"
#include "ipc_generated.h"

using namespace shm;

template <typename HostT>
void worker(HostT* host, int id, int iterations) {
    flatbuffers::FlatBufferBuilder builder(1024);

    for (int i = 0; i < iterations; ++i) {
        builder.Clear();

        auto addReq = ipc::CreateAddRequest(builder, 1.0 * i, 2.0 * i);
        // Note: Generic "GenerateReqId" might not exist on DirectHost
        // We can add it or just use a local counter if DirectHost doesn't need sequential IDs globally.
        // DirectHost uses slot logic, but Protocol message needs ID.
        // Let's assume we can just pass 0 or fix DirectHost.
        // IPCHost has it. DirectHost we didn't add it.
        // I will add a simple counter here or assume HostT has GenerateReqId.
        // Let's rely on SFINAE or just add it to DirectHost.
        uint32_t reqId = i; // Simplified

        auto msg = ipc::CreateMessage(builder, reqId, ipc::Payload_AddRequest, addReq.Union());
        builder.Finish(msg);

        std::vector<uint8_t> resp;
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

// Specialization for Init arguments
template <typename HostT>
bool InitHost(HostT& host, const std::string& name, int numThreads) {
    // Default queue size logic
    return host.Init(name, 32 * 1024 * 1024);
}

template <>
bool InitHost<DirectHost>(DirectHost& host, const std::string& name, int numThreads) {
    // numSlots = numThreads (one per worker)
    // slotSize = 1MB
    return host.Init(name, numThreads, 1024 * 1024);
}

template <typename HostT>
void run_benchmark_tpl(int numThreads, int iterations) {
    HostT host;
    if (!InitHost(host, "SimpleIPC", numThreads)) {
        std::cerr << "Failed to init IPC" << std::endl;
        exit(1);
    }

    std::cout << "Starting Benchmark with " << numThreads << " threads..." << std::endl;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back(worker<HostT>, &host, i, iterations);
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
    std::string mode = "spsc"; // Default

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-t" && i + 1 < argc) {
            specificThreadCount = std::stoi(argv[++i]);
        } else if (arg == "-i" && i + 1 < argc) {
            iterations = std::stoi(argv[++i]);
        } else if (arg == "-mode" && i + 1 < argc) {
            mode = argv[++i];
        }
    }

    std::cout << "Running Benchmark in " << mode << " mode." << std::endl;

    if (mode == "direct") {
         if (specificThreadCount > 0) {
            run_benchmark_tpl<DirectHost>(specificThreadCount, iterations);
        } else {
            run_benchmark_tpl<DirectHost>(1, iterations);
            run_benchmark_tpl<DirectHost>(4, iterations);
            run_benchmark_tpl<DirectHost>(8, iterations);
        }
    } else {
        if (specificThreadCount > 0) {
            run_benchmark_tpl<IPCHost<SPSCQueue>>(specificThreadCount, iterations);
        } else {
            run_benchmark_tpl<IPCHost<SPSCQueue>>(1, iterations);
            run_benchmark_tpl<IPCHost<SPSCQueue>>(4, iterations);
            run_benchmark_tpl<IPCHost<SPSCQueue>>(8, iterations);
        }
    }

    return 0;
}
