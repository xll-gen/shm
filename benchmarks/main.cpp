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
// Normal mode sends via a per-worker held-slot session (AcquireHeldSlot once,
// SendHeld per op) — the claim-free fast path added in v0.8.5. --legacy-claim
// restores the per-op AcquireSpecificSlot/SLOT_FREE cycle for A/B comparison.
bool LEGACY_CLAIM = false;
int IN_FLIGHT = 0; // 0 = mode-default (stream: 2 = library StreamSender default, normal: 1). Set via -i <n>.
// AffinityMode (mirrors Go's affinity.go enum):
//   0 = auto    (default; pin to CCX[N % numCCX] iff multi-CCX, else no-op)
//   1 = none    (explicit off)
//   2 = local   (force pin even on monolithic-L3 hosts)
//   3 = sibling (opt-in; pin host worker N to one SMT LP of physical core
//                [N % numPairs], guest goroutine to the other LP — see
//                shm/go/affinity.go::AffinitySibling)
// Set via --affinity auto | none | local | sibling.
int AFFINITY_MODE = 0;

// Per-worker stats, one cache line each. The previous version was a single
// global struct of three adjacent std::atomic<uint64_t> — one shared cache
// line hammered with two RMWs per op from every worker thread, the exact
// counterpart of the Go-side WaitStats false-sharing defect fixed 2026-06-26
// (go/spin.go paddedU64, +4–9% at 4T/8T). Each worker is the sole writer of
// its own entry and main() reads only after join(), so plain (non-atomic)
// fields are sufficient and the per-op RMWs disappear entirely.
struct alignas(64) BenchmarkStats {
    uint64_t ops{0};
    uint64_t latencySumNs{0}; // Nanoseconds, over sampled ops only
    uint64_t samples{0};      // Ops that contributed to latencySumNs
    uint64_t errors{0};
};
static_assert(sizeof(BenchmarkStats) == 64, "one cache line per worker");

// Latency is sampled (1-in-61 ops) rather than timed per-op: two
// steady_clock::now() calls (QPC, ~2x20 ns) inside every measured op are
// benchmark overhead of the same order as the IPC RTT itself, and the old
// microsecond truncation reported 0 for sub-us RTTs. Prime stride avoids
// op-index-correlated periodicity with the guest's Gosched-every-4096 cadence.
constexpr int kLatencySampleStride = 61;

