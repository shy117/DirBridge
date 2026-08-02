#ifndef DIRBRIDGE_TERMINAL_TERMINALTYPES_H
#define DIRBRIDGE_TERMINAL_TERMINALTYPES_H

#include <QMetaType>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dirbridge::terminal {

struct TerminalColor
{
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    bool valid = false;
};

enum class TerminalCellWidth
{
    Narrow,
    Wide,
    SpacerTail,
    SpacerHead,
};

struct TerminalCell
{
    std::string text;
    TerminalCellWidth width = TerminalCellWidth::Narrow;
    TerminalColor foreground;
    TerminalColor background;
    bool bold = false;
    bool italic = false;
    bool faint = false;
    bool underline = false;
    bool inverse = false;
    bool invisible = false;
    bool strikethrough = false;
};

enum class TerminalCursorStyle
{
    Block,
    Bar,
    Underline,
    HollowBlock,
};

struct TerminalCursor
{
    std::uint16_t column = 0;
    std::uint16_t row = 0;
    TerminalCursorStyle style = TerminalCursorStyle::Block;
    TerminalColor color;
    bool visible = false;
    bool blinking = false;
};

struct TerminalGeometry
{
    std::uint16_t columns = 100;
    std::uint16_t rows = 30;
    std::uint32_t cellWidthPixels = 8;
    std::uint32_t cellHeightPixels = 16;
};

struct TerminalSnapshot
{
    TerminalGeometry geometry;
    std::vector<std::vector<TerminalCell>> rows;
    TerminalColor defaultForeground;
    TerminalColor defaultBackground;
    TerminalCursor cursor;
    std::uint64_t scrollTotal = 0;
    std::uint64_t scrollOffset = 0;
    std::uint64_t scrollLength = 0;
    bool alternateScreen = false;
    bool mouseTracking = false;
    std::uint64_t generation = 0;
};

using TerminalSnapshotPtr = std::shared_ptr<const TerminalSnapshot>;

enum class TerminalKey
{
    Unknown,
    Backspace,
    Tab,
    Enter,
    Escape,
    Insert,
    Delete,
    Home,
    End,
    PageUp,
    PageDown,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
};

struct TerminalKeyEvent
{
    TerminalKey key = TerminalKey::Unknown;
    std::string text;
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool autoRepeat = false;
};

enum class TerminalMouseAction
{
    Press,
    Release,
    Move,
};

enum class TerminalMouseButton
{
    None,
    Left,
    Middle,
    Right,
    WheelUp,
    WheelDown,
};

struct TerminalMouseEvent
{
    TerminalMouseAction action = TerminalMouseAction::Move;
    TerminalMouseButton button = TerminalMouseButton::None;
    float xPixels = 0.0F;
    float yPixels = 0.0F;
    bool shift = false;
    bool control = false;
    bool alt = false;
};

} // namespace dirbridge::terminal

Q_DECLARE_METATYPE(dirbridge::terminal::TerminalSnapshotPtr)
Q_DECLARE_METATYPE(dirbridge::terminal::TerminalKeyEvent)
Q_DECLARE_METATYPE(dirbridge::terminal::TerminalMouseEvent)

#endif // DIRBRIDGE_TERMINAL_TERMINALTYPES_H
