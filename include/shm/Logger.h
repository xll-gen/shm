#pragma once

#ifdef SHM_DEBUG
#include <iostream>
#include <string>
#include <functional>
#include <sstream>

namespace shm {

enum class LogLevel {
    TRACE,
    DEBUG,
    INFO,
    WARN,
    ERROR
};

// Default log handler: [SHM][LEVEL] Message
inline std::function<void(LogLevel, const std::string&)> g_LogHandler = [](LogLevel level, const std::string& msg) {
    std::cerr << "[SHM]";
    switch (level) {
        case LogLevel::TRACE: std::cerr << "[TRACE] "; break;
        case LogLevel::DEBUG: std::cerr << "[DEBUG] "; break;
        case LogLevel::INFO:  std::cerr << "[INFO] ";  break;
        case LogLevel::WARN:  std::cerr << "[WARN] ";  break;
        case LogLevel::ERROR: std::cerr << "[ERROR] "; break;
    }
    std::cerr << msg << std::endl;
};

inline void SetLogHandler(std::function<void(LogLevel, const std::string&)> handler) {
    g_LogHandler = handler;
}

namespace detail {
    inline void Log(LogLevel level, const std::string& msg) {
        if (g_LogHandler) {
            g_LogHandler(level, msg);
        }
    }

    template<typename... Args>
    void LogStream(std::stringstream& ss, Args&&... args) {
        (ss << ... << std::forward<Args>(args));
    }
}

} // namespace shm

#define SHM_LOG_INTERNAL(level, ...) \
    do { \
        std::stringstream ss; \
        shm::detail::LogStream(ss, __VA_ARGS__); \
        shm::detail::Log(level, ss.str()); \
    } while(0)

#define SHM_LOG_TRACE(...) SHM_LOG_INTERNAL(shm::LogLevel::TRACE, __VA_ARGS__)
#define SHM_LOG_DEBUG(...) SHM_LOG_INTERNAL(shm::LogLevel::DEBUG, __VA_ARGS__)
#define SHM_LOG_INFO(...)  SHM_LOG_INTERNAL(shm::LogLevel::INFO,  __VA_ARGS__)
#define SHM_LOG_WARN(...)  SHM_LOG_INTERNAL(shm::LogLevel::WARN,  __VA_ARGS__)
#define SHM_LOG_ERROR(...) SHM_LOG_INTERNAL(shm::LogLevel::ERROR, __VA_ARGS__)

#else

// No-op macros
#define SHM_LOG_TRACE(...) do {} while(0)
#define SHM_LOG_DEBUG(...) do {} while(0)
#define SHM_LOG_INFO(...)  do {} while(0)
#define SHM_LOG_WARN(...)  do {} while(0)
#define SHM_LOG_ERROR(...) do {} while(0)

#endif
