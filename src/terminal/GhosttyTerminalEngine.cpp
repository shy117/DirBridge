#include "terminal/GhosttyTerminalEngine.h"

#include <ghostty/vt.h>

#include <QFileInfo>
#include <QLibrary>
#include <QString>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace dirbridge::terminal {

namespace {

TerminalColor color(const GhosttyColorRgb &value)
{
    return TerminalColor{value.r, value.g, value.b, true};
}

GhosttyKey ghosttyKey(TerminalKey key)
{
    switch (key)
    {
    case TerminalKey::Backspace: return GHOSTTY_KEY_BACKSPACE;
    case TerminalKey::Tab: return GHOSTTY_KEY_TAB;
    case TerminalKey::Enter: return GHOSTTY_KEY_ENTER;
    case TerminalKey::Escape: return GHOSTTY_KEY_ESCAPE;
    case TerminalKey::Insert: return GHOSTTY_KEY_INSERT;
    case TerminalKey::Delete: return GHOSTTY_KEY_DELETE;
    case TerminalKey::Home: return GHOSTTY_KEY_HOME;
    case TerminalKey::End: return GHOSTTY_KEY_END;
    case TerminalKey::PageUp: return GHOSTTY_KEY_PAGE_UP;
    case TerminalKey::PageDown: return GHOSTTY_KEY_PAGE_DOWN;
    case TerminalKey::ArrowUp: return GHOSTTY_KEY_ARROW_UP;
    case TerminalKey::ArrowDown: return GHOSTTY_KEY_ARROW_DOWN;
    case TerminalKey::ArrowLeft: return GHOSTTY_KEY_ARROW_LEFT;
    case TerminalKey::ArrowRight: return GHOSTTY_KEY_ARROW_RIGHT;
    case TerminalKey::F1: return GHOSTTY_KEY_F1;
    case TerminalKey::F2: return GHOSTTY_KEY_F2;
    case TerminalKey::F3: return GHOSTTY_KEY_F3;
    case TerminalKey::F4: return GHOSTTY_KEY_F4;
    case TerminalKey::F5: return GHOSTTY_KEY_F5;
    case TerminalKey::F6: return GHOSTTY_KEY_F6;
    case TerminalKey::F7: return GHOSTTY_KEY_F7;
    case TerminalKey::F8: return GHOSTTY_KEY_F8;
    case TerminalKey::F9: return GHOSTTY_KEY_F9;
    case TerminalKey::F10: return GHOSTTY_KEY_F10;
    case TerminalKey::F11: return GHOSTTY_KEY_F11;
    case TerminalKey::F12: return GHOSTTY_KEY_F12;
    case TerminalKey::A: return GHOSTTY_KEY_A;
    case TerminalKey::B: return GHOSTTY_KEY_B;
    case TerminalKey::C: return GHOSTTY_KEY_C;
    case TerminalKey::D: return GHOSTTY_KEY_D;
    case TerminalKey::E: return GHOSTTY_KEY_E;
    case TerminalKey::F: return GHOSTTY_KEY_F;
    case TerminalKey::G: return GHOSTTY_KEY_G;
    case TerminalKey::H: return GHOSTTY_KEY_H;
    case TerminalKey::I: return GHOSTTY_KEY_I;
    case TerminalKey::J: return GHOSTTY_KEY_J;
    case TerminalKey::K: return GHOSTTY_KEY_K;
    case TerminalKey::L: return GHOSTTY_KEY_L;
    case TerminalKey::M: return GHOSTTY_KEY_M;
    case TerminalKey::N: return GHOSTTY_KEY_N;
    case TerminalKey::O: return GHOSTTY_KEY_O;
    case TerminalKey::P: return GHOSTTY_KEY_P;
    case TerminalKey::Q: return GHOSTTY_KEY_Q;
    case TerminalKey::R: return GHOSTTY_KEY_R;
    case TerminalKey::S: return GHOSTTY_KEY_S;
    case TerminalKey::T: return GHOSTTY_KEY_T;
    case TerminalKey::U: return GHOSTTY_KEY_U;
    case TerminalKey::V: return GHOSTTY_KEY_V;
    case TerminalKey::W: return GHOSTTY_KEY_W;
    case TerminalKey::X: return GHOSTTY_KEY_X;
    case TerminalKey::Y: return GHOSTTY_KEY_Y;
    case TerminalKey::Z: return GHOSTTY_KEY_Z;
    case TerminalKey::Unknown: break;
    }
    return GHOSTTY_KEY_UNIDENTIFIED;
}

TerminalCursorStyle cursorStyle(GhosttyRenderStateCursorVisualStyle style)
{
    switch (style)
    {
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:
        return TerminalCursorStyle::Bar;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:
        return TerminalCursorStyle::Underline;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW:
        return TerminalCursorStyle::HollowBlock;
    case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK:
    default:
        return TerminalCursorStyle::Block;
    }
}

} // namespace

