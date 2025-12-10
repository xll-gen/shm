#include <iostream>
#include <shm/DirectHost.h>
#include <shm/Platform.h>

using namespace shm;

int main() {
    DirectHost host;
    std::string name = "ProtoVerTest";
    if (!host.Init(name, 1, 1024)) return 1;

    ShmHandle hMap;
    bool exists;
    void* ptr = Platform::CreateNamedShm(name.c_str(), 4096, hMap, exists);
    if (!ptr) return 1;

    ExchangeHeader* ex = (ExchangeHeader*)ptr;
    bool pass = true;
    if (ex->magic != SHM_MAGIC) pass = false;
    if (ex->version != SHM_VERSION) pass = false;

    Platform::CloseShm(hMap, ptr, 4096);
    host.Shutdown();
    return pass ? 0 : 1;
}
