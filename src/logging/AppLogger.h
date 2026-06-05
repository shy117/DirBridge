#ifndef DIRBRIDGE_LOGGING_APPLOGGER_H
#define DIRBRIDGE_LOGGING_APPLOGGER_H

#include <filesystem>
#include <memory>
#include <string>

namespace spdlog
{
class logger;
}

class AppLogger
{
public:
    static std::shared_ptr<spdlog::logger> initialize(const std::filesystem::path &logDirectory);
    static std::shared_ptr<spdlog::logger> get();
    static std::filesystem::path logFilePath();
    static void shutdown();
};

#endif // DIRBRIDGE_LOGGING_APPLOGGER_H