struct GhosttyTerminalEngine::Impl
{
    using TerminalNew = decltype(&ghostty_terminal_new);
    using TerminalFree = decltype(&ghostty_terminal_free);
    using TerminalWrite = decltype(&ghostty_terminal_vt_write);
    using TerminalResize = decltype(&ghostty_terminal_resize);
    using TerminalSet = decltype(&ghostty_terminal_set);
    using TerminalGet = decltype(&ghostty_terminal_get);
    using TerminalScroll = decltype(&ghostty_terminal_scroll_viewport);
    using RenderNew = decltype(&ghostty_render_state_new);
    using RenderFree = decltype(&ghostty_render_state_free);
    using RenderUpdate = decltype(&ghostty_render_state_update);
    using RenderGet = decltype(&ghostty_render_state_get);
    using RenderColorsGet = decltype(&ghostty_render_state_colors_get);
    using RowIteratorNew = decltype(&ghostty_render_state_row_iterator_new);
    using RowIteratorFree = decltype(&ghostty_render_state_row_iterator_free);
    using RowIteratorNext = decltype(&ghostty_render_state_row_iterator_next);
    using RowGet = decltype(&ghostty_render_state_row_get);
    using RowCellsNew = decltype(&ghostty_render_state_row_cells_new);
    using RowCellsFree = decltype(&ghostty_render_state_row_cells_free);
    using RowCellsNext = decltype(&ghostty_render_state_row_cells_next);
    using RowCellsGet = decltype(&ghostty_render_state_row_cells_get);
    using CellGet = decltype(&ghostty_cell_get);
    using KeyEncoderNew = decltype(&ghostty_key_encoder_new);
    using KeyEncoderFree = decltype(&ghostty_key_encoder_free);
    using KeyEncoderFromTerminal = decltype(&ghostty_key_encoder_setopt_from_terminal);
    using KeyEncoderEncode = decltype(&ghostty_key_encoder_encode);
    using KeyEventNew = decltype(&ghostty_key_event_new);
    using KeyEventFree = decltype(&ghostty_key_event_free);
    using KeyEventSetAction = decltype(&ghostty_key_event_set_action);
    using KeyEventSetKey = decltype(&ghostty_key_event_set_key);
    using KeyEventSetMods = decltype(&ghostty_key_event_set_mods);
    using KeyEventSetUtf8 = decltype(&ghostty_key_event_set_utf8);
    using PasteEncode = decltype(&ghostty_paste_encode);
    using MouseEncoderNew = decltype(&ghostty_mouse_encoder_new);
    using MouseEncoderFree = decltype(&ghostty_mouse_encoder_free);
    using MouseEncoderSet = decltype(&ghostty_mouse_encoder_setopt);
    using MouseEncoderFromTerminal = decltype(&ghostty_mouse_encoder_setopt_from_terminal);
    using MouseEncoderEncode = decltype(&ghostty_mouse_encoder_encode);
    using MouseEventNew = decltype(&ghostty_mouse_event_new);
    using MouseEventFree = decltype(&ghostty_mouse_event_free);
    using MouseEventSetAction = decltype(&ghostty_mouse_event_set_action);
    using MouseEventSetButton = decltype(&ghostty_mouse_event_set_button);
    using MouseEventClearButton = decltype(&ghostty_mouse_event_clear_button);
    using MouseEventSetMods = decltype(&ghostty_mouse_event_set_mods);
    using MouseEventSetPosition = decltype(&ghostty_mouse_event_set_position);

