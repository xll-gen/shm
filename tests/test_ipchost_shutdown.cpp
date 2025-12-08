#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <shm/IPCHost.h>
#include <shm/Platform.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace shm;

std::atomic<int> shutdownCount{0};

void GuestWorker(std::string shmName, int slotIdx) {
    // Open SHM manually
    std::string path = "/" + shmName;
    int fd = shm_open(path.c_str(), O_RDWR, 0666);
    if (fd < 0) {
        std::cerr << "Guest " << slotIdx << " failed to open SHM" << std::endl;
        return;
    }

    // Map enough. Default slot size is 1MB. 2 slots + header ~ 2MB. Map 4MB to be safe.
    size_t mapSize = 4 * 1024 * 1024;
    void* addr = mmap(NULL, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        std::cerr << "Guest " << slotIdx << " failed to mmap" << std::endl;
        close(fd);
        return;
    }

    // Get Slot
    ExchangeHeader* eh = (ExchangeHeader*)addr;
    uint8_t* ptr = (uint8_t*)addr + sizeof(ExchangeHeader);
    size_t slotTotal = sizeof(SlotHeader) + eh->slotSize;

    SlotHeader* header = (SlotHeader*)(ptr + slotIdx * slotTotal);
    std::string reqName = shmName + "_slot_" + std::to_string(slotIdx);
    std::string respName = shmName + "_slot_" + std::to_string(slotIdx) + "_resp";

    EventHandle hReq = Platform::CreateNamedEvent(reqName.c_str());
    EventHandle hResp = Platform::CreateNamedEvent(respName.c_str());

    if (!hReq || !hResp) {
         std::cerr << "Guest " << slotIdx << " failed to open events" << std::endl;
         munmap(addr, mapSize);
         close(fd);
         return;
    }

    // Loop
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
            std::cerr << "Guest " << slotIdx << " timed out waiting for shutdown" << std::endl;
            break; // Timeout
        }

        if (header->state.load(std::memory_order_acquire) == SLOT_REQ_READY) {
            if (header->msgType == MSG_TYPE_SHUTDOWN) {
                shutdownCount++;
                // Ack
                header->state.store(SLOT_RESP_READY, std::memory_order_release);
                Platform::SignalEvent(hResp);
                break;
            } else {
                // Dummy Ack for other messages
                 header->state.store(SLOT_RESP_READY, std::memory_order_release);
                 Platform::SignalEvent(hResp);
            }
        } else {
             Platform::CpuRelax();
             std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    Platform::CloseEvent(hReq);
    Platform::CloseEvent(hResp);
    munmap(addr, mapSize);
    close(fd);
}

int main() {
    std::string name = "ReproShut";
    // Cleanup first
    Platform::UnlinkShm(name.c_str());
    Platform::UnlinkNamedEvent((name + "_slot_0").c_str());
    Platform::UnlinkNamedEvent((name + "_slot_0_resp").c_str());
    Platform::UnlinkNamedEvent((name + "_slot_1").c_str());
    Platform::UnlinkNamedEvent((name + "_slot_1_resp").c_str());

    IPCHost host;
    // Init with 2 slots, Default 1MB
    if (!host.Init(name, 2)) {
        std::cerr << "Init failed" << std::endl;
        return 1;
    }

    // Start 2 Guests
    std::thread t1(GuestWorker, name, 0);
    std::thread t2(GuestWorker, name, 1);

    // Allow guests to start and enter loop
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "Sending Shutdown..." << std::endl;
    host.SendShutdown();
    std::cout << "Shutdown Sent." << std::endl;

    t1.join();
    t2.join();

    host.Shutdown();

    if (shutdownCount == 2) {
        std::cout << "SUCCESS: Both workers received shutdown." << std::endl;
        return 0;
    } else {
        std::cout << "FAILURE: Shutdown count " << shutdownCount << " (expected 2)" << std::endl;
        return 1;
    }
}
