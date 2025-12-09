#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <shm/DirectHost.h>
#include <shm/IPCUtils.h>

using namespace shm;

// Configuration
std::string SHM_NAME = "BenchmarkSHM";
int NUM_THREADS = 1;
int DATA_SIZE = 64; // Bytes
int DURATION_SEC = 10;
bool VERBOSE = false;
bool GUEST_CALL_MODE = false;

struct BenchmarkStats {
    std::atomic<uint64_t> ops{0};
    std::atomic<uint64_t> latencySum{0}; // Microseconds
    std::atomic<uint64_t> errors{0};
};

BenchmarkStats globalStats;
std::atomic<bool> running{true};

void WorkerThread(DirectHost* host, int id) {
    std::vector<uint8_t> req(DATA_SIZE);
    // Fill with pattern
    for (int i = 0; i < DATA_SIZE; ++i) req[i] = (uint8_t)(i % 256);

    // Header for protocol matching (optional, but good for verification)
    // Here we just send raw bytes as the Go server expects raw bytes or structured?
    // The Go server in benchmarks/go/main.go expects:
    // [TransportHeader][Payload] if we were using IPCHost.
    // But DirectHost sends raw payload.
    // However, the Go server *does* parse TransportHeader in its generic handler!
    // Let's check Go server code.
    // Go server:
    // func handler(req []byte, resp []byte, msgType shm.MsgType) ...
    // It assumes [8 bytes reqId] [payload].
    // So we MUST prepend 8 bytes if we want to match the Go server logic?
    // Wait, the Go server logic in `benchmarks/go/main.go` reads 8 bytes.
    // IF we are running "Direct Exchange" benchmark, we should match.

    // For this benchmark, we will manually prepend a 8-byte ID to mimic TransportHeader.
    uint64_t localReqId = 0;
    std::vector<uint8_t> resp;
    resp.reserve(DATA_SIZE + 8);

    // Pre-allocate buffer with space for header
    std::vector<uint8_t> sendBuf(DATA_SIZE + 8);

    // Error logging limiter
    int errorLogCount = 0;
    const int MAX_ERROR_LOGS = 5;

    while (running) {
        localReqId++;

        // Write Header
        memcpy(sendBuf.data(), &localReqId, 8);
        // Write Data
        memcpy(sendBuf.data() + 8, req.data(), DATA_SIZE);

        auto start = std::chrono::steady_clock::now();

        // Send to specific slot (1:1 mapping)
        // Note: DirectHost::SendToSlot blocks until response.
        int read = host->SendToSlot(id, sendBuf.data(), (int32_t)sendBuf.size(), MsgType::NORMAL, resp);

        auto end = std::chrono::steady_clock::now();

        if (read < 0) {
            globalStats.errors++;
            if (VERBOSE && errorLogCount < MAX_ERROR_LOGS) {
                std::cerr << "[Thread " << id << "] Send failed." << std::endl;
                errorLogCount++;
            }
            continue;
        }

        // Verify Response
        if (read >= 8) {
            uint64_t respId = 0;
            memcpy(&respId, resp.data(), 8);
            if (respId != localReqId) {
                globalStats.errors++;
                if (VERBOSE && errorLogCount < MAX_ERROR_LOGS) {
                     std::cerr << "[Thread " << id << "] ID Mismatch. Sent: " << localReqId << " Got: " << respId << std::endl;
                     errorLogCount++;
                }
                continue;
            }
            // Success
            globalStats.ops++;
            auto lat = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            globalStats.latencySum += lat;
        } else {
             globalStats.errors++;
             if (VERBOSE && errorLogCount < MAX_ERROR_LOGS) {
                 std::cerr << "[Thread " << id << "] Short response." << std::endl;
                 errorLogCount++;
             }
        }
    }
}

void GuestCallListener(DirectHost* host) {
    while (running) {
        host->ProcessGuestCalls([](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxRespSize, MsgType msgType) -> int32_t {
            if (msgType == MsgType::GUEST_CALL) {
                // Echo back
                // Simple echo
                int32_t copySize = reqSize;
                if (copySize > (int32_t)maxRespSize) copySize = maxRespSize;
                memcpy(resp, req, copySize);
                return copySize;
            }
            return 0;
        });
        // Yield to prevent 100% CPU on listener thread
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

int main(int argc, char** argv) {
    // Parse Args
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) NUM_THREADS = atoi(argv[++i]);
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) DATA_SIZE = atoi(argv[++i]);
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) DURATION_SEC = atoi(argv[++i]);
        if (strcmp(argv[i], "-v") == 0) VERBOSE = true;
        if (strcmp(argv[i], "--guest-call") == 0) GUEST_CALL_MODE = true;
    }

    std::cout << "Starting Benchmark:" << std::endl;
    std::cout << "  Threads: " << NUM_THREADS << std::endl;
    std::cout << "  Data Size: " << DATA_SIZE << " bytes" << std::endl;
    std::cout << "  Duration: " << DURATION_SEC << " seconds" << std::endl;
    std::cout << "  Guest Call Mode: " << (GUEST_CALL_MODE ? "Enabled" : "Disabled") << std::endl;

    DirectHost host;
    // Init Host
    // If Guest Call Mode, allocate Guest Slots.
    uint32_t numGuestSlots = GUEST_CALL_MODE ? NUM_THREADS : 0;

    // We add 64 bytes padding to slot size to accommodate header safely
    if (!host.Init(SHM_NAME, NUM_THREADS, DATA_SIZE + 128, numGuestSlots)) {
        std::cerr << "Failed to init DirectHost." << std::endl;
        return 1;
    }

    // Wait for Guest(s) to attach?
    // The DirectHost doesn't know if Guests are attached until we send and they reply.
    // We can do a warmup/handshake.
    std::cout << "Waiting for Guest to attach..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Simple handshake probe (optional)
    std::vector<uint8_t> resp;
    uint8_t data = 0;
    // We try to send to slot 0. If it fails (timeout), we wait more.
    int retries = 0;
    while (true) {
        // Send generic probe
        if (host.Send(&data, 1, MsgType::NORMAL, resp) < 0) {
            if (retries++ > 10) {
                std::cerr << "Guest not responding. Is the Go server running?" << std::endl;
                // Don't exit, maybe just slow.
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } else {
            std::cout << "Guest detected!" << std::endl;
            break;
        }
    }

    // Start Listener if needed
    std::thread listenerThread;
    if (GUEST_CALL_MODE) {
        listenerThread = std::thread(GuestCallListener, &host);
    }

    // Start Workers
    std::vector<std::thread> threads;
    for (int i = 0; i < NUM_THREADS; ++i) {
        threads.emplace_back(WorkerThread, &host, i);
    }

    // Run for Duration
    std::this_thread::sleep_for(std::chrono::seconds(DURATION_SEC));

    // Stop
    running = false;
    for (auto& t : threads) t.join();
    if (listenerThread.joinable()) listenerThread.join();

    // Send Shutdown to Guest
    std::cout << "Sending Shutdown..." << std::endl;
    host.SendShutdown();

    // Print Results
    uint64_t totalOps = globalStats.ops;
    uint64_t totalErr = globalStats.errors;
    double avgLat = totalOps > 0 ? (double)globalStats.latencySum / totalOps : 0.0;
    double throughput = (double)totalOps / DURATION_SEC;

    std::cout << "Results:" << std::endl;
    std::cout << "  Total Ops: " << totalOps << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(2) << throughput << " ops/s" << std::endl;
    std::cout << "  Avg Latency: " << avgLat << " us" << std::endl;
    std::cout << "  Errors: " << totalErr << std::endl;

    return 0;
}