    QLibrary library;
    TerminalNew terminalNew = nullptr;
    TerminalFree terminalFree = nullptr;
    TerminalWrite terminalWrite = nullptr;
    TerminalResize terminalResize = nullptr;
    TerminalSet terminalSet = nullptr;
    TerminalGet terminalGet = nullptr;
    TerminalScroll terminalScroll = nullptr;
    RenderNew renderNew = nullptr;
    RenderFree renderFree = nullptr;
    RenderUpdate renderUpdate = nullptr;
    RenderGet renderGet = nullptr;
    RenderColorsGet renderColorsGet = nullptr;
    RowIteratorNew rowIteratorNew = nullptr;
    RowIteratorFree rowIteratorFree = nullptr;
    RowIteratorNext rowIteratorNext = nullptr;
    RowGet rowGet = nullptr;
    RowCellsNew rowCellsNew = nullptr;
    RowCellsFree rowCellsFree = nullptr;
    RowCellsNext rowCellsNext = nullptr;
    RowCellsGet rowCellsGet = nullptr;
    CellGet cellGet = nullptr;
    KeyEncoderNew keyEncoderNew = nullptr;
    KeyEncoderFree keyEncoderFree = nullptr;
    KeyEncoderFromTerminal keyEncoderFromTerminal = nullptr;
    KeyEncoderEncode keyEncoderEncode = nullptr;
    KeyEventNew keyEventNew = nullptr;
    KeyEventFree keyEventFree = nullptr;
    KeyEventSetAction keyEventSetAction = nullptr;
    KeyEventSetKey keyEventSetKey = nullptr;
    KeyEventSetMods keyEventSetMods = nullptr;
    KeyEventSetUtf8 keyEventSetUtf8 = nullptr;
    PasteEncode pasteEncode = nullptr;
    MouseEncoderNew mouseEncoderNew = nullptr;
    MouseEncoderFree mouseEncoderFree = nullptr;
    MouseEncoderSet mouseEncoderSet = nullptr;
    MouseEncoderFromTerminal mouseEncoderFromTerminal = nullptr;
    MouseEncoderEncode mouseEncoderEncode = nullptr;
    MouseEventNew mouseEventNew = nullptr;
    MouseEventFree mouseEventFree = nullptr;
    MouseEventSetAction mouseEventSetAction = nullptr;
    MouseEventSetButton mouseEventSetButton = nullptr;
    MouseEventClearButton mouseEventClearButton = nullptr;
    MouseEventSetMods mouseEventSetMods = nullptr;
    MouseEventSetPosition mouseEventSetPosition = nullptr;

    GhosttyTerminal terminal = nullptr;
    GhosttyRenderState renderState = nullptr;
    GhosttyRenderStateRowIterator rowIterator = nullptr;
    GhosttyRenderStateRowCells rowCells = nullptr;
    GhosttyKeyEncoder keyEncoder = nullptr;
    GhosttyKeyEvent keyEvent = nullptr;
    GhosttyMouseEncoder mouseEncoder = nullptr;
    GhosttyMouseEvent mouseEvent = nullptr;
    TerminalGeometry geometry;
    std::string error;
    std::string modeTail;
    bool bracketedPaste = false;
    bool mouseButtonPressed = false;
    std::uint64_t generation = 0;

    template<typename Function>
    bool resolve(const char *name, Function &function)
    {
        function = reinterpret_cast<Function>(library.resolve(name));
        if (function != nullptr)
        {
            return true;
        }
        error = std::string("Ghostty VT DLL 缺少符号：") + name;
        return false;
    }

