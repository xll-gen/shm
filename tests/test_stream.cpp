#include <iostream>
#include <vector>
#include <thread>
#include <cstring>
#include <atomic>
#include <shm/DirectHost.h>
#include <shm/Stream.h>
#include <shm/IPCUtils.h>

using namespace shm;

int main() {
    DirectHost host;
    HostConfig config;
    config.shmName = "TestStream";
    config.numHostSlots = 4;
    config.payloadSize = 512; // Small slots to force many chunks

    auto initRes = host.Init(config);
    if (initRes.HasError()) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    // Simulate Guest in a background thread
    std::atomic<bool> guestRunning{true};
    std::vector<uint8_t> receivedData;
    std::atomic<int> receivedChunks{0};
    uint64_t expectedStreamID = 12345;

    std::thread guest([&]() {
        // 1. Map Header
        ShmHandle hMap;
        bool exists;
        void* addr = Platform::CreateNamedShm(config.shmName.c_str(), 64, hMap, exists);
        if (!addr) return;

        ExchangeHeader* ex = (ExchangeHeader*)addr;
        uint32_t slotSize = ex->slotSize;
        uint32_t numSlots = ex->numSlots;
        uint32_t reqOffset = ex->reqOffset;
        uint32_t respOffset = ex->respOffset;

        size_t totalSize = sizeof(ExchangeHeader) + numSlots * (sizeof(SlotHeader) + slotSize);
        Platform::CloseShm(hMap, addr, 64);

        // 2. Map Full
        addr = Platform::CreateNamedShm(config.shmName.c_str(), totalSize, hMap, exists);
        if (!addr) return;
        ex = (ExchangeHeader*)addr; // Re-pointer

        auto worker = [&](int i) {
            std::string reqName = config.shmName + "_slot_" + std::to_string(i);
            std::string respName = config.shmName + "_slot_" + std::to_string(i) + "_resp";
            EventHandle hReq = Platform::CreateNamedEvent(reqName.c_str());
            EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());

            size_t offset = sizeof(ExchangeHeader) + i * (sizeof(SlotHeader) + slotSize);
            uint8_t* slotBase = (uint8_t*)addr + offset;
            SlotHeader* header = (SlotHeader*)slotBase;
            uint8_t* reqBuf = slotBase + sizeof(SlotHeader) + reqOffset;

            while(guestRunning) {
                while (header->state.load(std::memory_order_acquire) != SLOT_REQ_READY) {
                    if (!guestRunning) return;
                    std::this_thread::sleep_for(std::chrono::microseconds(10));
                }

                MsgType type = header->msgType;
                if (type == MsgType::STREAM_START) {
                     StreamHeader* sh = (StreamHeader*)reqBuf;
                     if (sh->streamId == expectedStreamID) {
                         receivedData.resize(sh->totalSize);
                     }
                } else if (type == MsgType::STREAM_CHUNK) {
                     ChunkHeader* ch = (ChunkHeader*)reqBuf;
                     if (ch->streamId == expectedStreamID) {
                         uint8_t* payload = reqBuf + sizeof(ChunkHeader);

                         uint32_t maxReq = respOffset - reqOffset;
                         uint32_t maxPayload = maxReq - sizeof(ChunkHeader);

                         size_t offset = ch->chunkIndex * maxPayload;
                         if (offset + ch->payloadSize <= receivedData.size()) {
                             memcpy(receivedData.data() + offset, payload, ch->payloadSize);
                         }
                         receivedChunks++;
                     }
                }

                header->respSize = 0;
                header->msgType = MsgType::NORMAL;
                header->state.store(SLOT_RESP_READY, std::memory_order_release);
                Platform::SignalEvent(hResp);
            }
            Platform::CloseEvent(hReq);
            Platform::CloseEvent(hResp);
        };

        std::vector<std::thread> threads;
        for(int i=0; i<4; ++i) threads.emplace_back(worker, i);

        for(auto& t : threads) t.join();

        Platform::CloseShm(hMap, addr, totalSize);
    });

    // Prepare data
    size_t dataSize = 1024 * 1024; // 1MB
    std::vector<uint8_t> data(dataSize);
    for(size_t i=0; i<dataSize; ++i) data[i] = (uint8_t)(i % 256);

    StreamSender sender(&host);
    auto res = sender.Send(data.data(), dataSize, expectedStreamID);

    guestRunning = false;
    guest.join();

    host.Shutdown();

    if (res.HasError()) {
        std::cerr << "Send failed: " << (int)res.GetError() << std::endl;
        return 1;
    }

    if (receivedChunks == 0) {
         std::cerr << "No chunks received" << std::endl;
         return 1;
    }

    if (memcmp(data.data(), receivedData.data(), dataSize) != 0) {
        std::cerr << "Data mismatch" << std::endl;
        // Verify where
        for(size_t i=0; i<dataSize; ++i) {
            if (data[i] != receivedData[i]) {
                std::cerr << "Mismatch at " << i << " expected " << (int)data[i] << " got " << (int)receivedData[i] << std::endl;
                break;
            }
        }
        return 1;
    }

    std::cout << "Success! Sent " << dataSize << " bytes." << std::endl;
    return 0;
}
