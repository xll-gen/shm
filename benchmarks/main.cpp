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
#include <shm/Stream.h>
#include <shm/WaitStrategy.h>

using namespace shm;

// Configuration
std::string SHM_NAME = "SimpleIPC";
int NUM_THREADS = 1;
int DATA_SIZE = 64; // Bytes (Normal mode) or Total Stream Size (Stream Mode)
int CHUNK_SIZE = 4096; // Bytes (Stream Mode)
int DURATION_SEC = 10;
bool VERBOSE = false;
bool GUEST_CALL_MODE = false;
bool STREAM_MODE = false;
int IN_FLIGHT = 0; // 0 = mode-default (stream: 4, normal: 1). Set via -i <n>.
// AffinityMode (mirrors Go's affinity.go enum):
//   0 = auto   (default; pin to CCX[N % numCCX] iff multi-CCX, else no-op)
//   1 = none   (explicit off)
//   2 = local  (force pin even on monolithic-L3 hosts)
// Set via --affinity auto | none | local.
int AFFINITY_MODE = 0;

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

void WorkerThread(DirectHost* host, int id, int totalSlots) {
    // CPU affinity: pin this host worker thread to the same CCX (shared-L3
    // LP set) that the corresponding guest worker pins to, via a
    // deterministic slot-index round-robin mapping. Co-locating the host
    // and guest on one L3 region eliminates the cross-CCD Infinity Fabric
    // bounce on every state-line transition (Ryzen 3900X: ~80-100ns ->
    // ~30-50ns per bounce on Zen 2). PAUSE-every-iter handles the same-
    // physical-core SMT-sibling case if the OS scheduler places us there
    // inside the CCX mask.
    //
    // Mode semantics (mirror Go's affinity.go):
    //   0 = auto:  pin iff multi-CCX (chiplet AMD, multi-socket Xeon).
    //              Skip on monolithic-L3 hosts (most single-socket Intel
    //              desktops) where same-as-all-LPs mask adds no value.
    //   1 = none:  explicit opt-out.
    //   2 = local: force pin even with len(masks)==1.
    if (AFFINITY_MODE != 1) {
        auto masks = shm::Platform::EnumerateCcxMasks();
        if (!masks.empty() && !(AFFINITY_MODE == 0 && masks.size() <= 1)) {
            shm::Platform::PinCurrentThreadAffinity(masks[id % masks.size()]);
        }
    }

    std::vector<uint8_t> req(DATA_SIZE);
    for (int i = 0; i < DATA_SIZE; ++i) req[i] = (uint8_t)(i % 256);

    uint64_t localReqId = 0;
    std::vector<uint8_t> resp;
    resp.reserve(DATA_SIZE + 8);

    std::vector<uint8_t> sendBuf(DATA_SIZE + 8);

    int errorLogCount = 0;
    const int MAX_ERROR_LOGS = 5;

    // Stream-mode pipelining: each worker uses IN_FLIGHT slots from the pool,
    // so main() allocates numHostSlots = NUM_THREADS * IN_FLIGHT in stream mode.
    // Normal mode keeps the 1:1 thread-to-slot mapping (maxInFlight=1).
    // Default IN_FLIGHT=4 for stream when not set (overlaps host memcpy
    // of chunk N with guest memcpy of chunk N-1; the bench prior to v0.7.12
    // hard-clamped to 1, severely under-reporting StreamSender's capability).
    int maxInFlight = STREAM_MODE ? IN_FLIGHT : 1;

    StreamSender streamSender(host, maxInFlight);

    while (running) {
        localReqId++;
        auto start = std::chrono::steady_clock::now();

        if (STREAM_MODE) {
             // Send Stream
             // Use localReqId as StreamId
             // Combine id (thread idx) and localReqId to make it unique across threads
             uint64_t streamId = ((uint64_t)id << 48) | localReqId;

             auto res = streamSender.Send(req.data(), req.size(), streamId);
             auto end = std::chrono::steady_clock::now();

             if (res.HasError()) {
                 globalStats.errors++;
                 if (VERBOSE && errorLogCount < MAX_ERROR_LOGS) {
                     std::cerr << "[Thread " << id << "] Stream Send failed: " << (int)res.GetError() << std::endl;
                     errorLogCount++;
                 }
             } else {
                 globalStats.ops++;
                 auto lat = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
                 globalStats.latencySum += lat;
             }

        } else {
            // Normal Mode
            memcpy(sendBuf.data(), &localReqId, 8);
            memcpy(sendBuf.data() + 8, req.data(), DATA_SIZE);

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
            std::cout << "  -s <bytes>      Data size (default: 64). In Stream Mode, this is total stream size." << std::endl;
            std::cout << "  -c <bytes>      Chunk size for Stream Mode (default: 4096)." << std::endl;
            std::cout << "  -d <seconds>    Duration in seconds (default: 10)" << std::endl;
            std::cout << "  -i <n>          In-flight chunks per stream worker (default: 4 in stream mode, 1 otherwise)" << std::endl;
            std::cout << "  --affinity <m>  Worker thread CPU affinity: auto | none | local (default: auto)" << std::endl;
            std::cout << "  -v              Verbose logging" << std::endl;
            std::cout << "  --name <name>   SHM Name (default: SimpleIPC)" << std::endl;
            std::cout << "  --guest-call    Enable Guest Call mode" << std::endl;
            std::cout << "  --stream        Enable Stream mode" << std::endl;
            return 0;
        }
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) NUM_THREADS = atoi(argv[++i]);
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) DATA_SIZE = atoi(argv[++i]);
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) CHUNK_SIZE = atoi(argv[++i]);
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) DURATION_SEC = atoi(argv[++i]);
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) IN_FLIGHT = atoi(argv[++i]);
        if (strcmp(argv[i], "--affinity") == 0 && i + 1 < argc) {
            const char* m = argv[++i];
            if (strcmp(m, "auto") == 0) AFFINITY_MODE = 0;
            else if (strcmp(m, "none") == 0) AFFINITY_MODE = 1;
            else if (strcmp(m, "local") == 0) AFFINITY_MODE = 2;
            else { std::cerr << "Unknown --affinity value: " << m << " (use 'auto', 'none', or 'local')\n"; return 1; }
        }
        if (strcmp(argv[i], "-v") == 0) VERBOSE = true;
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) SHM_NAME = argv[++i];
        if (strcmp(argv[i], "--guest-call") == 0) GUEST_CALL_MODE = true;
        if (strcmp(argv[i], "--stream") == 0) STREAM_MODE = true;
    }

    // Mode-default for IN_FLIGHT (0 sentinel = unset).
    // Default 1 across both modes to preserve v0.7.11-baseline-comparable numbers;
    // the bench can opt into pipelining via -i <n>. Empirical sweep on Ryzen 9
    // 3900X (2026-06-26): inFlight 1→8 on 1T/16MiB/4MiB-chunks yielded only
    // ~8% throughput gain (191→206 ops/s) and inFlight=4 on 1T/1MiB/256K-chunks
    // regressed by ~31% — the host/guest memcpys share the same memory controller,
    // so overlapping them in time does not relieve the dominant bandwidth bottleneck.
    if (IN_FLIGHT <= 0) IN_FLIGHT = 1;
    if (!STREAM_MODE && IN_FLIGHT != 1) IN_FLIGHT = 1; // pipelining only applies to stream mode

    std::cout << "Starting Benchmark:" << std::endl;
    std::cout << "  SHM Name: " << SHM_NAME << std::endl;
    std::cout << "  Threads: " << NUM_THREADS << std::endl;
    std::cout << "  Data Size: " << DATA_SIZE << " bytes" << std::endl;
    if (STREAM_MODE) {
        std::cout << "  Mode: Stream" << std::endl;
        std::cout << "  Chunk Size: " << CHUNK_SIZE << " bytes" << std::endl;
        std::cout << "  In-Flight: " << IN_FLIGHT << std::endl;
    } else {
        std::cout << "  Mode: Normal" << std::endl;
    }
    std::cout << "  Duration: " << DURATION_SEC << " seconds" << std::endl;
    std::cout << "  Guest Call Mode: " << (GUEST_CALL_MODE ? "Enabled" : "Disabled") << std::endl;
    {
        auto masks = shm::Platform::EnumerateCcxMasks();
        const char* modeLabel = (AFFINITY_MODE == 0) ? "auto" :
                                (AFFINITY_MODE == 1) ? "none" : "local";
        std::cout << "  Affinity: " << modeLabel
                  << " (CCXs detected: " << masks.size() << ")" << std::endl;
    }

    DirectHost host;
    uint32_t numGuestSlots = GUEST_CALL_MODE ? NUM_THREADS : 0;

    uint32_t payloadSize = 0;
    if (STREAM_MODE) {
        // Ensure payload size can hold chunk + header
        payloadSize = CHUNK_SIZE + sizeof(ChunkHeader) + 128;
    } else {
        payloadSize = (DATA_SIZE + 128) * 2;
        if (payloadSize < 256) payloadSize = 256;
    }

    HostConfig config;
    config.shmName = SHM_NAME;
    // Stream mode: each worker can have IN_FLIGHT chunks outstanding, each
    // sitting in its own slot. Normal mode keeps the 1:1 thread-to-slot
    // mapping that the Direct Exchange model is built around.
    config.numHostSlots = STREAM_MODE
        ? static_cast<uint32_t>(NUM_THREADS) * static_cast<uint32_t>(IN_FLIGHT)
        : static_cast<uint32_t>(NUM_THREADS);
    config.payloadSize = payloadSize;
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
        // In stream mode, sending NORMAL msg might not be handled if guest expects only stream?
        // But Host always sends NORMAL for handshake here.
        // Guest should handle NORMAL as well or fallback.
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
        threads.emplace_back(WorkerThread, &host, i, NUM_THREADS);
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
    double totalBytes = (double)totalOps * DATA_SIZE;
    double throughputBytes = totalBytes / DURATION_SEC;

    std::cout << "Results:" << std::endl;
    std::cout << "  Total Ops:      " << FormatNumber(totalOps) << std::endl;
    std::cout << "  Throughput:     " << FormatNumber(throughput) << " ops/s" << std::endl;
    if (STREAM_MODE) {
        std::cout << "  Bandwidth:      " << FormatNumber(throughputBytes / (1024*1024)) << " MB/s" << std::endl;
    }
    std::cout << "  Avg Latency:    " << std::fixed << std::setprecision(2) << avgLat << " us" << std::endl;
    std::cout << "  Errors:         " << FormatNumber(totalErr) << std::endl;

    // NOTE: the host-side [HostWS] spin-stats diagnostic that used to print here
    // referenced shm::WaitStrategyStats, which no longer exists in
    // include/shm/WaitStrategy.h (the C++ host WaitStrategy is a header-only
    // class with no global counters — the Go side likewise gates its WaitStats
    // behind the shm_benchstats build tag). The block was dropped to unblock the
    // benchmark build; throughput/latency above are unaffected. See
    // IMPROVEMENT_BACKLOG.md (shm benchmark drift).

    return 0;
}
