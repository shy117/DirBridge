#include "ui/TerminalWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QFontMetrics>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <iostream>
#include <memory>

using namespace dirbridge::terminal;

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    QWidget host;
    QVBoxLayout layout(&host);
    TerminalWidget widget(&host);
    QLineEdit focusTarget(&host);
    layout.addWidget(&widget);
    layout.addWidget(&focusTarget);
    host.resize(500, 300);

    auto mutableSnapshot = std::make_shared<TerminalSnapshot>();
    mutableSnapshot->geometry.columns = 20;
    mutableSnapshot->geometry.rows = 4;
    mutableSnapshot->rows.resize(4);
    for (auto &row : mutableSnapshot->rows)
    {
        row.resize(20);
    }
    mutableSnapshot->rows[0][0].text = "O";
    mutableSnapshot->rows[0][1].text = "K";
    mutableSnapshot->rows[1][0].text = "WW";
    mutableSnapshot->rows[1][0].width = TerminalCellWidth::Wide;
    mutableSnapshot->rows[1][1].width = TerminalCellWidth::SpacerTail;
    mutableSnapshot->rows[1][1].background = {255, 0, 0, true};
    mutableSnapshot->cursor.visible = true;
    mutableSnapshot->cursor.column = 2;
    mutableSnapshot->scrollTotal = 100;
    mutableSnapshot->scrollLength = 4;
    mutableSnapshot->scrollOffset = 96;
    widget.setSnapshot(mutableSnapshot);

    QByteArray textBytes;
    TerminalKeyEvent key;
    bool sawKey = false;
    bool sawResize = false;
    int requestedScrollLines = 0;
    QObject::connect(&widget, &TerminalWidget::textInput,
        [&](const QByteArray &bytes) { textBytes += bytes; });
    QObject::connect(&widget, &TerminalWidget::keyInput,
        [&](const TerminalKeyEvent &event) {
            key = event;
            sawKey = true;
        });
    QObject::connect(&widget, &TerminalWidget::resizeRequested,
        [&](const TerminalGeometry &geometry) {
            sawResize = geometry.columns > 0 && geometry.rows > 0
                && geometry.cellWidthPixels > 0
                && geometry.cellHeightPixels > 0;
        });
    QObject::connect(&widget, &TerminalWidget::scrollRequested,
        [&](int lines) { requestedScrollLines += lines; });

    host.show();
    widget.setFocus(Qt::OtherFocusReason);
    application.processEvents();
    QScrollBar *scrollBar = widget.findChild<QScrollBar *>("terminalScrollBar");
    if (scrollBar == nullptr || !scrollBar->isVisible()
        || scrollBar->maximum() != 96 || scrollBar->value() != 96)
    {
        return 8;
    }
    scrollBar->setValue(80);
    scrollBar->setValue(72);
    if (requestedScrollLines != -24)
    {
        return 9;
    }
    mutableSnapshot->scrollOffset = 72;
    widget.setSnapshot(mutableSnapshot);
    requestedScrollLines = 0;
    QKeyEvent tabEvent(QEvent::KeyPress, Qt::Key_Tab,
        Qt::NoModifier, QStringLiteral("\t"));
    QApplication::sendEvent(&widget, &tabEvent);
    if (!sawKey || key.key != TerminalKey::Tab
        || QApplication::focusWidget() != &widget)
    {
        return 5;
    }
    if (requestedScrollLines != 24)
    {
        return 10;
    }

    QKeyEvent textEvent(QEvent::KeyPress, Qt::Key_A,
        Qt::NoModifier, QStringLiteral("a"));
    QApplication::sendEvent(&widget, &textEvent);
    QKeyEvent enterEvent(QEvent::KeyPress, Qt::Key_Return,
        Qt::NoModifier, QStringLiteral("\r"));
    QApplication::sendEvent(&widget, &enterEvent);
    if (textBytes != QByteArray("a") || !sawKey
        || key.key != TerminalKey::Enter)
    {
        return 1;
    }

    QInputMethodEvent inputMethod;
    inputMethod.setCommitString(QString::fromUtf8("中文"));
    QApplication::sendEvent(&widget, &inputMethod);
    if (!textBytes.endsWith(QByteArray::fromHex("e4b8ade69687")))
    {
        return 2;
    }

    host.resize(520, 320);
    QEventLoop wait;
    QTimer::singleShot(100, &wait, &QEventLoop::quit);
    wait.exec();
    if (!sawResize)
    {
        return 3;
    }

    QImage image(widget.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    painter.end();
    if (image.isNull())
    {
        return 4;
    }

    const QFontMetrics metrics(widget.font());
    const int cellWidth = std::max(
        1, metrics.horizontalAdvance(QLatin1Char('M')));
    const int cellHeight = std::max(1, metrics.height());
    bool wideGlyphReachedTail = false;
    for (int y = 6 + cellHeight; y < 6 + 2 * cellHeight; ++y)
    {
        for (int x = 6 + cellWidth; x < 6 + 2 * cellWidth; ++x)
        {
            const QColor pixel = image.pixelColor(x, y);
            if (pixel.green() > 80 && pixel.blue() > 80)
            {
                wideGlyphReachedTail = true;
                break;
            }
        }
        if (wideGlyphReachedTail)
        {
            break;
        }
    }
    if (!wideGlyphReachedTail)
    {
        return 6;
    }

    QByteArray pastedBytes;
    int pasteCount = 0;
    int mouseInputCount = 0;
    QObject::connect(&widget, &TerminalWidget::pasteInput,
        [&](const QByteArray &bytes) {
            pastedBytes = bytes;
            ++pasteCount;
        });
    QObject::connect(&widget, &TerminalWidget::mouseInput,
        [&](const TerminalMouseEvent &) { ++mouseInputCount; });
    QApplication::clipboard()->setText(QStringLiteral("right-click-paste"));
    mutableSnapshot->mouseTracking = true;
    mutableSnapshot->alternateScreen = true;
    widget.setSnapshot(mutableSnapshot);
    if (scrollBar->isVisible())
    {
        return 11;
    }
    QMouseEvent rightPress(QEvent::MouseButtonPress,
        QPointF(10, 10), QPointF(10, 10),
        Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &rightPress);
    QMouseEvent rightRelease(QEvent::MouseButtonRelease,
        QPointF(10, 10), QPointF(10, 10),
        Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &rightRelease);
    if (pasteCount != 1 || pastedBytes != QByteArray("right-click-paste")
        || mouseInputCount != 0)
    {
        return 7;
    }

    std::cout << "[PASS] terminal widget input, IME, resize, paint and right-click paste checks\n";
    return 0;
}