    bool resolveAll()
    {
#define DIRBRIDGE_RESOLVE(member, symbol) \
        if (!resolve(symbol, member)) return false
        DIRBRIDGE_RESOLVE(terminalNew, "ghostty_terminal_new");
        DIRBRIDGE_RESOLVE(terminalFree, "ghostty_terminal_free");
        DIRBRIDGE_RESOLVE(terminalWrite, "ghostty_terminal_vt_write");
        DIRBRIDGE_RESOLVE(terminalResize, "ghostty_terminal_resize");
        DIRBRIDGE_RESOLVE(terminalSet, "ghostty_terminal_set");
        DIRBRIDGE_RESOLVE(terminalGet, "ghostty_terminal_get");
        DIRBRIDGE_RESOLVE(terminalScroll, "ghostty_terminal_scroll_viewport");
        DIRBRIDGE_RESOLVE(renderNew, "ghostty_render_state_new");
        DIRBRIDGE_RESOLVE(renderFree, "ghostty_render_state_free");
        DIRBRIDGE_RESOLVE(renderUpdate, "ghostty_render_state_update");
        DIRBRIDGE_RESOLVE(renderGet, "ghostty_render_state_get");
        DIRBRIDGE_RESOLVE(renderColorsGet, "ghostty_render_state_colors_get");
        DIRBRIDGE_RESOLVE(rowIteratorNew, "ghostty_render_state_row_iterator_new");
        DIRBRIDGE_RESOLVE(rowIteratorFree, "ghostty_render_state_row_iterator_free");
        DIRBRIDGE_RESOLVE(rowIteratorNext, "ghostty_render_state_row_iterator_next");
        DIRBRIDGE_RESOLVE(rowGet, "ghostty_render_state_row_get");
        DIRBRIDGE_RESOLVE(rowCellsNew, "ghostty_render_state_row_cells_new");
        DIRBRIDGE_RESOLVE(rowCellsFree, "ghostty_render_state_row_cells_free");
        DIRBRIDGE_RESOLVE(rowCellsNext, "ghostty_render_state_row_cells_next");
        DIRBRIDGE_RESOLVE(rowCellsGet, "ghostty_render_state_row_cells_get");
        DIRBRIDGE_RESOLVE(cellGet, "ghostty_cell_get");
        DIRBRIDGE_RESOLVE(keyEncoderNew, "ghostty_key_encoder_new");
        DIRBRIDGE_RESOLVE(keyEncoderFree, "ghostty_key_encoder_free");
        DIRBRIDGE_RESOLVE(keyEncoderFromTerminal, "ghostty_key_encoder_setopt_from_terminal");
        DIRBRIDGE_RESOLVE(keyEncoderEncode, "ghostty_key_encoder_encode");
        DIRBRIDGE_RESOLVE(keyEventNew, "ghostty_key_event_new");
        DIRBRIDGE_RESOLVE(keyEventFree, "ghostty_key_event_free");
        DIRBRIDGE_RESOLVE(keyEventSetAction, "ghostty_key_event_set_action");
        DIRBRIDGE_RESOLVE(keyEventSetKey, "ghostty_key_event_set_key");
        DIRBRIDGE_RESOLVE(keyEventSetMods, "ghostty_key_event_set_mods");
        DIRBRIDGE_RESOLVE(keyEventSetUtf8, "ghostty_key_event_set_utf8");
        DIRBRIDGE_RESOLVE(pasteEncode, "ghostty_paste_encode");
        DIRBRIDGE_RESOLVE(mouseEncoderNew, "ghostty_mouse_encoder_new");
        DIRBRIDGE_RESOLVE(mouseEncoderFree, "ghostty_mouse_encoder_free");
        DIRBRIDGE_RESOLVE(mouseEncoderSet, "ghostty_mouse_encoder_setopt");
        DIRBRIDGE_RESOLVE(mouseEncoderFromTerminal, "ghostty_mouse_encoder_setopt_from_terminal");
        DIRBRIDGE_RESOLVE(mouseEncoderEncode, "ghostty_mouse_encoder_encode");
        DIRBRIDGE_RESOLVE(mouseEventNew, "ghostty_mouse_event_new");
        DIRBRIDGE_RESOLVE(mouseEventFree, "ghostty_mouse_event_free");
        DIRBRIDGE_RESOLVE(mouseEventSetAction, "ghostty_mouse_event_set_action");
        DIRBRIDGE_RESOLVE(mouseEventSetButton, "ghostty_mouse_event_set_button");
        DIRBRIDGE_RESOLVE(mouseEventClearButton, "ghostty_mouse_event_clear_button");
        DIRBRIDGE_RESOLVE(mouseEventSetMods, "ghostty_mouse_event_set_mods");
        DIRBRIDGE_RESOLVE(mouseEventSetPosition, "ghostty_mouse_event_set_position");
#undef DIRBRIDGE_RESOLVE
        return true;
    }

    void cleanup()
    {
        if (mouseEvent && mouseEventFree) mouseEventFree(mouseEvent);
        if (mouseEncoder && mouseEncoderFree) mouseEncoderFree(mouseEncoder);
        if (keyEvent && keyEventFree) keyEventFree(keyEvent);
        if (keyEncoder && keyEncoderFree) keyEncoderFree(keyEncoder);
        if (rowCells && rowCellsFree) rowCellsFree(rowCells);
        if (rowIterator && rowIteratorFree) rowIteratorFree(rowIterator);
        if (renderState && renderFree) renderFree(renderState);
        if (terminal && terminalFree) terminalFree(terminal);
        mouseEvent = nullptr;
        mouseEncoder = nullptr;
        keyEvent = nullptr;
        keyEncoder = nullptr;
        rowCells = nullptr;
        rowIterator = nullptr;
        renderState = nullptr;
        terminal = nullptr;
        library.unload();
    }
};

GhosttyTerminalEngine::GhosttyTerminalEngine()
    : impl_(std::make_unique<Impl>())
{
}

GhosttyTerminalEngine::~GhosttyTerminalEngine()
{
    impl_->cleanup();
}

