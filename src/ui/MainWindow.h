#ifndef DIRBRIDGE_UI_MAINWINDOW_H
#define DIRBRIDGE_UI_MAINWINDOW_H

#include <QMainWindow>

#include "core/DependencyCheck.h"

class QComboBox;
class QLineEdit;
class QTreeWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const DependencyCheckResult &dependencyCheck, QWidget *parent = nullptr);

private:
    void setupMenuBar();
    void setupToolBar();
    void setupQuickConnectBar();
    void setupCentralWorkspace(const DependencyCheckResult &dependencyCheck);
    QTreeWidget *createSessionManager();
};

#endif // DIRBRIDGE_UI_MAINWINDOW_H
