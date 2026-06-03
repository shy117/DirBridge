#ifndef XFOLDER_UI_MAINWINDOW_H
#define XFOLDER_UI_MAINWINDOW_H

#include <QMainWindow>

#include "core/CurlProtocolCheck.h"

class QComboBox;
class QLineEdit;
class QTreeWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const CurlProtocolCheckResult &curlCheck, QWidget *parent = nullptr);

private:
    void setupMenuBar();
    void setupToolBar();
    void setupQuickConnectBar();
    void setupCentralWorkspace(const CurlProtocolCheckResult &curlCheck);
    QTreeWidget *createSessionManager();
};

#endif // XFOLDER_UI_MAINWINDOW_H