std::vector<BenchmarkStats> perThreadStats; // sized in main() before workers start
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
    BenchmarkStats& stats = perThreadStats[id];
    // CPU affinity: pin this host worker thread to the LP its matching
    // guest goroutine targets, via a deterministic slot-index round-robin
    // mapping. Mode semantics (mirror Go's affinity.go::resolveAuto):
    //
    //   0 = auto:    chipset-aware. If LTP_PC_SMT pairs are reported and
    //                totalSlots fits within numPairs → Sibling. Else if
    //                multi-CCX → Local. Else → no-pin.
    //   1 = none:    explicit opt-out.
    //   2 = local:   force CCX-mask pin even on monolithic-L3 hosts.
    //   3 = sibling: pin host worker N to the host LP of physical core
    //                [N % numPairs]; the matching Go guest pins to the
    //                other LP, putting both endpoints on shared L1d/L2.
    //
    // The shared-L1d invariant of Sibling mode is documented at length
    // in shm/EXPERIMENTS.md §"2026-06-26 SMT-sibling co-location".
    int resolved = AFFINITY_MODE;
    if (resolved == 0) {
        auto pairs = shm::Platform::EnumerateSmtPairs();
        auto masks = shm::Platform::EnumerateCcxMasks();
        if (!pairs.empty() && totalSlots > 0 && totalSlots <= static_cast<int>(pairs.size())) {
            resolved = 3; // sibling
        } else if (masks.size() > 1) {
            resolved = 2; // local
        } else {
            resolved = 1; // none
        }
    }
    if (resolved == 3) {
        auto pairs = shm::Platform::EnumerateSmtPairs();
        if (!pairs.empty()) {
            shm::Platform::PinCurrentThreadAffinity(pairs[id % pairs.size()].host);
        }
    } else if (resolved == 2) {
        auto masks = shm::Platform::EnumerateCcxMasks();
        if (!masks.empty()) {
            shm::Platform::PinCurrentThreadAffinity(masks[id % masks.size()]);
        }
    }

    std::vector<uint8_t> req(DATA_SIZE);
    for (int i = 0; i < DATA_SIZE; ++i) req[i] = (uint8_t)(i % 256);

    uint64_t localReqId = 0;
    std::vector<uint8_t> resp;
    resp.reserve(DATA_SIZE + 8);

    std::vector<uint8_t> sendBuf(DATA_SIZE + 8);
    // The payload bytes after the 8-byte reqId are invariant across ops:
    // fill them once here so the per-op work is a single 8-byte write.
    memcpy(sendBuf.data() + 8, req.data(), DATA_SIZE);

    int errorLogCount = 0;
    const int MAX_ERROR_LOGS = 5;

    // Stream-mode pipelining: each worker uses IN_FLIGHT slots from the pool,
    // so main() allocates numHostSlots = NUM_THREADS * IN_FLIGHT in stream mode.
    // Normal mode keeps the 1:1 thread-to-slot mapping (maxInFlight=1).
    int maxInFlight = STREAM_MODE ? IN_FLIGHT : 1;

    // Fixed slot range per worker (v0.8.7): worker `id` draws its stream slots
    // from [id*maxInFlight, id*maxInFlight+maxInFlight). With maxInFlight==1
    // this is slot==id, co-locating this pinned host thread with the Go guest
    // worker that services slot id on the sibling LP (matches numHostSlots =
    // NUM_THREADS*IN_FLIGHT allocated in main). Restores the sibling-affinity
    // win the shared pool scrambles.
    StreamSender streamSender(host, maxInFlight, STREAM_MODE ? id * maxInFlight : -1);

    // Held-slot session for normal mode (default): claim this worker's
    // dedicated slot once, re-arm per op. Reacquired inside the loop if a
    // send error disowns it.
    DirectHost::HeldSlot held;
    if (!STREAM_MODE && !LEGACY_CLAIM) {
        held = host->AcquireHeldSlot(id);
    }

    int sampleCountdown = 1; // sample the 1st op, then every kLatencySampleStride-th
    std::chrono::steady_clock::time_point start;

    while (running) {
        localReqId++;
        bool sampled = STREAM_MODE || (--sampleCountdown == 0);
        if (sampled) {
            sampleCountdown = kLatencySampleStride;
            start = std::chrono::steady_clock::now();
        }

        if (STREAM_MODE) {
             // Send Stream
             // Use localReqId as StreamId
             // Combine id (thread idx) and localReqId to make it unique across threads
             uint64_t streamId = ((uint64_t)id << 48) | localReqId;

             auto res = streamSender.Send(req.data(), req.size(), streamId);
             auto end = std::chrono::steady_clock::now();

             if (res.HasError()) {
                 stats.errors++;
                 if (VERBOSE && errorLogCount < MAX_ERROR_LOGS) {
                     std::cerr << "[Thread " << id << "] Stream Send failed: " << (int)res.GetError() << std::endl;
                     errorLogCount++;
                 }
             } else {
                 stats.ops++;
                 stats.latencySumNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                 stats.samples++;
             }

        } else {
            // Normal Mode
            memcpy(sendBuf.data(), &localReqId, 8);

            Result<int> res(Error::InvalidArgs);
            if (LEGACY_CLAIM) {
                res = host->SendToSlot(id, sendBuf.data(), (int32_t)sendBuf.size(), MsgType::NORMAL, resp);
            } else {
                if (!held.IsValid()) {
                    held = host->AcquireHeldSlot(id);
                    if (!held.IsValid()) {
                        // Acquire can fail when `running` flips during
                        // shutdown; re-check the loop condition instead of
                        // writing through a null GetReqBuffer().
                        stats.errors++;
                        continue;
                    }
                }
                // Request copy into the shm slot, same work SendToSlot does
                // in-library on the legacy path (inline fast path, see
                // Platform::CopySmall).
                Platform::CopySmall(held.GetReqBuffer(), sendBuf.data(), sendBuf.size());
                res = held.Send((int32_t)sendBuf.size(), MsgType::NORMAL, resp);
            }

            if (res.HasError()) {
                stats.errors++;
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
                    stats.errors++;
                    if (VERBOSE && errorLogCount < MAX_ERROR_LOGS) {
                        std::cerr << "[Thread " << id << "] ID Mismatch. Sent: " << localReqId << " Got: " << respId << std::endl;
                        errorLogCount++;
                    }
                    continue;
                }
                stats.ops++;
                if (sampled) {
                    auto end = std::chrono::steady_clock::now();
                    stats.latencySumNs += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
                    stats.samples++;
                }
            } else {
                stats.errors++;
                if (VERBOSE && errorLogCount < MAX_ERROR_LOGS) {
                    std::cerr << "[Thread " << id << "] Short response." << std::endl;
                    errorLogCount++;
                }
            }
        }
    }
}

