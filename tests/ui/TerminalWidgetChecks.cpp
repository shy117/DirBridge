#include "ui/TerminalWidget.h"

#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QTimer>

#include <iostream>
#include <memory>

using namespace dirbridge::terminal;

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    TerminalWidget widget;
    widget.resize(480, 240);

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
    mutableSnapshot->cursor.visible = true;
    mutableSnapshot->cursor.column = 2;
    widget.setSnapshot(mutableSnapshot);

    QByteArray textBytes;
    TerminalKeyEvent key;
    bool sawKey = false;
    bool sawResize = false;
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

    widget.show();
    widget.resize(500, 260);
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

    std::cout << "[PASS] terminal widget input, IME, resize and paint checks\n";
    return 0;
}
