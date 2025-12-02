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
        auto msg = ipc::CreateMessage(builder, host->GenerateReqId(), ipc::Payload_AddRequest, addReq.Union());
        builder.Finish(msg);

        std::vector<uint8_t> resp;
        // Call now takes (payload, size, outResp)
        if (!host->Call(builder.GetBufferPointer(), builder.GetSize(), resp)) {
            std::cerr << "Call failed!" << std::endl;
            return;
        }

        if (resp.empty()) {
             // std::cerr << "Empty response" << std::endl;
             continue;
        }

        auto respMsg = ipc::GetMessage(resp.data());
        if (respMsg->payload_type() == ipc::Payload_AddResponse) {
            // Success
        } else {
            // std::cerr << "Unexpected payload type" << std::endl;
        }
    }
}

void run_benchmark(int numThreads, int iterations) {
    IPCHost host;
    if (!host.Init("SimpleIPC", 32 * 1024 * 1024)) {
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

void test_control_messages() {
    std::cout << "Testing Control Messages..." << std::endl;
    IPCHost host;
    if (!host.Init("SimpleIPC", 32 * 1024 * 1024)) {
        std::cerr << "Failed to init IPC for control test" << std::endl;
        return;
    }

    // Give Go side some time to connect
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Test Heartbeat
    std::cout << "Sending Heartbeat..." << std::endl;
    if (host.SendHeartbeat()) {
        std::cout << "Heartbeat SUCCESS" << std::endl;
    } else {
        std::cerr << "Heartbeat FAILED (Timeout)" << std::endl;
    }

    // Test Shutdown
    std::cout << "Sending Shutdown..." << std::endl;
    host.SendShutdown();
    std::cout << "Shutdown Sent." << std::endl;

    host.Shutdown();
    std::cout << "Test Complete." << std::endl;
    std::cout << "------------------------------------------------" << std::endl;
}


int main(int argc, char* argv[]) {
    // 1. Test Control Messages first (to ensure logic is correct)
    // test_control_messages();

    // Wait a bit for the previous Go process to exit and SHM to be cleaned up
    // std::this_thread::sleep_for(std::chrono::seconds(2));

    int iterations = 10000;
    int specificThreadCount = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-t" && i + 1 < argc) {
            specificThreadCount = std::stoi(argv[++i]);
        } else if (arg == "-i" && i + 1 < argc) {
            iterations = std::stoi(argv[++i]);
        }
    }

    if (specificThreadCount > 0) {
        run_benchmark(specificThreadCount, iterations);
    } else {
        run_benchmark(1, iterations);
        run_benchmark(4, iterations);
        run_benchmark(8, iterations);
    }

    return 0;
}
