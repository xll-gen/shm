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

int main() {
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

    host.Start([&](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxResp, MsgType type) {
        return reassembler.Handle(req, reqSize, resp, maxResp, type);
    });

    // Spawn Go process
    // Build Go binary first
    int buildRet = system("cd tests/go_integration && go build -o integration_test");
    if (buildRet != 0) {
        std::cerr << "Failed to build Go test" << std::endl;
        return 1;
    }

    std::cout << "Host: Launching Go Guest..." << std::endl;
    // Run in background
    std::thread goProc([]() {
        system("./tests/go_integration/integration_test -name StreamIntegration");
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
