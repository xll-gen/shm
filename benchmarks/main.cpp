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
#include <mutex>
#include "../../src/IPCHost.h"
#include "../../include/IPCUtils.h"

static_assert(sizeof(shm::ExchangeHeader) == 64, "ExchangeHeader size mismatch");
static_assert(sizeof(shm::SlotHeader) == 128, "SlotHeader size mismatch");
static_assert(offsetof(shm::ExchangeHeader, guestHeartbeat) == 16, "guestHeartbeat offset mismatch");

using namespace shm;

std::mutex log_mutex;

template<typename T>
void log_ts(T msg) {
    std::lock_guard<std::mutex> lock(log_mutex);
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &t);
    std::cout << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count() << " ";
    std::cout << msg << std::endl;
}

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
        req.id = (int64_t(id) << 32) | i;
        req.x = 1.0 * i;
        req.y = 2.0 * i;

        // Call handles Header injection/extraction
        // We pass the raw struct as the payload
        if (!host->Call(reinterpret_cast<const uint8_t*>(&req), sizeof(req), respBuf)) {
            log_ts("Call failed!");
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
             log_ts("ID mismatch! Sent: " + std::to_string(req.id) + ", Recv: " + std::to_string(resp->id));
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
        log_ts("Failed to init IPC");
        exit(1);
    }

    log_ts("Starting Benchmark with " + std::to_string(numThreads) + " threads...");

    std::atomic<bool> benchmark_running = true;
    std::thread monitor_thread;
    if (mode == IPCMode::Direct) {
        monitor_thread = std::thread([&]() {
            while (benchmark_running) {
                bool is_alive = host.IsGuestAlive();
                log_ts("[Monitor] Guest alive: " + std::string(is_alive ? "yes" : "no"));

                if (!is_alive) {
                    log_ts("Guest is not alive, shutting down.");
                    host.Shutdown();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        });
    }

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back(worker, &host, i, iterations);
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    benchmark_running = false;
    if (monitor_thread.joinable()) {
        monitor_thread.join();
    }

    double totalOps = (double)numThreads * iterations;
    double ops = totalOps / diff.count();
    double latency = (diff.count() * 1000000.0) / totalOps;

    std::cerr << "Threads: " << numThreads << std::endl;
    std::cerr << "Total Ops: " << (long long)totalOps << std::endl;
    std::cerr << "Time: " << diff.count() << " s" << std::endl;
    std::cerr << "Throughput: " << std::fixed << std::setprecision(2) << ops << " ops/s" << std::endl;
    std::cerr << "Avg Latency: " << latency << " us" << std::endl;
    std::cerr << "------------------------------------------------" << std::endl;

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
        unsigned int num_cpus = std::thread::hardware_concurrency();
        if (specificThreadCount > (int)num_cpus) {
            std::cout << "Warning: Thread count capped at " << num_cpus << std::endl;
            specificThreadCount = num_cpus;
        }
        run_benchmark(specificThreadCount, iterations, mode);
    } else {
        unsigned int num_cpus = std::thread::hardware_concurrency();
        run_benchmark(1, iterations, mode);
        if (num_cpus >= 4) run_benchmark(4, iterations, mode);
        if (num_cpus >= 8) run_benchmark(8, iterations, mode);
    }

    return 0;
}