std::unique_ptr<GhosttyTerminalEngine> GhosttyTerminalEngine::create(
    const std::filesystem::path &libraryPath,
    const TerminalGeometry &geometry,
    std::string &error)
{
    auto engine = std::unique_ptr<GhosttyTerminalEngine>(
        new GhosttyTerminalEngine());
    if (!engine->initialize(libraryPath, geometry))
    {
        error = engine->lastError();
        return nullptr;
    }
    error.clear();
    return engine;
}

bool GhosttyTerminalEngine::initialize(
    const std::filesystem::path &libraryPath,
    const TerminalGeometry &geometry)
{
    if (geometry.columns == 0 || geometry.rows == 0)
    {
        setError("终端初始尺寸无效。");
        return false;
    }
    const QString path = QString::fromStdWString(libraryPath.wstring());
    if (!QFileInfo::exists(path))
    {
        setError(QString("Ghostty VT 运行库不存在：%1").arg(path).toStdString());
        return false;
    }
    impl_->library.setFileName(path);
    if (!impl_->library.load())
    {
        setError(QString("无法加载 Ghostty VT 运行库：%1")
            .arg(impl_->library.errorString()).toStdString());
        return false;
    }
    if (!impl_->resolveAll())
    {
        return false;
    }
    if (impl_->terminalNew(nullptr, &impl_->terminal,
            geometry.columns, geometry.rows) != GHOSTTY_SUCCESS
        || impl_->renderNew(nullptr, &impl_->renderState) != GHOSTTY_SUCCESS
        || impl_->rowIteratorNew(nullptr, &impl_->rowIterator) != GHOSTTY_SUCCESS
        || impl_->rowCellsNew(nullptr, &impl_->rowCells) != GHOSTTY_SUCCESS
        || impl_->keyEncoderNew(nullptr, &impl_->keyEncoder) != GHOSTTY_SUCCESS
        || impl_->keyEventNew(nullptr, &impl_->keyEvent) != GHOSTTY_SUCCESS
        || impl_->mouseEncoderNew(nullptr, &impl_->mouseEncoder) != GHOSTTY_SUCCESS
        || impl_->mouseEventNew(nullptr, &impl_->mouseEvent) != GHOSTTY_SUCCESS)
    {
        setError("Ghostty VT 终端对象初始化失败。");
        return false;
    }

    const GhosttyColorRgb foreground{224, 224, 224};
    const GhosttyColorRgb background{30, 30, 30};
    const GhosttyColorRgb cursor{224, 224, 224};
    const std::size_t scrollbackLines = 5000;
    const std::uint64_t disabledImages = 0;
    const bool disabled = false;
    impl_->terminalSet(impl_->terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &foreground);
    impl_->terminalSet(impl_->terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &background);
    impl_->terminalSet(impl_->terminal, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &cursor);
    impl_->terminalSet(impl_->terminal, GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &scrollbackLines);
    impl_->terminalSet(impl_->terminal, GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &disabledImages);
    impl_->terminalSet(impl_->terminal, GHOSTTY_TERMINAL_OPT_GLYPH_PROTOCOL, &disabled);
    impl_->geometry = geometry;
    return resize(geometry);
}

bool GhosttyTerminalEngine::ingest(const std::uint8_t *bytes, std::size_t size)
{
    impl_->error.clear();
    if (bytes == nullptr && size != 0)
    {
        setError("终端输出缓冲区无效。");
        return false;
    }
    if (size == 0)
    {
        return true;
    }
    impl_->terminalWrite(impl_->terminal, bytes, size);
    impl_->modeTail.append(reinterpret_cast<const char *>(bytes), size);
    const auto enabled = impl_->modeTail.rfind("\x1b[?2004h");
    const auto disabled = impl_->modeTail.rfind("\x1b[?2004l");
    if (enabled != std::string::npos || disabled != std::string::npos)
    {
        impl_->bracketedPaste = disabled == std::string::npos
            || (enabled != std::string::npos && enabled > disabled);
    }
    if (impl_->modeTail.size() > 64)
    {
        impl_->modeTail.erase(0, impl_->modeTail.size() - 64);
    }
    ++impl_->generation;
    return true;
}

bool GhosttyTerminalEngine::resize(const TerminalGeometry &geometry)
{
    impl_->error.clear();
    if (geometry.columns == 0 || geometry.rows == 0)
    {
        setError("终端尺寸无效。");
        return false;
    }
    if (impl_->terminalResize(impl_->terminal,
            geometry.columns, geometry.rows,
            std::max<std::uint32_t>(1, geometry.cellWidthPixels),
            std::max<std::uint32_t>(1, geometry.cellHeightPixels)) != GHOSTTY_SUCCESS)
    {
        setError("Ghostty VT resize 失败。");
        return false;
    }
    impl_->geometry = geometry;
    ++impl_->generation;
    return true;
}

