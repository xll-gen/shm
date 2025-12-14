#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <cstring>
#include <sstream>
#include <shm/DirectHost.h>
#include <shm/IPCUtils.h>

using namespace shm;

// Configuration
std::string SHM_NAME = "SimpleIPC";
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

std::string FormatNumber(uint64_t n) {
    std::string s = std::to_string(n);
    int insertAt = s.length() - 3;
    while (insertAt > 0) {
        s.insert(insertAt, ",");
        insertAt -= 3;
    }
    return s;
}

std::string FormatNumber(double n) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(2) << n;
    std::string s = ss.str();
    size_t decimalPos = s.find('.');
    int insertAt = (int)decimalPos - 3;
    while (insertAt > 0) {
        s.insert(insertAt, ",");
        insertAt -= 3;
    }
    return s;
}

void WorkerThread(DirectHost* host, int id) {
    std::vector<uint8_t> req(DATA_SIZE);
    for (int i = 0; i < DATA_SIZE; ++i) req[i] = (uint8_t)(i % 256);

    uint64_t localReqId = 0;
    std::vector<uint8_t> resp;
    resp.reserve(DATA_SIZE + 8);

    std::vector<uint8_t> sendBuf(DATA_SIZE + 8);

    int errorLogCount = 0;
    const int MAX_ERROR_LOGS = 5;

    while (running) {
        localReqId++;

        memcpy(sendBuf.data(), &localReqId, 8);
        memcpy(sendBuf.data() + 8, req.data(), DATA_SIZE);

        auto start = std::chrono::steady_clock::now();

        auto res = host->SendToSlot(id, sendBuf.data(), (int32_t)sendBuf.size(), MsgType::NORMAL, resp);

        auto end = std::chrono::steady_clock::now();

        if (res.HasError()) {
            globalStats.errors++;
            if (VERBOSE && errorLogCount < MAX_ERROR_LOGS) {
                std::cerr << "[Thread " << id << "] Send failed." << std::endl;
                errorLogCount++;
            }
            continue;
        }

        int read = res.Value();

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
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            std::cout << "Usage: " << argv[0] << " [options]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -t <n>          Number of threads (default: 1)" << std::endl;
            std::cout << "  -s <bytes>      Data size in bytes (default: 64)" << std::endl;
            std::cout << "  -d <seconds>    Duration in seconds (default: 10)" << std::endl;
            std::cout << "  -v              Verbose logging" << std::endl;
            std::cout << "  --guest-call    Enable Guest Call mode" << std::endl;
            return 0;
        }
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
    uint32_t numGuestSlots = GUEST_CALL_MODE ? NUM_THREADS : 0;

    uint32_t requiredSize = (DATA_SIZE + 128) * 2;
    if (requiredSize < 256) requiredSize = 256;

    HostConfig config;
    config.shmName = SHM_NAME;
    config.numHostSlots = NUM_THREADS;
    config.payloadSize = requiredSize;
    config.numGuestSlots = numGuestSlots;

    if (!host.Init(config)) {
        std::cerr << "Failed to init DirectHost." << std::endl;
        return 1;
    }

    std::cout << "Waiting for Guest to attach..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::vector<uint8_t> resp;
    uint8_t data = 0;
    int retries = 0;
    while (true) {
        if (host.Send(&data, 1, MsgType::NORMAL, resp).HasError()) {
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
    std::cout << "Running: [                                                  ] 0 %" << std::flush;
    for (int i = 0; i < DURATION_SEC; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        float progress = (float)(i + 1) / DURATION_SEC;
        int barWidth = 50;

        std::cout << "\rRunning: [";
        int pos = (int)(barWidth * progress);
        for (int j = 0; j < barWidth; ++j) {
            if (j < pos) std::cout << "=";
            else if (j == pos) std::cout << ">";
            else std::cout << " ";
        }
        std::cout << "] " << int(progress * 100.0) << " %" << std::flush;
    }
    std::cout << std::endl;

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
    std::cout << "  Total Ops:      " << FormatNumber(totalOps) << std::endl;
    std::cout << "  Throughput:     " << FormatNumber(throughput) << " ops/s" << std::endl;
    std::cout << "  Avg Latency:    " << std::fixed << std::setprecision(2) << avgLat << " us" << std::endl;
    std::cout << "  Errors:         " << FormatNumber(totalErr) << std::endl;

    return 0;
}
