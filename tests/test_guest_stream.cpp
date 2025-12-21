#include <shm/DirectHost.h>
#include <shm/StreamReassembler.h>
#include <shm/IPCUtils.h>
#include <shm/Platform.h>
#include <vector>
#include <thread>
#include <cassert>
#include <iostream>
#include <cstring>
#include <chrono>

using namespace shm;

int main() {
    const char* shmName = "TestGuestStreamCPP";
    Platform::UnlinkShm(shmName);

    // 1. Initialize Host
    HostConfig config;
    config.shmName = shmName;
    config.numHostSlots = 1;
    config.numGuestSlots = 2;
    config.payloadSize = 1024;

    DirectHost host;
    if (!host.Init(config)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    // 2. Setup Reassembler
    bool streamReceived = false;
    uint64_t receivedStreamId = 0;
    std::vector<uint8_t> receivedData;

    StreamReassembler reassembler([&](uint64_t id, const std::vector<uint8_t>& data) {
        streamReceived = true;
        receivedStreamId = id;
        receivedData = data;
    });

    // 3. Start Host Thread
    host.Start([&](const uint8_t* req, int32_t reqSize, uint8_t* resp, uint32_t maxRespSize, MsgType msgType) -> int32_t {
        size_t respSize = 0;
        MsgType mutableType = msgType;
        if (reassembler.Handle(req, (size_t)reqSize, resp, respSize, mutableType)) {
            return (int32_t)respSize;
        }
        return 0;
    });

    // 4. Mock Guest
    uint64_t totalSize = 0;
    void* addr = nullptr;
    ShmHandle h = Platform::OpenShm(shmName, 64, totalSize, addr);
    if (!addr) {
         std::cerr << "Guest failed to map header" << std::endl;
         return 1;
    }
    ExchangeHeader* ex = (ExchangeHeader*)addr;
    uint32_t slotSize = ex->slotSize;
    uint32_t reqOffset = ex->reqOffset;
    uint32_t numSlots = ex->numSlots;
    uint32_t numGuestSlots = ex->numGuestSlots;
    Platform::CloseShm(h, addr, 64);

    uint64_t fullSize = 64 + (uint64_t)(numSlots + numGuestSlots) * (128 + slotSize);
    h = Platform::OpenShm(shmName, fullSize, totalSize, addr);
    if (!addr) {
        std::cerr << "Guest failed to map full" << std::endl;
        return 1;
    }

    // Helper to get slot
    auto getSlot = [&](int idx) -> SlotHeader* {
        uint8_t* ptr = (uint8_t*)addr + 64 + idx * (128 + slotSize);
        return (SlotHeader*)ptr;
    };

    auto getReqBuf = [&](int idx) -> uint8_t* {
        return (uint8_t*)getSlot(idx) + 128 + reqOffset;
    };

    // Events
    EventHandle reqEv = Platform::OpenEvent("TestGuestStreamCPP_guest_call");
    Platform::OpenEvent("TestGuestStreamCPP_slot_1_resp"); // Open to ensure existence, though Host creates them
    Platform::OpenEvent("TestGuestStreamCPP_slot_2_resp");

    // Send Stream Start
    {
        int slotIdx = 1; // Guest Slot 0 is at Host Index 1
        SlotHeader* s = getSlot(slotIdx);
        uint8_t* buf = getReqBuf(slotIdx);

        StreamHeader hdr;
        hdr.streamId = 999;
        hdr.totalSize = 5;
        hdr.totalChunks = 2; // Split 5 bytes into 3 + 2
        hdr.reserved = 0;

        memcpy(buf, &hdr, sizeof(hdr));

        s->msgType = MsgType::STREAM_START;
        s->reqSize = sizeof(hdr);
        s->state.store(SLOT_REQ_READY);
        Platform::SignalEvent(reqEv);

        // Wait Response
        while (s->state.load() != SLOT_RESP_READY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        s->state.store(SLOT_FREE);
    }

    // Send Chunk 0
    {
        int slotIdx = 1;
        SlotHeader* s = getSlot(slotIdx);
        uint8_t* buf = getReqBuf(slotIdx);

        ChunkHeader hdr;
        hdr.streamId = 999;
        hdr.chunkIndex = 0;
        hdr.payloadSize = 3;
        hdr.reserved = 0;

        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), "ABC", 3);

        s->msgType = MsgType::STREAM_CHUNK;
        s->reqSize = sizeof(hdr) + 3;
        s->state.store(SLOT_REQ_READY);
        Platform::SignalEvent(reqEv);

         while (s->state.load() != SLOT_RESP_READY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        s->state.store(SLOT_FREE);
    }

    // Send Chunk 1
    {
        int slotIdx = 2; // Use other slot
        SlotHeader* s = getSlot(slotIdx);
        uint8_t* buf = getReqBuf(slotIdx);

        ChunkHeader hdr;
        hdr.streamId = 999;
        hdr.chunkIndex = 1;
        hdr.payloadSize = 2;
        hdr.reserved = 0;

        memcpy(buf, &hdr, sizeof(hdr));
        memcpy(buf + sizeof(hdr), "DE", 2);

        s->msgType = MsgType::STREAM_CHUNK;
        s->reqSize = sizeof(hdr) + 2;
        s->state.store(SLOT_REQ_READY);
        Platform::SignalEvent(reqEv);

         while (s->state.load() != SLOT_RESP_READY) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        s->state.store(SLOT_FREE);
    }

    // Verify
    int timeout = 100;
    while (!streamReceived && timeout-- > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!streamReceived) {
        std::cerr << "Stream not received" << std::endl;
        return 1;
    }

    if (receivedStreamId != 999) {
         std::cerr << "Wrong Stream ID" << std::endl;
         return 1;
    }

    if (receivedData.size() != 5) {
         std::cerr << "Wrong Size" << std::endl;
         return 1;
    }

    if (memcmp(receivedData.data(), "ABCDE", 5) != 0) {
         std::cerr << "Wrong Data" << std::endl;
         return 1;
    }

    // Clean up
    host.SendShutdown();
    host.Stop();

    Platform::CloseShm(h, addr, fullSize);

    std::cout << "Test Passed" << std::endl;
    return 0;
}
