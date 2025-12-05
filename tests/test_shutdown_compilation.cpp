#include <shm/DirectHost.h>
#include <iostream>

int main() {
    shm::DirectHost host;
    // This call ensures that the method is resolvable and no ambiguous overload exists.
    host.SendShutdown();
    std::cout << "SendShutdown compiled and linked successfully." << std::endl;
    return 0;
}
