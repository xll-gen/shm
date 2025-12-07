#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <cstring>
#include <chrono>
#include <shm/DirectHost.h>
#include <shm/Platform.h>

using namespace shm;

int main() {
    DirectHost host;
    std::string name = "MsgSeqTest";
    int numSlots = 4;

    if (!host.Init(name, numSlots, 1024)) {
        std::cerr << "Failed to init host" << std::endl;
        return 1;
    }

    // Manually map SHM to act as Guest
    ShmHandle hMap;
    bool exists;
    // We assume layout matches standard
    size_t size = 64 + (128 + 1024) * (size_t)numSlots;
    // Actually we should read size from DirectHost but we can't.
    // We can map larger to be safe or rely on CreateNamedShm opening existing size
    void* ptr = Platform::CreateNamedShm(name.c_str(), 1024 * 1024, hMap, exists);

    if (!ptr) {
        std::cerr << "Failed to map SHM" << std::endl;
        return 1;
    }

    ExchangeHeader* ex = (ExchangeHeader*)ptr;
    // Re-verify ex header
    size_t slotHeaderSize = 128;
    size_t slotSize = ex->slotSize;
    size_t perSlot = slotHeaderSize + slotSize;
    uint8_t* basePtr = (uint8_t*)ptr + sizeof(ExchangeHeader);

    // Test Case 1: Check Initial Values and Stride
    // Host Logic: Init sets seq = i + 1. Stride = numSlots (4).

    bool allPass = true;

    // We will simulate responses for all slots
    std::vector<std::thread> guests;
    std::vector<uint32_t> capturedSeqs;
    std::mutex captureMutex;

    for(int i=0; i<numSlots; ++i) {
        guests.emplace_back([&, i](){
            SlotHeader* h = (SlotHeader*)(basePtr + (i * perSlot));
            std::string respName = name + "_slot_" + std::to_string(i) + "_resp";
            EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());

            // Loop twice per slot to verify stride
            for(int k=0; k<2; ++k) {
                // Wait for REQ
                int t = 0;
                while(h->state.load(std::memory_order_acquire) != SLOT_REQ_READY) {
                    Platform::CpuRelax();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    t++;
                    if(t > 2000) break;
                }

                if (h->state.load() == SLOT_REQ_READY) {
                    uint32_t seq = h->msgSeq;
                    {
                        std::lock_guard<std::mutex> lock(captureMutex);
                        capturedSeqs.push_back(seq);
                    }
                    // Respond
                    h->state.store(SLOT_RESP_READY, std::memory_order_release);
                    Platform::SignalEvent(hResp);

                     // Wait for host to clear (FREE or BUSY)
                    t = 0;
                    while(h->state.load() == SLOT_RESP_READY) {
                        Platform::CpuRelax();
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        t++;
                        if (t > 500) break;
                    }
                }
            }
            Platform::CloseEvent(hResp);
        });
    }

    // Host Actions
    // We send to each slot sequentially twice
    std::vector<uint8_t> resp;

    std::cout << "Sending Cycle 1..." << std::endl;
    for(int i=0; i<numSlots; ++i) {
        host.SendToSlot(i, nullptr, 0, MSG_TYPE_NORMAL, resp);
    }

    std::cout << "Sending Cycle 2..." << std::endl;
    for(int i=0; i<numSlots; ++i) {
        host.SendToSlot(i, nullptr, 0, MSG_TYPE_NORMAL, resp);
    }

    for(auto& t : guests) t.join();

    // Verification
    // Expected:
    // Cycle 1: 1, 2, 3, 4
    // Cycle 2: 5, 6, 7, 8 (Stride = 4)
    // Note: Since we captured in a list protected by mutex, the order depends on thread timing if concurrent.
    // But we sent sequentially. So we might expect order.
    // But the guests ran in parallel threads.
    // However, host.SendToSlot blocks. So Host actions are serialized.
    // So the capture order should effectively be sequential.

    std::vector<uint32_t> expected = {1, 2, 3, 4, 5, 6, 7, 8};

    if (capturedSeqs.size() != expected.size()) {
        std::cout << "FAIL: Count mismatch. Got " << capturedSeqs.size() << " Expected " << expected.size() << std::endl;
        allPass = false;
    } else {
        bool match = true;
        for(size_t i=0; i<expected.size(); ++i) {
            if(capturedSeqs[i] != expected[i]) {
                match = false;
                std::cout << "FAIL: Mismatch at " << i << " Got " << capturedSeqs[i] << " Expected " << expected[i] << std::endl;
            }
        }
        if (match) std::cout << "PASS: All sequences correct." << std::endl;
        else allPass = false;
    }

    Platform::CloseShm(hMap, ptr, 1024*1024); // Size approximate
    return allPass ? 0 : 1;
}
