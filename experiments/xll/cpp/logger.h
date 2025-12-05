#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <mutex>

class Logger {
public:
    static Logger& GetInstance();
    void Log(const std::string& message);
    void SetLogFile(const std::string& filename);

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::ofstream logFile;
    std::string logFilename;
    std::mutex logMutex;
};

#define LOG(message) Logger::GetInstance().Log(message)

#endif // LOGGER_H