TerminalSnapshotPtr GhosttyTerminalEngine::snapshot()
{
    impl_->error.clear();
    if (impl_->renderUpdate(impl_->renderState, impl_->terminal) != GHOSTTY_SUCCESS)
    {
        setError("Ghostty VT 屏幕快照更新失败。");
        return {};
    }

    auto result = std::make_shared<TerminalSnapshot>();
    result->geometry = impl_->geometry;
    result->generation = impl_->generation;

    GhosttyRenderStateColors colors{};
    colors.size = sizeof(colors);
    if (impl_->renderColorsGet(impl_->renderState, &colors) == GHOSTTY_SUCCESS)
    {
        result->defaultForeground = color(colors.foreground);
        result->defaultBackground = color(colors.background);
        if (colors.cursor_has_value)
        {
            result->cursor.color = color(colors.cursor);
        }
    }

    bool cursorHasValue = false;
    bool cursorVisible = false;
    bool cursorBlinking = false;
    std::uint16_t cursorX = 0;
    std::uint16_t cursorY = 0;
    GhosttyRenderStateCursorVisualStyle visualStyle =
        GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;
    impl_->renderGet(impl_->renderState,
        GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE, &cursorHasValue);
    impl_->renderGet(impl_->renderState,
        GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &cursorVisible);
    impl_->renderGet(impl_->renderState,
        GHOSTTY_RENDER_STATE_DATA_CURSOR_BLINKING, &cursorBlinking);
    impl_->renderGet(impl_->renderState,
        GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &cursorX);
    impl_->renderGet(impl_->renderState,
        GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &cursorY);
    impl_->renderGet(impl_->renderState,
        GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE, &visualStyle);
    result->cursor.column = cursorX;
    result->cursor.row = cursorY;
    result->cursor.visible = cursorHasValue && cursorVisible;
    result->cursor.blinking = cursorBlinking;
    result->cursor.style = cursorStyle(visualStyle);

    GhosttyTerminalScrollbar scrollbar{};
    if (impl_->terminalGet(impl_->terminal,
            GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar) == GHOSTTY_SUCCESS)
    {
        result->scrollTotal = scrollbar.total;
        result->scrollOffset = scrollbar.offset;
        result->scrollLength = scrollbar.len;
    }
    GhosttyTerminalScreen activeScreen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    impl_->terminalGet(impl_->terminal,
        GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &activeScreen);
    result->alternateScreen = activeScreen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE;
    impl_->terminalGet(impl_->terminal,
        GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &result->mouseTracking);

    if (impl_->renderGet(impl_->renderState,
            GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
            &impl_->rowIterator) != GHOSTTY_SUCCESS)
    {
        setError("Ghostty VT 行迭代器初始化失败。");
        return {};
    }

    result->rows.reserve(result->geometry.rows);
    while (impl_->rowIteratorNext(impl_->rowIterator)
        && result->rows.size() < result->geometry.rows)
    {
        if (impl_->rowGet(impl_->rowIterator,
                GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
                &impl_->rowCells) != GHOSTTY_SUCCESS)
        {
            setError("Ghostty VT 单元格迭代器初始化失败。");
            return {};
        }
        std::vector<TerminalCell> row;
        row.reserve(result->geometry.columns);
        while (impl_->rowCellsNext(impl_->rowCells)
            && row.size() < result->geometry.columns)
        {
            TerminalCell cell;
            GhosttyCell rawCell = 0;
            GhosttyCellWide cellWide = GHOSTTY_CELL_WIDE_NARROW;
            if (impl_->rowCellsGet(impl_->rowCells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
                    &rawCell) == GHOSTTY_SUCCESS
                && impl_->cellGet(rawCell, GHOSTTY_CELL_DATA_WIDE,
                    &cellWide) == GHOSTTY_SUCCESS)
            {
                switch (cellWide)
                {
                case GHOSTTY_CELL_WIDE_WIDE:
                    cell.width = TerminalCellWidth::Wide;
                    break;
                case GHOSTTY_CELL_WIDE_SPACER_TAIL:
                    cell.width = TerminalCellWidth::SpacerTail;
                    break;
                case GHOSTTY_CELL_WIDE_SPACER_HEAD:
                    cell.width = TerminalCellWidth::SpacerHead;
                    break;
                case GHOSTTY_CELL_WIDE_NARROW:
                default:
                    cell.width = TerminalCellWidth::Narrow;
                    break;
                }
            }
            std::array<char, 128> grapheme{};
            GhosttyBuffer buffer{
                reinterpret_cast<std::uint8_t *>(grapheme.data()),
                grapheme.size(), 0};
            if (impl_->rowCellsGet(impl_->rowCells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8,
                    &buffer) == GHOSTTY_SUCCESS && buffer.len > 0)
            {
                cell.text.assign(grapheme.data(), buffer.len);
            }
            GhosttyColorRgb foreground{};
            if (impl_->rowCellsGet(impl_->rowCells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
                    &foreground) == GHOSTTY_SUCCESS)
            {
                cell.foreground = color(foreground);
            }
            GhosttyColorRgb background{};
            if (impl_->rowCellsGet(impl_->rowCells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
                    &background) == GHOSTTY_SUCCESS)
            {
                cell.background = color(background);
            }
            GhosttyStyle style{};
            style.size = sizeof(style);
            if (impl_->rowCellsGet(impl_->rowCells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
                    &style) == GHOSTTY_SUCCESS)
            {
                cell.bold = style.bold;
                cell.italic = style.italic;
                cell.faint = style.faint;
                cell.underline = style.underline != 0;
                cell.inverse = style.inverse;
                cell.invisible = style.invisible;
                cell.strikethrough = style.strikethrough;
            }
            row.push_back(std::move(cell));
        }
        row.resize(result->geometry.columns);
        result->rows.push_back(std::move(row));
    }
    result->rows.resize(result->geometry.rows);
    for (auto &row : result->rows)
    {
        row.resize(result->geometry.columns);
    }
    return result;
}

