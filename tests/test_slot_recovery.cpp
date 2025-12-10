#include <iostream>
#include <thread>
#include <chrono>
#include <shm/DirectHost.h>
#include <shm/Platform.h>

using namespace shm;

int main() {
    DirectHost host;
    std::string name = "SlotRecoveryTest";
    if (!host.Init(name, 1, 1024)) return 1;

    std::thread guest([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        ShmHandle hMap;
        bool exists;
        void* ptr = Platform::CreateNamedShm(name.c_str(), 1024*1024, hMap, exists);
        if (!ptr) return;

        ExchangeHeader* ex = (ExchangeHeader*)ptr;
        SlotHeader* slot = (SlotHeader*)((uint8_t*)ptr + sizeof(ExchangeHeader));

        std::string reqName = name + "_slot_0";
        std::string respName = name + "_slot_0_resp";
        EventHandle hReq = Platform::CreateNamedEvent(reqName.c_str());
        EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());

        int sanity = 0;
        while(slot->state.load(std::memory_order_acquire) != SLOT_REQ_READY) {
            std::this_thread::yield();
            if (++sanity > 1000000) break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        slot->state.store(SLOT_RESP_READY, std::memory_order_release);
        Platform::SignalEvent(hResp);

        sanity = 0;
        while(slot->state.load(std::memory_order_acquire) != SLOT_REQ_READY) {
            std::this_thread::yield();
            if (++sanity > 1000000) break;
        }

        slot->state.store(SLOT_RESP_READY, std::memory_order_release);
        Platform::SignalEvent(hResp);

        Platform::CloseEvent(hReq);
        Platform::CloseEvent(hResp);
        Platform::CloseShm(hMap, ptr, 1024*1024);
    });

    std::vector<uint8_t> resp;
    int res = host.SendToSlot(0, nullptr, 0, MsgType::NORMAL, resp, 50);

    if (res != -1) {
        std::cerr << "Expected timeout, got success" << std::endl;
        guest.join();
        return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    res = host.SendToSlot(0, nullptr, 0, MsgType::NORMAL, resp, 1000);

    guest.join();

    if (res == -1) {
        std::cerr << "Call 2 failed (Recovery failed)" << std::endl;
        return 1;
    }

    return 0;
}
