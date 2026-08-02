#include "shm/DirectHost.h"
#include "shm/Stream.h"
#include "shm/StreamReassembler.h"
#include <iostream>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <vector>
#include <cstdlib>

using namespace shm;

// Supplied by tests/CMakeLists.txt at configure time. Absolute, because ctest
// runs this binary from the BUILD tree while the Go sources live in the SOURCE
// tree; the previous source-relative paths could never resolve.
#ifndef SHM_GO_EXECUTABLE
#define SHM_GO_EXECUTABLE ""
#endif
#ifndef SHM_GO_INTEGRATION_DIR
#define SHM_GO_INTEGRATION_DIR ""
#endif
#ifndef SHM_GO_INTEGRATION_BIN
#define SHM_GO_INTEGRATION_BIN ""
#endif

// CTest SKIP_RETURN_CODE (see tests/CMakeLists.txt). A missing Go toolchain is
// an environment fact, not a repository defect, and must not look like the
// protocol regression this test exists to catch.
static const int kSkipExitCode = 77;

int main() {
    const std::string goExe(SHM_GO_EXECUTABLE);
    const std::string goDir(SHM_GO_INTEGRATION_DIR);
    if (goExe.empty() || goDir.empty()) {
        std::cout << "SKIP: no Go toolchain was found at configure time "
                     "(re-run cmake with Go on PATH to enable this test)." << std::endl;
        return kSkipExitCode;
    }

    const std::string SHM_NAME = "StreamIntegration";
    const int NUM_SLOTS = 2;
    const int NUM_GUEST_SLOTS = 2;
    const int PAYLOAD_SIZE = 1024 * 1024; // 1MB

    HostConfig config;
    config.shmName = SHM_NAME;
    config.numHostSlots = NUM_SLOTS;
    config.numGuestSlots = NUM_GUEST_SLOTS;
    config.payloadSize = PAYLOAD_SIZE;

    DirectHost host;
    if (!host.Init(config)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }
    // Cleanup on exit
    // Note: In C++, destructors run, but we want to ensure SHM is removed if we crash?
    // Usually host.Shutdown() handles it.

    std::mutex mtx;
    std::condition_variable cv;
    bool streamReceived = false;
    std::vector<uint8_t> receivedData;

    StreamReassembler reassembler([&](uint64_t streamId, const std::vector<uint8_t>& data) {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "Host: Received stream " << streamId << ", size " << data.size() << std::endl;
        receivedData = data;
        streamReceived = true;
        cv.notify_one();
    });

    host.Start([&](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxResp, MsgType type) -> int32_t {
        (void)maxResp;
        size_t respSize = 0;
        MsgType mutableType = type;
        if (reassembler.Handle(req, (size_t)reqSize, resp, respSize, mutableType)) {
            return (int32_t)respSize;
        }
        return 0;
    });

    // Spawn Go process. Absolute paths + quoting: the build dir is not the
    // source dir, and both can contain spaces (e.g. under Program Files).
    const std::string goBin(SHM_GO_INTEGRATION_BIN);
    const std::string buildCmd =
        "\"\"" + goExe + "\" build -C \"" + goDir + "\" -o \"" + goBin + "\"\"";
    int buildRet = system(buildCmd.c_str());
    if (buildRet != 0) {
        std::cerr << "Failed to build Go test (rc=" << buildRet << "): " << buildCmd << std::endl;
        return 1;
    }

    std::cout << "Host: Launching Go Guest..." << std::endl;
    // Run in background
    std::thread goProc([goBin]() {
        std::string runCmd = "\"\"" + goBin + "\" -name StreamIntegration\"";
        system(runCmd.c_str());
    });
    goProc.detach();

    // Give Go time to connect
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Send Stream to Guest
    std::cout << "Host: Sending Stream..." << std::endl;
    StreamSender sender(&host, 2);
    std::vector<uint8_t> payload(1024 * 1024, 0xAA);
    auto res = sender.Send(payload.data(), payload.size(), 123);
    if (res.HasError()) {
        std::cerr << "Host: Send failed: " << (int)res.GetError() << std::endl;
        return 1;
    }
    std::cout << "Host: Stream Sent." << std::endl;

    // Wait for Guest Stream
    std::cout << "Host: Waiting for Guest Stream..." << std::endl;
    std::unique_lock<std::mutex> lk(mtx);
    if (cv.wait_for(lk, std::chrono::seconds(20), [&]{ return streamReceived; })) {
        std::cout << "Host: Stream Received!" << std::endl;
        // Verify 0xBB
        if (receivedData.size() != 512 * 1024) {
            std::cerr << "Host: Wrong size: " << receivedData.size() << std::endl;
            return 1;
        }
        for (auto b : receivedData) {
            if (b != 0xBB) {
                std::cerr << "Host: Byte mismatch!" << std::endl;
                return 1;
            }
        }
    } else {
        std::cerr << "Host: Timeout waiting for stream" << std::endl;
        return 1;
    }

    host.SendShutdown();
    host.Shutdown();

    std::cout << "Host: Test Passed." << std::endl;
    return 0;
}
