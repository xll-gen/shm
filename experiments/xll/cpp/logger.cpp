#include "logger.h"
#include <iostream>
#include <sstream>

Logger::Logger() {
    // Default log file or initial setup
    SetLogFile("xll_zero_copy.log");
}

Logger::~Logger() {
    if (logFile.is_open()) {
        logFile.close();
    }
}

Logger& Logger::GetInstance() {
    static Logger instance;
    return instance;
}

void Logger::SetLogFile(const std::string& filename) {
    std::lock_guard<std::mutex> lock(logMutex);
    if (logFile.is_open()) {
        logFile.close();
    }
    logFilename = filename;
    logFile.open(logFilename, std::ios_base::app);
    if (!logFile.is_open()) {
        // Fallback to console if file can't be opened
        std::cerr << "Warning: Could not open log file: " << logFilename << std::endl;
    }
}

void Logger::Log(const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex);
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::localtime(&in_time_t), "[%Y-%m-%d %H:%M:%S]") << " " << message << std::endl;

    if (logFile.is_open()) {
        logFile << ss.str();
        logFile.flush(); // Ensure message is written immediately
    } else {
        std::cerr << "LOG ERROR: Log file not open. Message: " << ss.str();
    }
}
