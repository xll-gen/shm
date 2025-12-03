#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <iomanip>
#include <string>
#include <cstring>
#include <cstdint>
#include <cassert>
#include "../../src/IPCHost.h"

using namespace shm;

// Define simple POD structures for the benchmark protocol
#pragma pack(push, 1)
struct BenchmarkReq {
    int64_t id;
    double x;
    double y;
};

struct BenchmarkResp {
    int64_t id;
    double result;
};
#pragma pack(pop)

// Sanity check for struct sizes to ensure "header integrity"
static_assert(sizeof(BenchmarkReq) == 24, "BenchmarkReq size mismatch");
static_assert(sizeof(BenchmarkResp) == 16, "BenchmarkResp size mismatch");

void worker(IPCHost* host, int id, int iterations) {
    // Reusable buffer for response
    std::vector<uint8_t> respBuf;
    respBuf.reserve(128); // Pre-allocate enough space

    for (int i = 0; i < iterations; ++i) {
        BenchmarkReq req;
        req.id = i;
        req.x = 1.0 * i;
        req.y = 2.0 * i;

        // Call handles Header injection/extraction
        // We pass the raw struct as the payload
        if (!host->Call(reinterpret_cast<const uint8_t*>(&req), sizeof(req), respBuf)) {
            std::cerr << "Call failed!" << std::endl;
            return;
        }

        if (respBuf.size() != sizeof(BenchmarkResp)) {
             // In a real app we might handle this error, but for benchmark we might log once or ignore
             // if (respBuf.size() > 0) std::cerr << "Invalid response size: " << respBuf.size() << std::endl;
             continue;
        }

        // Verify result
        const BenchmarkResp* resp = reinterpret_cast<const BenchmarkResp*>(respBuf.data());
        if (resp->id != req.id) {
             std::cerr << "ID mismatch! Sent: " << req.id << ", Recv: " << resp->id << std::endl;
        }
        // Verification of calculation (x + y)
        // double expected = req.x + req.y;
        // if (std::abs(resp->result - expected) > 1e-9) { ... }
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
    std::cout << "Protocol: Raw Byte Structs (Req: 24b, Resp: 16b)" << std::endl;

    if (specificThreadCount > 0) {
        run_benchmark(specificThreadCount, iterations, mode);
    } else {
        run_benchmark(1, iterations, mode);
        run_benchmark(4, iterations, mode);
        run_benchmark(8, iterations, mode);
    }

    return 0;
}
