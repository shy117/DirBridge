#include "ui/TerminalWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStringList>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>

using dirbridge::terminal::TerminalColor;
using dirbridge::terminal::TerminalCellWidth;
using dirbridge::terminal::TerminalCursorStyle;
using dirbridge::terminal::TerminalGeometry;
using dirbridge::terminal::TerminalKey;
using dirbridge::terminal::TerminalKeyEvent;
using dirbridge::terminal::TerminalMouseAction;
using dirbridge::terminal::TerminalMouseButton;
using dirbridge::terminal::TerminalMouseEvent;

namespace {

QColor color(const TerminalColor &value, const QColor &fallback)
{
    return value.valid
        ? QColor(value.red, value.green, value.blue)
        : fallback;
}

TerminalKey letterKey(int key)
{
    if (key < Qt::Key_A || key > Qt::Key_Z)
    {
        return TerminalKey::Unknown;
    }
    return static_cast<TerminalKey>(
        static_cast<int>(TerminalKey::A) + key - Qt::Key_A);
}

TerminalKey specialKey(int key)
{
    switch (key)
    {
    case Qt::Key_Backspace: return TerminalKey::Backspace;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return TerminalKey::Tab;
    case Qt::Key_Return:
    case Qt::Key_Enter: return TerminalKey::Enter;
    case Qt::Key_Escape: return TerminalKey::Escape;
    case Qt::Key_Insert: return TerminalKey::Insert;
    case Qt::Key_Delete: return TerminalKey::Delete;
    case Qt::Key_Home: return TerminalKey::Home;
    case Qt::Key_End: return TerminalKey::End;
    case Qt::Key_PageUp: return TerminalKey::PageUp;
    case Qt::Key_PageDown: return TerminalKey::PageDown;
    case Qt::Key_Up: return TerminalKey::ArrowUp;
    case Qt::Key_Down: return TerminalKey::ArrowDown;
    case Qt::Key_Left: return TerminalKey::ArrowLeft;
    case Qt::Key_Right: return TerminalKey::ArrowRight;
    case Qt::Key_F1: return TerminalKey::F1;
    case Qt::Key_F2: return TerminalKey::F2;
    case Qt::Key_F3: return TerminalKey::F3;
    case Qt::Key_F4: return TerminalKey::F4;
    case Qt::Key_F5: return TerminalKey::F5;
    case Qt::Key_F6: return TerminalKey::F6;
    case Qt::Key_F7: return TerminalKey::F7;
    case Qt::Key_F8: return TerminalKey::F8;
    case Qt::Key_F9: return TerminalKey::F9;
    case Qt::Key_F10: return TerminalKey::F10;
    case Qt::Key_F11: return TerminalKey::F11;
    case Qt::Key_F12: return TerminalKey::F12;
    default: return letterKey(key);
    }
}

} // namespace

TerminalWidget::TerminalWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("terminalWidget");
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setMouseTracking(true);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setAutoFillBackground(false);
    m_scrollBar = new QScrollBar(Qt::Vertical, this);
    m_scrollBar->setObjectName("terminalScrollBar");
    m_scrollBar->setVisible(false);
    connect(m_scrollBar, &QScrollBar::valueChanged, this, [this](int value) {
        if (!m_snapshot || m_scrollBar->signalsBlocked())
        {
            return;
        }
        const qint64 delta = static_cast<qint64>(value)
            - m_requestedScrollOffset;
        m_requestedScrollOffset = value;
        if (delta != 0)
        {
            Q_EMIT scrollRequested(static_cast<int>(delta));
        }
    });
    updateMetrics();
    m_resizeTimer.setSingleShot(true);
    m_resizeTimer.setInterval(50);
    connect(&m_resizeTimer, &QTimer::timeout,
        this, &TerminalWidget::emitCurrentGeometry);
}

void TerminalWidget::setSnapshot(
    dirbridge::terminal::TerminalSnapshotPtr snapshot)
{
    m_snapshot = std::move(snapshot);
    if (m_snapshot && !m_statusError)
    {
        m_status.clear();
    }
    updateScrollBar();
    update();
}

