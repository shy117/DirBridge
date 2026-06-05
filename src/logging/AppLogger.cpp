#include "logging/AppLogger.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

namespace
{
std::shared_ptr<spdlog::logger> g_logger;
std::filesystem::path g_logFilePath;
}

std::shared_ptr<spdlog::logger> AppLogger::initialize(const std::filesystem::path &logDirectory)
{
    std::filesystem::create_directories(logDirectory);
    g_logFilePath = logDirectory / "dirbridge.log";
    g_logger = spdlog::basic_logger_mt("dirbridge", g_logFilePath.string(), true);
    g_logger->set_level(spdlog::level::info);
    g_logger->flush_on(spdlog::level::info);
    spdlog::set_default_logger(g_logger);
    g_logger->info("DirBridge logger initialized");
    return g_logger;
}

std::shared_ptr<spdlog::logger> AppLogger::get()
{
    if (g_logger)
    {
        return g_logger;
    }

    return initialize("logs");
}

std::filesystem::path AppLogger::logFilePath()
{
    return g_logFilePath;
}

void AppLogger::shutdown()
{
    if (g_logger)
    {
        g_logger->info("DirBridge logger shutdown");
        g_logger->flush();
    }
    spdlog::drop("dirbridge");
    g_logger.reset();
}
