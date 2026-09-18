#ifndef DIRBRIDGE_UI_TERMINALWIDGET_H
#define DIRBRIDGE_UI_TERMINALWIDGET_H

#include "terminal/TerminalTypes.h"

#include <QByteArray>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QWidget>

class QInputMethodEvent;
class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QScrollBar;
class QWheelEvent;

class TerminalWidget final : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);

    void setSnapshot(dirbridge::terminal::TerminalSnapshotPtr snapshot);
    void setStatus(const QString &message, bool error = false);
    bool hasSelection() const noexcept;
    void clearSelection();

Q_SIGNALS:
    void keyInput(const dirbridge::terminal::TerminalKeyEvent &event);
    void textInput(const QByteArray &utf8);
    void pasteInput(const QByteArray &utf8);
    void mouseInput(const dirbridge::terminal::TerminalMouseEvent &event);
    void scrollRequested(int lines);
    void selectionInput(const dirbridge::terminal::TerminalSelectionEvent &event);
    void copyRequested();
    void resizeRequested(const dirbridge::terminal::TerminalGeometry &geometry);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct CellPoint
    {
        int column = 0;
        int row = 0;
    };

    void updateMetrics();
    void layoutScrollBar();
    void updateScrollBar();
    void scrollToBottom();
    int terminalContentWidth() const;
    void emitCurrentGeometry();
    void pasteClipboard();
    void copySelection();
    void sendSelection(dirbridge::terminal::TerminalSelectionAction action, int lines = 0);
    void stopSelecting();
    void updateSelectionScroll();
    CellPoint cellAt(const QPointF &position) const;
    bool isSelected(int column, int row) const;
    dirbridge::terminal::TerminalMouseEvent mouseEvent(
        QMouseEvent *event,
        dirbridge::terminal::TerminalMouseAction action) const;

    dirbridge::terminal::TerminalSnapshotPtr m_snapshot;
    QScrollBar *m_scrollBar = nullptr;
    QTimer m_resizeTimer;
    QString m_status;
    QString m_preedit;
    bool m_statusError = false;
    bool m_selecting = false;
    bool m_hasSelection = false;
    QPointF m_selectionPosition;
    QPointF m_selectionStartPosition;
    QTimer m_selectionScrollTimer;
    int m_selectionScrollDirection = 0;
    std::uint64_t m_selectionRequestId = 0;
    int m_cellWidth = 8;
    int m_cellHeight = 16;
    int m_ascent = 12;
    int m_margin = 6;
    qint64 m_requestedScrollOffset = 0;
};

#endif // DIRBRIDGE_UI_TERMINALWIDGET_H