void TerminalWidget::setStatus(const QString &message, bool error)
{
    m_status = message;
    m_statusError = error;
    update();
}

bool TerminalWidget::event(QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Tab
            || keyEvent->key() == Qt::Key_Backtab)
        {
            keyPressEvent(keyEvent);
            return true;
        }
    }
    return QWidget::event(event);
}

bool TerminalWidget::hasSelection() const noexcept
{
    return m_hasSelection;
}

QString TerminalWidget::selectedText() const
{
    if (!m_snapshot || !m_hasSelection)
    {
        return {};
    }
    CellPoint first = m_selectionAnchor;
    CellPoint last = m_selectionCursor;
    if (first.row > last.row
        || (first.row == last.row && first.column > last.column))
    {
        std::swap(first, last);
    }
    QStringList lines;
    for (int row = first.row; row <= last.row
        && row < static_cast<int>(m_snapshot->rows.size()); ++row)
    {
        const int start = row == first.row ? first.column : 0;
        const int end = row == last.row
            ? last.column
            : static_cast<int>(m_snapshot->rows[row].size()) - 1;
        QString line;
        for (int column = start; column <= end
            && column < static_cast<int>(m_snapshot->rows[row].size()); ++column)
        {
            line += QString::fromUtf8(
                m_snapshot->rows[row][column].text.data(),
                static_cast<int>(m_snapshot->rows[row][column].text.size()));
        }
        while (line.endsWith(' '))
        {
            line.chop(1);
        }
        lines.push_back(line);
    }
    return lines.join('\n');
}

void TerminalWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const QColor fallbackBackground(30, 30, 30);
    const QColor fallbackForeground(224, 224, 224);
    const QColor defaultBackground = m_snapshot
        ? color(m_snapshot->defaultBackground, fallbackBackground)
        : fallbackBackground;
    const QColor defaultForeground = m_snapshot
        ? color(m_snapshot->defaultForeground, fallbackForeground)
        : fallbackForeground;
    painter.fillRect(rect(), defaultBackground);

    if (m_snapshot)
    {
        QFont baseFont = font();
        // Backgrounds are painted first so a wide character's spacer cell
        // cannot erase the right half of its glyph.
        for (int row = 0; row < static_cast<int>(m_snapshot->rows.size()); ++row)
        {
            const auto &cells = m_snapshot->rows[row];
            for (int column = 0; column < static_cast<int>(cells.size()); ++column)
            {
                const auto &cell = cells[column];
                QRect cellRect(
                    m_margin + column * m_cellWidth,
                    m_margin + row * m_cellHeight,
                    m_cellWidth,
                    m_cellHeight);
                QColor foreground = color(cell.foreground, defaultForeground);
                QColor background = color(cell.background, defaultBackground);
                if (cell.inverse)
                {
                    std::swap(foreground, background);
                }
                if (isSelected(column, row))
                {
                    background = QColor(65, 105, 170);
                    foreground = Qt::white;
                }
                painter.fillRect(cellRect, background);
            }
        }

        for (int row = 0; row < static_cast<int>(m_snapshot->rows.size()); ++row)
        {
            const auto &cells = m_snapshot->rows[row];
            for (int column = 0; column < static_cast<int>(cells.size()); ++column)
            {
                const auto &cell = cells[column];
                if (cell.width == TerminalCellWidth::SpacerTail
                    || cell.width == TerminalCellWidth::SpacerHead
                    || cell.invisible || cell.text.empty())
                {
                    continue;
                }
                const int widthInCells = cell.width == TerminalCellWidth::Wide
                    ? 2
                    : 1;
                QRect cellRect(
                    m_margin + column * m_cellWidth,
                    m_margin + row * m_cellHeight,
                    widthInCells * m_cellWidth,
                    m_cellHeight);
                QColor foreground = color(cell.foreground, defaultForeground);
                QColor background = color(cell.background, defaultBackground);
                if (cell.inverse)
                {
                    std::swap(foreground, background);
                }
                const bool selected = isSelected(column, row)
                    || (widthInCells == 2 && isSelected(column + 1, row));
                if (selected)
                {
                    foreground = Qt::white;
                }
                QFont cellFont = baseFont;
                cellFont.setBold(cell.bold);
                cellFont.setItalic(cell.italic);
                cellFont.setUnderline(cell.underline);
                cellFont.setStrikeOut(cell.strikethrough);
                painter.setFont(cellFont);
                if (cell.faint)
                {
                    foreground.setAlpha(145);
                }
                painter.setPen(foreground);
                painter.save();
                painter.setClipRect(cellRect);
                painter.drawText(
                    QPoint(cellRect.left(), cellRect.top() + m_ascent),
                    QString::fromUtf8(cell.text.data(),
                        static_cast<int>(cell.text.size())));
                painter.restore();
            }
        }

        if (m_snapshot->cursor.visible)
        {
            const auto &cursor = m_snapshot->cursor;
            int cursorColumn = cursor.column;
            int cursorWidthInCells = 1;
            if (cursor.row < m_snapshot->rows.size()
                && cursor.column < m_snapshot->rows[cursor.row].size())
            {
                const auto &cursorCell =
                    m_snapshot->rows[cursor.row][cursor.column];
                if (cursorCell.width == TerminalCellWidth::Wide)
                {
                    cursorWidthInCells = 2;
                }
                else if (cursorCell.width == TerminalCellWidth::SpacerTail
                    && cursorColumn > 0)
                {
                    --cursorColumn;
                    cursorWidthInCells = 2;
                }
            }
            QRect cursorRect(
                m_margin + cursorColumn * m_cellWidth,
                m_margin + cursor.row * m_cellHeight,
                cursorWidthInCells * m_cellWidth,
                m_cellHeight);
            QColor cursorColor = color(cursor.color, defaultForeground);
            switch (cursor.style)
            {
            case TerminalCursorStyle::Bar:
                painter.fillRect(cursorRect.left(), cursorRect.top(),
                    2, cursorRect.height(), cursorColor);
                break;
            case TerminalCursorStyle::Underline:
                painter.fillRect(cursorRect.left(), cursorRect.bottom() - 1,
                    cursorRect.width(), 2, cursorColor);
                break;
            case TerminalCursorStyle::HollowBlock:
                painter.setPen(cursorColor);
                painter.drawRect(cursorRect.adjusted(0, 0, -1, -1));
                break;
            case TerminalCursorStyle::Block:
                cursorColor.setAlpha(110);
                painter.fillRect(cursorRect, cursorColor);
                break;
            }
            if (!m_preedit.isEmpty())
            {
                painter.setPen(defaultForeground);
                painter.setFont(font());
                painter.drawText(
                    QPoint(cursorRect.left(), cursorRect.top() + m_ascent),
                    m_preedit);
                painter.drawLine(cursorRect.bottomLeft(), cursorRect.bottomRight());
            }
        }
    }

    if (!m_status.isEmpty())
    {
        painter.setFont(font());
        const QFontMetrics metrics(font());
        QRect textRect = metrics.boundingRect(
            QRect(0, 0, std::max(200, width() - 24), height()),
            Qt::TextWordWrap, m_status).adjusted(-8, -5, 8, 5);
        textRect.moveTopLeft(QPoint(10, 8));
        painter.fillRect(textRect,
            m_statusError ? QColor(120, 35, 35, 225) : QColor(45, 45, 45, 210));
        painter.setPen(Qt::white);
        painter.drawText(textRect.adjusted(8, 5, -8, -5),
            Qt::TextWordWrap, m_status);
    }
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutScrollBar();
    m_resizeTimer.start();
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
    const bool control = event->modifiers().testFlag(Qt::ControlModifier);
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const bool alt = event->modifiers().testFlag(Qt::AltModifier);
    if (control && shift && event->key() == Qt::Key_C)
    {
        copySelection();
        event->accept();
        return;
    }
    if (control && event->key() == Qt::Key_C && hasSelection())
    {
        copySelection();
        event->accept();
        return;
    }
    if (control && event->key() == Qt::Key_V)
    {
        pasteClipboard();
        event->accept();
        return;
    }

    TerminalKey key = specialKey(event->key());
    if (key != TerminalKey::Unknown
        && (control || alt || event->text().isEmpty()
            || event->key() == Qt::Key_Backspace
            || event->key() == Qt::Key_Tab
            || event->key() == Qt::Key_Backtab
            || event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter
            || event->key() == Qt::Key_Escape))
    {
        scrollToBottom();
        TerminalKeyEvent terminalEvent;
        terminalEvent.key = key;
        terminalEvent.text = event->text().toUtf8().toStdString();
        terminalEvent.shift = shift;
        terminalEvent.control = control;
        terminalEvent.alt = alt;
        terminalEvent.autoRepeat = event->isAutoRepeat();
        Q_EMIT keyInput(terminalEvent);
        event->accept();
        return;
    }
    if (!event->text().isEmpty() && !control && !alt)
    {
        scrollToBottom();
        Q_EMIT textInput(event->text().toUtf8());
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void TerminalWidget::inputMethodEvent(QInputMethodEvent *event)
{
    m_preedit = event->preeditString();
    if (!event->commitString().isEmpty())
    {
        scrollToBottom();
        Q_EMIT textInput(event->commitString().toUtf8());
    }
    update();
    event->accept();
}

QVariant TerminalWidget::inputMethodQuery(Qt::InputMethodQuery query) const
{
    if (query == Qt::ImCursorRectangle && m_snapshot)
    {
        return QRect(
            m_margin + m_snapshot->cursor.column * m_cellWidth,
            m_margin + m_snapshot->cursor.row * m_cellHeight,
            m_cellWidth, m_cellHeight);
    }
    if (query == Qt::ImFont)
    {
        return font();
    }
    return QWidget::inputMethodQuery(query);
}

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::RightButton)
    {
        pasteClipboard();
        event->accept();
        return;
    }
    if (m_snapshot && m_snapshot->mouseTracking
        && !event->modifiers().testFlag(Qt::ShiftModifier))
    {
        Q_EMIT mouseInput(mouseEvent(event, TerminalMouseAction::Press));
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton)
    {
        m_selectionAnchor = cellAt(event->position());
        m_selectionCursor = m_selectionAnchor;
        m_selecting = true;
        m_hasSelection = false;
        update();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (m_snapshot && m_snapshot->mouseTracking
        && !event->modifiers().testFlag(Qt::ShiftModifier))
    {
        Q_EMIT mouseInput(mouseEvent(event, TerminalMouseAction::Move));
        event->accept();
        return;
    }
    if (m_selecting)
    {
        m_selectionCursor = cellAt(event->position());
        m_hasSelection = m_selectionCursor.column != m_selectionAnchor.column
            || m_selectionCursor.row != m_selectionAnchor.row;
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton)
    {
        event->accept();
        return;
    }
    if (m_snapshot && m_snapshot->mouseTracking
        && !event->modifiers().testFlag(Qt::ShiftModifier))
    {
        Q_EMIT mouseInput(mouseEvent(event, TerminalMouseAction::Release));
        event->accept();
        return;
    }
    if (m_selecting && event->button() == Qt::LeftButton)
    {
        m_selectionCursor = cellAt(event->position());
        m_hasSelection = m_selectionCursor.column != m_selectionAnchor.column
            || m_selectionCursor.row != m_selectionAnchor.row;
        m_selecting = false;
        update();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TerminalWidget::wheelEvent(QWheelEvent *event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0)
    {
        event->ignore();
        return;
    }
    if (m_snapshot && m_snapshot->mouseTracking
        && !event->modifiers().testFlag(Qt::ShiftModifier))
    {
        TerminalMouseEvent terminalEvent;
        terminalEvent.action = TerminalMouseAction::Press;
        terminalEvent.button = steps > 0
            ? TerminalMouseButton::WheelUp
            : TerminalMouseButton::WheelDown;
        terminalEvent.xPixels = static_cast<float>(event->position().x() - m_margin);
        terminalEvent.yPixels = static_cast<float>(event->position().y() - m_margin);
        terminalEvent.shift = event->modifiers().testFlag(Qt::ShiftModifier);
        terminalEvent.control = event->modifiers().testFlag(Qt::ControlModifier);
        terminalEvent.alt = event->modifiers().testFlag(Qt::AltModifier);
        Q_EMIT mouseInput(terminalEvent);
    }
    else
    {
        Q_EMIT scrollRequested(-steps * 3);
    }
    event->accept();
}

void TerminalWidget::updateMetrics()
{
    const QFontMetrics metrics(font());
    m_cellWidth = std::max(1, metrics.horizontalAdvance(QLatin1Char('M')));
    m_cellHeight = std::max(1, metrics.height());
    m_ascent = metrics.ascent();
}

void TerminalWidget::layoutScrollBar()
{
    if (m_scrollBar == nullptr)
    {
        return;
    }
    const int scrollBarWidth = m_scrollBar->sizeHint().width();
    m_scrollBar->setGeometry(
        std::max(0, width() - scrollBarWidth),
        0,
        scrollBarWidth,
        height());
}

void TerminalWidget::updateScrollBar()
{
    if (m_scrollBar == nullptr)
    {
        return;
    }
    const bool shouldShow = m_snapshot
        && !m_snapshot->alternateScreen
        && m_snapshot->scrollTotal > m_snapshot->scrollLength;
    const bool visibilityChanged = !m_scrollBar->isHidden() != shouldShow;
    if (shouldShow)
    {
        const std::uint64_t maximum = m_snapshot->scrollTotal - m_snapshot->scrollLength;
        const QSignalBlocker blocker(m_scrollBar);
        m_scrollBar->setRange(0, static_cast<int>(std::min<std::uint64_t>(
            maximum, static_cast<std::uint64_t>(std::numeric_limits<int>::max()))));
        m_scrollBar->setPageStep(static_cast<int>(std::min<std::uint64_t>(
            m_snapshot->scrollLength,
            static_cast<std::uint64_t>(std::numeric_limits<int>::max()))));
        m_scrollBar->setSingleStep(1);
        const int offset = static_cast<int>(std::min<std::uint64_t>(
            m_snapshot->scrollOffset,
            static_cast<std::uint64_t>(m_scrollBar->maximum())));
        m_scrollBar->setValue(offset);
        m_requestedScrollOffset = offset;
    }
    else
    {
        m_requestedScrollOffset = 0;
    }
    m_scrollBar->setVisible(shouldShow);
    layoutScrollBar();
    if (visibilityChanged)
    {
        m_resizeTimer.start();
    }
}

void TerminalWidget::scrollToBottom()
{
    if (!m_snapshot || m_snapshot->alternateScreen
        || m_snapshot->scrollTotal <= m_snapshot->scrollLength)
    {
        return;
    }
    const std::uint64_t bottom = m_snapshot->scrollTotal - m_snapshot->scrollLength;
    const qint64 delta = static_cast<qint64>(bottom)
        - static_cast<qint64>(m_snapshot->scrollOffset);
    if (delta != 0)
    {
        Q_EMIT scrollRequested(static_cast<int>(std::clamp<qint64>(
            delta,
            std::numeric_limits<int>::min(),
            std::numeric_limits<int>::max())));
    }
}

int TerminalWidget::terminalContentWidth() const
{
    const int scrollBarWidth = m_scrollBar != nullptr && m_scrollBar->isVisible()
        ? m_scrollBar->width()
        : 0;
    return std::max(1, width() - scrollBarWidth);
}

void TerminalWidget::emitCurrentGeometry()
{
    updateMetrics();
    const int availableWidth = std::max(1, terminalContentWidth() - 2 * m_margin);
    const int availableHeight = std::max(1, height() - 2 * m_margin);
    TerminalGeometry geometry;
    geometry.columns = static_cast<std::uint16_t>(
        std::clamp(availableWidth / m_cellWidth, 1, 1000));
    geometry.rows = static_cast<std::uint16_t>(
        std::clamp(availableHeight / m_cellHeight, 1, 1000));
    geometry.cellWidthPixels = static_cast<std::uint32_t>(m_cellWidth);
    geometry.cellHeightPixels = static_cast<std::uint32_t>(m_cellHeight);
    Q_EMIT resizeRequested(geometry);
}

void TerminalWidget::pasteClipboard()
{
    const QString text = QApplication::clipboard()->text();
    const QByteArray utf8 = text.toUtf8();
    constexpr qsizetype maximumPaste = 1024 * 1024;
    if (utf8.size() > maximumPaste)
    {
        QMessageBox::warning(this, "无法粘贴",
            "粘贴内容超过 1 MiB，已拒绝发送。");
        return;
    }
    const qsizetype lineCount = std::max<qsizetype>(
        1, text.count(QLatin1Char('\n')) + 1);
    if (lineCount > 1 || utf8.size() > 64 * 1024)
    {
        const auto answer = QMessageBox::question(this, "确认粘贴",
            QString("将向远端终端粘贴 %1 行、%2 字节内容。是否继续？")
                .arg(lineCount).arg(utf8.size()));
        if (answer != QMessageBox::Yes)
        {
            return;
        }
    }
    scrollToBottom();
    Q_EMIT pasteInput(utf8);
}

void TerminalWidget::copySelection()
{
    if (m_hasSelection)
    {
        QApplication::clipboard()->setText(selectedText());
    }
}

TerminalWidget::CellPoint TerminalWidget::cellAt(const QPointF &position) const
{
    CellPoint point;
    const int maximumColumn = m_snapshot
        ? std::max(0, static_cast<int>(m_snapshot->geometry.columns) - 1)
        : 0;
    const int maximumRow = m_snapshot
        ? std::max(0, static_cast<int>(m_snapshot->geometry.rows) - 1)
        : 0;
    point.column = std::clamp(
        static_cast<int>((position.x() - m_margin) / m_cellWidth),
        0, maximumColumn);
    point.row = std::clamp(
        static_cast<int>((position.y() - m_margin) / m_cellHeight),
        0, maximumRow);
    if (m_snapshot
        && point.row < static_cast<int>(m_snapshot->rows.size())
        && point.column < static_cast<int>(m_snapshot->rows[point.row].size())
        && m_snapshot->rows[point.row][point.column].width
            == TerminalCellWidth::SpacerTail
        && point.column > 0)
    {
        --point.column;
    }
    return point;
}

bool TerminalWidget::isSelected(int column, int row) const
{
    if (!m_hasSelection)
    {
        return false;
    }
    CellPoint first = m_selectionAnchor;
    CellPoint last = m_selectionCursor;
    if (first.row > last.row
        || (first.row == last.row && first.column > last.column))
    {
        std::swap(first, last);
    }
    if (row < first.row || row > last.row)
    {
        return false;
    }
    if (first.row == last.row)
    {
        return column >= first.column && column <= last.column;
    }
    if (row == first.row)
    {
        return column >= first.column;
    }
    if (row == last.row)
    {
        return column <= last.column;
    }
    return true;
}

TerminalMouseEvent TerminalWidget::mouseEvent(
    QMouseEvent *event,
    TerminalMouseAction action) const
{
    TerminalMouseEvent result;
    result.action = action;
    if (event->button() == Qt::LeftButton) result.button = TerminalMouseButton::Left;
    else if (event->button() == Qt::MiddleButton) result.button = TerminalMouseButton::Middle;
    else if (event->button() == Qt::RightButton) result.button = TerminalMouseButton::Right;
    result.xPixels = static_cast<float>(event->position().x() - m_margin);
    result.yPixels = static_cast<float>(event->position().y() - m_margin);
    result.shift = event->modifiers().testFlag(Qt::ShiftModifier);
    result.control = event->modifiers().testFlag(Qt::ControlModifier);
    result.alt = event->modifiers().testFlag(Qt::AltModifier);
    return result;
}
