#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <iomanip>
#include "../../src/IPC.h"
#include "ipc_generated.h"

// Define a simple Taskfile-like build script for this benchmark?
// For now, I'll rely on the user adding it to their build system,
// but I will write the code.

using namespace shm;

void worker(IPCClient* client, int id, int iterations) {
    flatbuffers::FlatBufferBuilder builder(1024);

    for (int i = 0; i < iterations; ++i) {
        builder.Clear();

        // Construct Payload directly (AddRequest)
        // We no longer use the 'Message' envelope from the old protocol
        // because the IPC layer now handles req_id.
        // But wait, the Go server expects a FlatBuffer payload.
        // Let's assume we just send the AddRequest Table as the root.

        auto addReq = ipc::CreateAddRequest(builder, 1.0 * i, 2.0 * i);

        // We must wrap it in a Message if the Go side expects it.
        // The Go code I saw earlier switched on msg.PayloadType().
        // So I should still send a Message, but I don't need to populate req_id inside it
        // OR I can populate it but it will be redundant.
        // Let's populate it for consistency with the schema, but the Transport ignores it.

        auto msg = ipc::CreateMessage(builder, 0 /* unused req_id in FB */, ipc::Payload_AddRequest, addReq.Union());
        builder.Finish(msg);

        std::vector<uint8_t> resp;
        // Call now takes (payload, size, outResp)
        if (!client->Call(builder.GetBufferPointer(), builder.GetSize(), resp)) {
            std::cerr << "Call failed!" << std::endl;
            return;
        }

        if (resp.empty()) {
             std::cerr << "Empty response" << std::endl;
             continue;
        }

        auto respMsg = ipc::GetMessage(resp.data());
        if (respMsg->payload_type() == ipc::Payload_AddResponse) {
            auto res = respMsg->payload_as_AddResponse();
            // Verify?
            // double expected = 3.0 * i;
            // if (res->result() != expected) std::cerr << "Wrong result" << std::endl;
        }
    }
}

int main() {
    IPCClient client;
    if (!client.Init("SimpleIPC", 4 * 1024 * 1024)) {
        std::cerr << "Failed to init IPC" << std::endl;
        return 1;
    }

    std::cout << "Connected to SHM. Starting Benchmark..." << std::endl;

    int numThreads = 8;
    int iterations = 100000;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; ++i) {
        threads.emplace_back(worker, &client, i, iterations);
    }

    for (auto& t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    double totalOps = (double)numThreads * iterations;
    double ops = totalOps / diff.count();
    double latency = (diff.count() * 1000000.0) / totalOps;

    std::cout << "Done." << std::endl;
    std::cout << "Total Ops: " << (long long)totalOps << std::endl;
    std::cout << "Time: " << diff.count() << " s" << std::endl;
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << ops << " ops/s" << std::endl;
    std::cout << "Avg Latency: " << latency << " us" << std::endl;

    client.Shutdown();
    return 0;
}
