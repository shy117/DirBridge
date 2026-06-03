#include "ui/MainWindow.h"

#include <QLabel>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(const CurlProtocolCheckResult &curlCheck, QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("XFolder");
    resize(1200, 720);

    auto *central = new QWidget(this);
    auto *layout = new QVBoxLayout(central);

    auto *title = new QLabel("XFolder", central);
    QFont titleFont = title->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);

    auto *summary = new QLabel(
        QString("Qt Widgets shell is running. libcurl %1 supports FTP=%2, SFTP=%3.")
            .arg(curlCheck.version)
            .arg(curlCheck.hasFtp ? "yes" : "no")
            .arg(curlCheck.hasSftp ? "yes" : "no"),
        central);

    layout->addWidget(title);
    layout->addWidget(summary);
    layout->addStretch();

    setCentralWidget(central);
    statusBar()->showMessage("Ready");
}
