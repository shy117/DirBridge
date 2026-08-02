#include "terminal/GhosttyTerminalEngine.h"

#include <QCoreApplication>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace dirbridge::terminal;

namespace {

std::string screenText(const TerminalSnapshotPtr &snapshot)
{
    std::string result;
    for (const auto &row : snapshot->rows)
    {
        for (const auto &cell : row)
        {
            result += cell.text;
        }
        result.push_back('\n');
    }
    return result;
}

bool sameScreen(const TerminalSnapshotPtr &left,
                const TerminalSnapshotPtr &right)
{
    if (!left || !right || left->rows.size() != right->rows.size()
        || left->cursor.column != right->cursor.column
        || left->cursor.row != right->cursor.row
        || left->alternateScreen != right->alternateScreen)
    {
        return false;
    }
    for (std::size_t row = 0; row < left->rows.size(); ++row)
    {
        if (left->rows[row].size() != right->rows[row].size())
        {
            return false;
        }
        for (std::size_t column = 0; column < left->rows[row].size(); ++column)
        {
            const auto &a = left->rows[row][column];
            const auto &b = right->rows[row][column];
            if (a.text != b.text || a.bold != b.bold
                || a.foreground.red != b.foreground.red
                || a.foreground.green != b.foreground.green
                || a.foreground.blue != b.foreground.blue)
            {
                return false;
            }
        }
    }
    return true;
}

bool ingest(ITerminalEngine &engine, const std::string &bytes)
{
    return engine.ingest(
        reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size());
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2)
    {
        return 64;
    }

    TerminalGeometry geometry;
    geometry.columns = 24;
    geometry.rows = 6;
    std::string error;
    auto engine = GhosttyTerminalEngine::create(
        std::filesystem::absolute(argv[1]), geometry, error);
    if (!engine)
    {
        std::cerr << error << '\n';
        return 1;
    }

    const std::string stream =
        "plain \x1b[31mred\x1b[0m\r\n"
        "\xe4\xb8\xad\xe6\x96\x87 e\xcc\x81\x1b[2;12Hcursor";
    if (!ingest(*engine, stream))
    {
        return 2;
    }
    const auto snapshot = engine->snapshot();
    const std::string text = screenText(snapshot);
    if (!snapshot || text.find("plain red") == std::string::npos
        || text.find("\xe4\xb8\xad\xe6\x96\x87") == std::string::npos
        || snapshot->cursor.row != 1 || snapshot->cursor.column != 17)
    {
        return 3;
    }
    const auto &redCell = snapshot->rows[0][6];
    if (!redCell.foreground.valid || redCell.foreground.red < 100
        || redCell.foreground.red <= redCell.foreground.green)
    {
        return 4;
    }

    if (!ingest(*engine, "\x1b[?1049halt\x1b[?2004h")
        || !engine->snapshot()->alternateScreen)
    {
        return 5;
    }
    const auto bracketed = engine->encodePaste("one\ntwo");
    const std::string bracketedText(bracketed.begin(), bracketed.end());
    if (bracketedText.find("\x1b[200~") != 0
        || bracketedText.rfind("\x1b[201~") == std::string::npos)
    {
        return 6;
    }
    if (!ingest(*engine, "\x1b[?2004l\x1b[?1049l")
        || engine->snapshot()->alternateScreen
        || engine->encodePaste("safe")
            != std::vector<std::uint8_t>({'s', 'a', 'f', 'e'}))
    {
        return 7;
    }

    TerminalKeyEvent enter;
    enter.key = TerminalKey::Enter;
    if (engine->encodeKey(enter) != std::vector<std::uint8_t>({'\r'}))
    {
        return 8;
    }
    TerminalKeyEvent interrupt;
    interrupt.key = TerminalKey::C;
    interrupt.text = "c";
    interrupt.control = true;
    if (engine->encodeKey(interrupt) != std::vector<std::uint8_t>({3}))
    {
        return 9;
    }

    auto whole = GhosttyTerminalEngine::create(
        std::filesystem::absolute(argv[1]), geometry, error);
    auto chunks = GhosttyTerminalEngine::create(
        std::filesystem::absolute(argv[1]), geometry, error);
    if (!whole || !chunks || !ingest(*whole, stream))
    {
        return 10;
    }
    for (const unsigned char byte : stream)
    {
        if (!chunks->ingest(&byte, 1))
        {
            return 10;
        }
    }
    if (!sameScreen(whole->snapshot(), chunks->snapshot()))
    {
        return 11;
    }

    TerminalGeometry resized = geometry;
    resized.columns = 12;
    resized.rows = 10;
    if (!whole->resize(resized)
        || whole->snapshot()->geometry.columns != 12)
    {
        return 12;
    }

    std::cout << "[PASS] Ghostty terminal engine VT and input checks\n";
    return 0;
}