std::vector<std::uint8_t> GhosttyTerminalEngine::encodeKey(
    const TerminalKeyEvent &event)
{
    impl_->error.clear();
    const GhosttyKey key = ghosttyKey(event.key);
    if (key == GHOSTTY_KEY_UNIDENTIFIED)
    {
        return {};
    }
    GhosttyMods mods = 0;
    if (event.shift) mods |= GHOSTTY_MODS_SHIFT;
    if (event.control) mods |= GHOSTTY_MODS_CTRL;
    if (event.alt) mods |= GHOSTTY_MODS_ALT;
    impl_->keyEncoderFromTerminal(impl_->keyEncoder, impl_->terminal);
    impl_->keyEventSetAction(impl_->keyEvent,
        event.autoRepeat ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS);
    impl_->keyEventSetKey(impl_->keyEvent, key);
    impl_->keyEventSetMods(impl_->keyEvent, mods);
    impl_->keyEventSetUtf8(impl_->keyEvent,
        event.text.empty() ? nullptr : event.text.data(), event.text.size());

    std::array<char, 128> buffer{};
    std::size_t written = 0;
    GhosttyResult encoded = impl_->keyEncoderEncode(
        impl_->keyEncoder, impl_->keyEvent,
        buffer.data(), buffer.size(), &written);
    if (encoded == GHOSTTY_SUCCESS)
    {
        return std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + written);
    }
    if (encoded != GHOSTTY_OUT_OF_SPACE || written == 0)
    {
        setError("Ghostty VT 按键编码失败。");
        return {};
    }
    std::vector<char> dynamicBuffer(written);
    if (impl_->keyEncoderEncode(impl_->keyEncoder, impl_->keyEvent,
            dynamicBuffer.data(), dynamicBuffer.size(), &written) != GHOSTTY_SUCCESS)
    {
        setError("Ghostty VT 按键编码失败。");
        return {};
    }
    return std::vector<std::uint8_t>(
        dynamicBuffer.begin(), dynamicBuffer.begin() + written);
}

std::vector<std::uint8_t> GhosttyTerminalEngine::encodeText(
    const std::string &utf8)
{
    impl_->error.clear();
    return std::vector<std::uint8_t>(utf8.begin(), utf8.end());
}

std::vector<std::uint8_t> GhosttyTerminalEngine::encodePaste(
    const std::string &utf8)
{
    impl_->error.clear();
    std::vector<char> mutableText(utf8.begin(), utf8.end());
    std::size_t required = 0;
    const GhosttyResult sized = impl_->pasteEncode(
        mutableText.data(), mutableText.size(), impl_->bracketedPaste,
        nullptr, 0, &required);
    if (sized != GHOSTTY_OUT_OF_SPACE && !(sized == GHOSTTY_SUCCESS && required == 0))
    {
        setError("Ghostty VT 粘贴编码失败。");
        return {};
    }
    if (required == 0)
    {
        return {};
    }
    mutableText.assign(utf8.begin(), utf8.end());
    std::vector<char> output(required);
    if (impl_->pasteEncode(mutableText.data(), mutableText.size(),
            impl_->bracketedPaste, output.data(), output.size(),
            &required) != GHOSTTY_SUCCESS)
    {
        setError("Ghostty VT 粘贴编码失败。");
        return {};
    }
    return std::vector<std::uint8_t>(output.begin(), output.begin() + required);
}

