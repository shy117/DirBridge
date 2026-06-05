#include "ui/MainWindow.h"

#include "ui/FilePanel.h"

#include <QAction>
#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

MainWindow::MainWindow(const DependencyCheckResult &dependencyCheck, QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("XFolder - Remote Folder Manager");
    resize(1380, 820);

    setupMenuBar();
    setupToolBar();
    setupQuickConnectBar();
    setupCentralWorkspace(dependencyCheck);

    statusBar()->showMessage(
        QString("libcurl ready=%1, JSON ready=%2, logging ready=%3")
            .arg(dependencyCheck.curl.hasFtp && dependencyCheck.curl.hasSftp ? "yes" : "no")
            .arg(dependencyCheck.jsonReady ? "yes" : "no")
            .arg(dependencyCheck.loggingReady ? "yes" : "no"));
}

void MainWindow::setupMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu("文件(&F)");
    fileMenu->addAction("新建会话");
    fileMenu->addAction("打开会话");
    fileMenu->addSeparator();
    fileMenu->addAction("退出", this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu("编辑(&E)");
    editMenu->addAction("复制");
    editMenu->addAction("粘贴");
    editMenu->addAction("删除");
    editMenu->addAction("重命名");

    QMenu *viewMenu = menuBar()->addMenu("查看(&V)");
    viewMenu->addAction("刷新");
    viewMenu->addAction("显示隐藏文件");

    QMenu *commandMenu = menuBar()->addMenu("命令(&C)");
    commandMenu->addAction("上传");
    commandMenu->addAction("下载");

    menuBar()->addMenu("工具(&T)")->addAction("选项");
    menuBar()->addMenu("窗口(&W)")->addAction("关闭当前标签");
    menuBar()->addMenu("帮助(&H)")->addAction("关于 XFolder");
}

void MainWindow::setupToolBar()
{
    QToolBar *toolbar = addToolBar("主工具栏");
    toolbar->setMovable(false);
    toolbar->addAction("新建");
    toolbar->addAction("连接");
    toolbar->addAction("断开");
    toolbar->addSeparator();
    toolbar->addAction("上传");
    toolbar->addAction("下载");
    toolbar->addSeparator();
    toolbar->addAction("刷新");
    toolbar->addAction("设置");
}

void MainWindow::setupQuickConnectBar()
{
    QToolBar *quickBar = addToolBar("快速连接");
    quickBar->setMovable(false);

    auto *protocol = new QComboBox(quickBar);
    protocol->addItems({"SFTP", "FTP", "FTPS"});
    auto *host = new QLineEdit(quickBar);
    host->setPlaceholderText("主机地址");
    auto *user = new QLineEdit(quickBar);
    user->setPlaceholderText("用户名");
    auto *password = new QLineEdit(quickBar);
    password->setPlaceholderText("密码");
    password->setEchoMode(QLineEdit::Password);
    auto *connectButton = new QPushButton("连接", quickBar);

    quickBar->addWidget(new QLabel("协议", quickBar));
    quickBar->addWidget(protocol);
    quickBar->addWidget(new QLabel("主机", quickBar));
    quickBar->addWidget(host);
    quickBar->addWidget(new QLabel("用户", quickBar));
    quickBar->addWidget(user);
    quickBar->addWidget(new QLabel("密码", quickBar));
    quickBar->addWidget(password);
    quickBar->addWidget(connectButton);
}

void MainWindow::setupCentralWorkspace(const DependencyCheckResult &dependencyCheck)
{
    auto *sessionDock = new QDockWidget("Session Manager", this);
    sessionDock->setObjectName("SessionManagerDock");
    sessionDock->setWidget(createSessionManager());
    addDockWidget(Qt::LeftDockWidgetArea, sessionDock);

    auto *verticalSplitter = new QSplitter(Qt::Vertical, this);
    auto *fileSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);

    auto *localTabs = new QTabWidget(fileSplitter);
    auto *localPanel = new FilePanel(FilePanel::Mode::Local, localTabs);
    localTabs->addTab(localPanel, "本地：桌面");

    auto *remoteTabs = new QTabWidget(fileSplitter);
    auto *remotePanel = new FilePanel(FilePanel::Mode::RemotePlaceholder, remoteTabs);
    remotePanel->setRemoteSummary(
        QString::fromStdString(dependencyCheck.curl.version),
        dependencyCheck.curl.hasFtp,
        dependencyCheck.curl.hasSftp);
    remoteTabs->addTab(remotePanel, "远程：未连接");

    fileSplitter->addWidget(localTabs);
    fileSplitter->addWidget(remoteTabs);
    fileSplitter->setStretchFactor(0, 1);
    fileSplitter->setStretchFactor(1, 1);

    auto *bottomTabs = new QTabWidget(verticalSplitter);
    auto *transferTable = new QTreeWidget(bottomTabs);
    transferTable->setHeaderLabels({"名称", "状态", "进度", "大小", "本地路径", "<->", "远程路径", "速度", "剩余时间"});
    auto *logView = new QTreeWidget(bottomTabs);
    logView->setHeaderLabels({"时间", "级别", "消息"});
    bottomTabs->addTab(transferTable, "传输");
    bottomTabs->addTab(logView, "日志");

    verticalSplitter->addWidget(fileSplitter);
    verticalSplitter->addWidget(bottomTabs);
    verticalSplitter->setStretchFactor(0, 5);
    verticalSplitter->setStretchFactor(1, 1);

    setCentralWidget(verticalSplitter);
}

QTreeWidget *MainWindow::createSessionManager()
{
    auto *tree = new QTreeWidget(this);
    tree->setHeaderHidden(true);

    auto *root = new QTreeWidgetItem(tree, {"所有会话"});
    new QTreeWidgetItem(root, {"示例 SFTP 会话"});
    new QTreeWidgetItem(root, {"示例 FTP 会话"});
    root->setExpanded(true);

    return tree;
}
