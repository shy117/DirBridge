#include "ui/FilePanel.h"

FilePanel::FilePanel(Mode mode, QWidget *parent)
    : QWidget(parent)
    , m_mode(mode)
{
    setupUi();
    connectSignals();
    initialize();
}