// Guest-call echo handler, served by the library's event-driven worker
// (DirectHost::Start → GuestCallWorker::GuestWorkerLoop). The previous
// hand-rolled listener polled ProcessGuestCalls with an unconditional 1 ms
// sleep, so the cell measured the poll quantum rather than the IPC — every
// guest call parked and per-call latency was dominated by up to 1 ms of
// listener sleep. The production worker blocks on the shared guest-call
// request event instead.
int32_t GuestCallEcho(const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxRespSize, MsgType msgType) {
    if (msgType == MsgType::GUEST_CALL) {
        int32_t copySize = reqSize;
        if (copySize > (int32_t)maxRespSize) copySize = maxRespSize;
        memcpy(resp, req, copySize);
        return copySize;
    }
    return 0;
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
            std::cout << "  -i <n>          In-flight chunks per stream worker (default: 1; depth 2 measured slower on this workload, see main.cpp)" << std::endl;
            std::cout << "  --legacy-claim  Normal mode: per-op claim/free cycle instead of the held-slot session (A/B)" << std::endl;
            std::cout << "  --affinity <m>  Worker thread CPU affinity: auto | none | local | sibling (default: auto)" << std::endl;
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
            else if (strcmp(m, "sibling") == 0) AFFINITY_MODE = 3;
            else { std::cerr << "Unknown --affinity value: " << m << " (use 'auto', 'none', 'local', or 'sibling')\n"; return 1; }
        }
        if (strcmp(argv[i], "-v") == 0) VERBOSE = true;
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) SHM_NAME = argv[++i];
        if (strcmp(argv[i], "--guest-call") == 0) GUEST_CALL_MODE = true;
        if (strcmp(argv[i], "--stream") == 0) STREAM_MODE = true;
        if (strcmp(argv[i], "--legacy-claim") == 0) LEGACY_CLAIM = true;
    }

    // Mode-default for IN_FLIGHT (0 sentinel = unset). Default 1 in both
    // modes — measured, not assumed: a 2026-07-04 A/B on the Ryzen 9 3900X
    // (direct-into-destination reassembler, 4KiB chunks) found depth 2 LOSES
    // to depth 1 on every stream-profile cell, including the 16MiB cell where
    // depth 2 had previously looked good against the old double-copy
    // reassembler (1T/16MiB: depth1 186 ops/s vs depth2 139; 64KiB cells
    // -18..-36%; depth 2 also doubles numHostSlots, pushing 8T past the
    // 12-SMT-pair Sibling-affinity fit). NOTE this contradicts the shipping
    // library defaults (C++ StreamSender inFlight=2, Go stream_sender
    // maxInFlight=2) — recorded as a library-default review item in the
    // backlog rather than hidden with a bench-only gate. Override with -i <n>.
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
        auto pairs = shm::Platform::EnumerateSmtPairs();
        const char* modeLabel = (AFFINITY_MODE == 0) ? "auto" :
                                (AFFINITY_MODE == 1) ? "none" :
                                (AFFINITY_MODE == 2) ? "local" : "sibling";
        std::cout << "  Affinity: " << modeLabel
                  << " (CCXs detected: " << masks.size()
                  << ", SMT pairs: " << pairs.size() << ")" << std::endl;
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

    // Start the library's event-driven guest-call worker if needed
    if (GUEST_CALL_MODE) {
        host.Start(GuestCallEcho);
    }

    // Start Workers
    perThreadStats.assign(NUM_THREADS, BenchmarkStats{});
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
    if (GUEST_CALL_MODE) host.Stop();

    // Send Shutdown to Guest
    std::cout << "Sending Shutdown..." << std::endl;
    host.SendShutdown();

    // Print Results
    uint64_t totalOps = 0, totalErr = 0, totalLatNs = 0, totalSamples = 0;
    for (const auto& st : perThreadStats) {
        totalOps += st.ops;
        totalErr += st.errors;
        totalLatNs += st.latencySumNs;
        totalSamples += st.samples;
    }
    double avgLat = totalSamples > 0 ? (double)totalLatNs / totalSamples / 1000.0 : 0.0;
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
