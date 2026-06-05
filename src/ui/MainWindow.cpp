#include "ui/MainWindow.h"

#include "logging/AppLogger.h"
#include "ui/FilePanel.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <spdlog/spdlog.h>

namespace
{
QString protocolText(RemoteProtocol protocol)
{
    return QString::fromStdString(toString(protocol)).toUpper();
}

std::string makeSiteId(const SiteProfile &profile)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    std::string host = profile.host.empty() ? "site" : profile.host;
    std::replace(host.begin(), host.end(), '.', '-');
    std::replace(host.begin(), host.end(), ':', '-');
    return toString(profile.protocol) + "-" + host + "-" + std::to_string(millis);
}

int findProtocolIndex(QComboBox *combo, RemoteProtocol protocol)
{
    return combo->findText(protocolText(protocol), Qt::MatchFixedString);
}
}

MainWindow::MainWindow(const DependencyCheckResult &dependencyCheck, QWidget *parent)
    : QMainWindow(parent)
    , m_siteStore(dependencyCheck.siteConfigPath.empty() ? std::filesystem::path("config") / "sites.json" : dependencyCheck.siteConfigPath)
    , m_remoteFileSystem(std::make_unique<FakeRemoteFileSystem>())
{
    setWindowTitle("DirBridge - Remote Folder Manager");
    resize(1380, 820);

    loadSites();
    setupMenuBar();
    setupToolBar();
    setupQuickConnectBar();
    setupCentralWorkspace(dependencyCheck);
    populateSessionManager();

    appendLog("INFO", "DirBridge UI started");
    appendLog("INFO", QString("site config: %1").arg(QString::fromStdString(m_siteStore.path().string())));
    appendLog("INFO", QString("libcurl ready=%1, JSON ready=%2, logging ready=%3")
        .arg(dependencyCheck.curl.hasFtp && dependencyCheck.curl.hasSftp ? "yes" : "no")
        .arg(dependencyCheck.jsonReady ? "yes" : "no")
        .arg(dependencyCheck.loggingReady && dependencyCheck.siteStoreReady ? "yes" : "no"));

    statusBar()->showMessage(
        QString("libcurl ready=%1, JSON ready=%2, logging ready=%3")
            .arg(dependencyCheck.curl.hasFtp && dependencyCheck.curl.hasSftp ? "yes" : "no")
            .arg(dependencyCheck.jsonReady ? "yes" : "no")
            .arg(dependencyCheck.loggingReady && dependencyCheck.siteStoreReady ? "yes" : "no"));
}

void MainWindow::loadSites()
{
    try
    {
        m_sites = m_siteStore.load();
    }
    catch (const std::exception &error)
    {
        m_sites.clear();
        appendLog("ERROR", QString("加载站点配置失败：%1").arg(error.what()));
    }
}

void MainWindow::saveSites()
{
    m_siteStore.save(m_sites);
    populateSessionManager();
}

void MainWindow::setupMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu("文件(&F)");
    fileMenu->addAction("新建会话");
    fileMenu->addAction("保存快速连接", this, [this]() {
        connectQuickProfile(true);
    });
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
    menuBar()->addMenu("帮助(&H)")->addAction("关于 DirBridge");
}