std::vector<std::uint8_t> GhosttyTerminalEngine::encodeMouse(
    const TerminalMouseEvent &event)
{
    impl_->error.clear();
    impl_->mouseEncoderFromTerminal(impl_->mouseEncoder, impl_->terminal);
    GhosttyMouseEncoderSize size{};
    size.size = sizeof(size);
    size.screen_width = impl_->geometry.columns * impl_->geometry.cellWidthPixels;
    size.screen_height = impl_->geometry.rows * impl_->geometry.cellHeightPixels;
    size.cell_width = std::max<std::uint32_t>(1, impl_->geometry.cellWidthPixels);
    size.cell_height = std::max<std::uint32_t>(1, impl_->geometry.cellHeightPixels);
    impl_->mouseEncoderSet(impl_->mouseEncoder,
        GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
    impl_->mouseEncoderSet(impl_->mouseEncoder,
        GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED, &impl_->mouseButtonPressed);
    const bool trackLastCell = true;
    impl_->mouseEncoderSet(impl_->mouseEncoder,
        GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &trackLastCell);

    GhosttyMouseAction action = GHOSTTY_MOUSE_ACTION_MOTION;
    if (event.action == TerminalMouseAction::Press) action = GHOSTTY_MOUSE_ACTION_PRESS;
    if (event.action == TerminalMouseAction::Release) action = GHOSTTY_MOUSE_ACTION_RELEASE;
    impl_->mouseEventSetAction(impl_->mouseEvent, action);
    GhosttyMouseButton button = GHOSTTY_MOUSE_BUTTON_UNKNOWN;
    if (event.button == TerminalMouseButton::Left) button = GHOSTTY_MOUSE_BUTTON_LEFT;
    if (event.button == TerminalMouseButton::Middle) button = GHOSTTY_MOUSE_BUTTON_MIDDLE;
    if (event.button == TerminalMouseButton::Right) button = GHOSTTY_MOUSE_BUTTON_RIGHT;
    if (event.button == TerminalMouseButton::WheelUp) button = GHOSTTY_MOUSE_BUTTON_FOUR;
    if (event.button == TerminalMouseButton::WheelDown) button = GHOSTTY_MOUSE_BUTTON_FIVE;
    if (event.button == TerminalMouseButton::None)
        impl_->mouseEventClearButton(impl_->mouseEvent);
    else
        impl_->mouseEventSetButton(impl_->mouseEvent, button);
    GhosttyMods mods = 0;
    if (event.shift) mods |= GHOSTTY_MODS_SHIFT;
    if (event.control) mods |= GHOSTTY_MODS_CTRL;
    if (event.alt) mods |= GHOSTTY_MODS_ALT;
    impl_->mouseEventSetMods(impl_->mouseEvent, mods);
    impl_->mouseEventSetPosition(impl_->mouseEvent,
        GhosttyMousePosition{event.xPixels, event.yPixels});

    std::array<char, 128> buffer{};
    std::size_t written = 0;
    const GhosttyResult encoded = impl_->mouseEncoderEncode(
        impl_->mouseEncoder, impl_->mouseEvent,
        buffer.data(), buffer.size(), &written);
    if (encoded != GHOSTTY_SUCCESS)
    {
        setError("Ghostty VT 鼠标编码失败。");
        return {};
    }
    if (event.action == TerminalMouseAction::Press)
        impl_->mouseButtonPressed = true;
    else if (event.action == TerminalMouseAction::Release)
        impl_->mouseButtonPressed = false;
    return std::vector<std::uint8_t>(buffer.begin(), buffer.begin() + written);
}

bool GhosttyTerminalEngine::scrollLines(int lines)
{
    impl_->error.clear();
    GhosttyTerminalScrollViewport viewport{};
    viewport.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA;
    viewport.value.delta = lines;
    impl_->terminalScroll(impl_->terminal, viewport);
    ++impl_->generation;
    return true;
}

std::string GhosttyTerminalEngine::lastError() const
{
    return impl_->error;
}

void GhosttyTerminalEngine::setError(std::string message)
{
    impl_->error = std::move(message);
}

} // namespace dirbridge::terminal
