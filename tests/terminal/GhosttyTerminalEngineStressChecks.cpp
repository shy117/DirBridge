#include "terminal/GhosttyTerminalEngine.h"

#include <QCoreApplication>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>

using namespace dirbridge::terminal;

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2)
    {
        return 64;
    }
    TerminalGeometry geometry;
    geometry.columns = 120;
    geometry.rows = 40;
    std::string error;
    auto engine = GhosttyTerminalEngine::create(
        std::filesystem::absolute(argv[1]), geometry, error);
    if (!engine)
    {
        return 1;
    }

    std::string block;
    block.reserve(64 * 1024);
    while (block.size() + 80 < 64 * 1024)
    {
        block += "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n";
    }
    std::uint64_t total = 0;
    constexpr std::uint64_t target = 64ULL * 1024ULL * 1024ULL;
    while (total < target)
    {
        const auto remaining = static_cast<std::size_t>(
            std::min<std::uint64_t>(block.size(), target - total));
        if (!engine->ingest(
                reinterpret_cast<const std::uint8_t *>(block.data()),
                remaining))
        {
            return 2;
        }
        total += remaining;
    }
    const std::string marker = "\r\nDIRBRIDGE_64MIB_OK";
    if (!engine->ingest(
            reinterpret_cast<const std::uint8_t *>(marker.data()),
            marker.size()))
    {
        return 3;
    }
    const auto snapshot = engine->snapshot();
    if (!snapshot)
    {
        return 4;
    }
    std::string text;
    for (const auto &row : snapshot->rows)
    {
        for (const auto &cell : row)
        {
            text += cell.text;
        }
    }
    if (text.find("DIRBRIDGE_64MIB_OK") == std::string::npos
        || snapshot->scrollLength > 5040)
    {
        return 4;
    }
    std::cout << "[PASS] 64 MiB VT stream with bounded scrollback\n";
    return 0;
}