void MainWindow::setupToolBar()
{
    QToolBar *toolbar = addToolBar("主工具栏");
    toolbar->setMovable(false);
    toolbar->addAction("新建");
    toolbar->addAction("连接", this, [this]() {
        connectQuickProfile(false);
    });
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

    m_protocolCombo = new QComboBox(quickBar);
    m_protocolCombo->addItems({"SFTP", "FTP", "FTPS"});

    m_hostEdit = new QLineEdit(quickBar);
    m_hostEdit->setPlaceholderText("主机地址");

    m_portEdit = new QLineEdit(quickBar);
    m_portEdit->setPlaceholderText("端口");
    m_portEdit->setFixedWidth(64);
    m_portEdit->setText(QString::number(defaultPortForProtocol(RemoteProtocol::Sftp)));

    m_userEdit = new QLineEdit(quickBar);
    m_userEdit->setPlaceholderText("用户名");

    m_passwordEdit = new QLineEdit(quickBar);
    m_passwordEdit->setPlaceholderText("密码");
    m_passwordEdit->setEchoMode(QLineEdit::Password);

    m_connectButton = new QPushButton("连接", quickBar);
    m_saveSiteButton = new QPushButton("保存站点", quickBar);

    quickBar->addWidget(new QLabel("协议", quickBar));
    quickBar->addWidget(m_protocolCombo);
    quickBar->addWidget(new QLabel("主机", quickBar));
    quickBar->addWidget(m_hostEdit);
    quickBar->addWidget(new QLabel("端口", quickBar));
    quickBar->addWidget(m_portEdit);
    quickBar->addWidget(new QLabel("用户", quickBar));
    quickBar->addWidget(m_userEdit);
    quickBar->addWidget(new QLabel("密码", quickBar));
    quickBar->addWidget(m_passwordEdit);
    quickBar->addWidget(m_connectButton);
    quickBar->addWidget(m_saveSiteButton);

    connect(m_protocolCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        const RemoteProtocol protocol = remoteProtocolFromString(text.toStdString());
        m_portEdit->setText(QString::number(defaultPortForProtocol(protocol)));
    });
    connect(m_connectButton, &QPushButton::clicked, this, [this]() {
        connectQuickProfile(false);
    });
    connect(m_saveSiteButton, &QPushButton::clicked, this, [this]() {
        connectQuickProfile(true);
    });
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
    m_remotePanel = new FilePanel(FilePanel::Mode::RemotePlaceholder, remoteTabs);
    m_remotePanel->setRemoteSummary(
        QString::fromStdString(dependencyCheck.curl.version),
        dependencyCheck.curl.hasFtp,
        dependencyCheck.curl.hasSftp);
    remoteTabs->addTab(m_remotePanel, "远程：未连接");

    fileSplitter->addWidget(localTabs);
    fileSplitter->addWidget(remoteTabs);
    fileSplitter->setStretchFactor(0, 1);
    fileSplitter->setStretchFactor(1, 1);

    auto *bottomTabs = new QTabWidget(verticalSplitter);
    auto *transferTable = new QTreeWidget(bottomTabs);
    transferTable->setHeaderLabels({"名称", "状态", "进度", "大小", "本地路径", "<->", "远程路径", "速度", "剩余时间"});
    transferTable->header()->setStretchLastSection(true);

    m_logView = new QTreeWidget(bottomTabs);
    m_logView->setHeaderLabels({"时间", "级别", "消息"});
    m_logView->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_logView->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_logView->header()->setStretchLastSection(true);

    bottomTabs->addTab(transferTable, "传输");
    bottomTabs->addTab(m_logView, "日志");

    verticalSplitter->addWidget(fileSplitter);
    verticalSplitter->addWidget(bottomTabs);
    verticalSplitter->setStretchFactor(0, 5);
    verticalSplitter->setStretchFactor(1, 1);

    setCentralWidget(verticalSplitter);
}

QTreeWidget *MainWindow::createSessionManager()
{
    m_sessionTree = new QTreeWidget(this);
    m_sessionTree->setHeaderHidden(true);
    connect(m_sessionTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        fillQuickConnectFromItem(item);
        if (item != nullptr && item->data(0, Qt::UserRole).isValid())
        {
            showRemoteProfile(m_sites.at(item->data(0, Qt::UserRole).toInt()));
        }
    });
    return m_sessionTree;
}

void MainWindow::populateSessionManager()
{
    if (m_sessionTree == nullptr)
    {
        return;
    }

    m_sessionTree->clear();
    auto *root = new QTreeWidgetItem(m_sessionTree, {"所有会话"});
    for (int index = 0; index < static_cast<int>(m_sites.size()); ++index)
    {
        const SiteProfile &profile = m_sites.at(index);
        auto *item = new QTreeWidgetItem(root, {siteDisplayName(profile)});
        item->setData(0, Qt::UserRole, index);
        item->setToolTip(0, QString("%1://%2:%3")
            .arg(QString::fromStdString(toString(profile.protocol)))
            .arg(QString::fromStdString(profile.host))
            .arg(profile.port));
    }
    root->setExpanded(true);
}

void MainWindow::appendLog(const QString &level, const QString &message)
{
    if (m_logView != nullptr)
    {
        auto *item = new QTreeWidgetItem(m_logView, {
            QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"),
            level,
            message
        });
        m_logView->addTopLevelItem(item);
        m_logView->scrollToItem(item);
    }

    auto logger = AppLogger::get();
    if (logger == nullptr)
    {
        return;
    }

    const std::string text = message.toStdString();
    if (level == "ERROR")
    {
        logger->error(text);
    }
    else if (level == "WARN")
    {
        logger->warn(text);
    }
    else
    {
        logger->info(text);
    }
    logger->flush();
}

