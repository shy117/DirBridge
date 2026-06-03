#ifndef XFOLDER_UI_MAINWINDOW_H
#define XFOLDER_UI_MAINWINDOW_H

#include <QMainWindow>

#include "core/CurlProtocolCheck.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const CurlProtocolCheckResult &curlCheck, QWidget *parent = nullptr);
};

#endif // XFOLDER_UI_MAINWINDOW_H
