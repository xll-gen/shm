#include <shm/DirectHost.h>
#include <shm/Logger.h>
#include <iostream>
#include <vector>
#include <string>

int main() {
#ifdef SHM_DEBUG
    std::cout << "Debug Enabled" << std::endl;
    std::vector<std::string> logs;
    shm::SetLogHandler([&](shm::LogLevel level, const std::string& msg) {
        logs.push_back(msg);
        std::cout << "Captured: " << msg << std::endl;
    });

    shm::HostConfig config;
    config.shmName = "LoggingTest";
    config.numHostSlots = 1;

    shm::DirectHost host;
    host.Init(config);

    if (logs.empty()) {
        std::cerr << "No logs captured!" << std::endl;
        return 1;
    }

    bool found = false;
    for(const auto& log : logs) {
        if (log.find("Initializing DirectHost") != std::string::npos) {
            found = true;
            break;
        }
    }

    if (!found) {
        std::cerr << "Expected log message not found." << std::endl;
        return 1;
    }
    host.Shutdown();
#else
    std::cout << "Debug Disabled" << std::endl;
#endif
    return 0;
}