SiteProfile MainWindow::profileFromQuickConnect() const
{
    SiteProfile profile;
    profile.protocol = remoteProtocolFromString(m_protocolCombo->currentText().toStdString());
    profile.host = m_hostEdit->text().trimmed().toStdString();
    profile.port = static_cast<std::uint16_t>(m_portEdit->text().toUShort());
    if (profile.port == 0)
    {
        profile.port = defaultPortForProtocol(profile.protocol);
    }
    profile.username = m_userEdit->text().trimmed().toStdString();
    profile.password = m_passwordEdit->text().toStdString();
    profile.defaultRemotePath = "/";
    profile.encoding = "UTF-8";
    profile.name = profile.host.empty()
        ? std::string("未命名站点")
        : protocolText(profile.protocol).toStdString() + " " + profile.host;
    profile.id = makeSiteId(profile);
    return profile;
}

void MainWindow::connectQuickProfile(bool saveProfile)
{
    try
    {
        SiteProfile profile = profileFromQuickConnect();
        if (profile.host.empty())
        {
            QMessageBox::warning(this, "连接信息不完整", "请输入主机地址。");
            appendLog("WARN", "快速连接缺少主机地址");
            return;
        }

        if (saveProfile)
        {
            const auto sameSite = [&profile](const SiteProfile &site) {
                return site.protocol == profile.protocol
                    && site.host == profile.host
                    && site.port == profile.port
                    && site.username == profile.username;
            };
            auto existing = std::find_if(m_sites.begin(), m_sites.end(), sameSite);
            if (existing == m_sites.end())
            {
                m_sites.push_back(profile);
            }
            else
            {
                profile.id = existing->id;
                *existing = profile;
            }
            saveSites();
            appendLog("INFO", QString("已保存站点：%1").arg(siteDisplayName(profile)));
        }

        showRemoteProfile(profile);
    }
    catch (const std::exception &error)
    {
        appendLog("ERROR", QString("快速连接失败：%1").arg(error.what()));
        QMessageBox::critical(this, "快速连接失败", error.what());
    }
}

void MainWindow::showRemoteProfile(const SiteProfile &profile)
{
    appendLog("INFO", QString("连接远程站点：%1").arg(siteDisplayName(profile)));

    const RemoteOperationResult result = m_remoteFileSystem->connect(profile);
    if (!result.success)
    {
        appendLog("ERROR", QString("连接失败：%1").arg(QString::fromStdString(result.message)));
        QMessageBox::warning(this, "连接失败", QString::fromStdString(result.message));
        return;
    }

    const std::vector<FileItem> items = m_remoteFileSystem->listDirectory(profile.defaultRemotePath);
    m_remotePanel->setRemoteItems(
        QString::fromStdString(profile.defaultRemotePath),
        items,
        QString("mock 远程会话已连接：%1，%2 个项目")
            .arg(siteDisplayName(profile))
            .arg(items.size()));
    appendLog("INFO", QString("远程目录已加载：%1").arg(QString::fromStdString(profile.defaultRemotePath)));
}

void MainWindow::fillQuickConnectFromItem(QTreeWidgetItem *item)
{
    if (item == nullptr || !item->data(0, Qt::UserRole).isValid())
    {
        return;
    }

    const int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= static_cast<int>(m_sites.size()))
    {
        return;
    }

    const SiteProfile &profile = m_sites.at(index);
    const int protocolIndex = findProtocolIndex(m_protocolCombo, profile.protocol);
    if (protocolIndex >= 0)
    {
        m_protocolCombo->setCurrentIndex(protocolIndex);
    }
    m_hostEdit->setText(QString::fromStdString(profile.host));
    m_portEdit->setText(QString::number(profile.port));
    m_userEdit->setText(QString::fromStdString(profile.username));
    m_passwordEdit->setText(QString::fromStdString(profile.password));
    appendLog("INFO", QString("已填充站点：%1").arg(siteDisplayName(profile)));
}

QString MainWindow::siteDisplayName(const SiteProfile &profile) const
{
    const QString name = QString::fromStdString(profile.name);
    if (!name.trimmed().isEmpty())
    {
        return name;
    }

    return QString("%1 %2").arg(protocolText(profile.protocol), QString::fromStdString(profile.host));
}
