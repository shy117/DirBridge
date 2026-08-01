#include "ui/MainWindow.h"

#include "terminal/SshTerminalManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QLabel>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <filesystem>

using dirbridge::terminal::SshTerminalManager;
using dirbridge::terminal::defaultSshTerminalRuntimePaths;

void MainWindow::setupSshTerminalManager()
{
    m_sshTerminalManager = std::make_unique<SshTerminalManager>(
        defaultSshTerminalRuntimePaths(
            std::filesystem::path(
                QCoreApplication::applicationDirPath().toStdWString()),
            std::filesystem::path(QDir::currentPath().toStdWString())),
        this);

    connect(
        m_sshTerminalManager.get(),
        &SshTerminalManager::sessionReady,
        this,
        [this](const QString &terminalId) {
            const auto found = m_terminalUiSessions.find(terminalId);
            if (found != m_terminalUiSessions.end())
            {
                found->second.statusLabel->setText(
                    "SSH 已连接，等待终端绘制层接管输出。");
            }
        });
    connect(
        m_sshTerminalManager.get(),
        &SshTerminalManager::sessionOutput,
        this,
        [this](const QString &terminalId, const QByteArray &) {
            const auto found = m_terminalUiSessions.find(terminalId);
            if (found != m_terminalUiSessions.end()
                && !found->second.receivedOutput)
            {
                found->second.receivedOutput = true;
                found->second.statusLabel->setText(
                    "SSH 会话正在运行；输出未写入日志，等待正式 Terminal Engine 渲染。");
            }
        });
    connect(
        m_sshTerminalManager.get(),
        &SshTerminalManager::sessionExited,
        this,
        [this](const QString &terminalId, quint32 exitCode, bool closeRequested) {
            const auto found = m_terminalUiSessions.find(terminalId);
            if (found == m_terminalUiSessions.end())
            {
                return;
            }
            found->second.statusLabel->setText(closeRequested
                ? "SSH 终端正在关闭…"
                : QString("SSH 进程已退出（代码 %1）。").arg(exitCode));
        });
    connect(
        m_sshTerminalManager.get(),
        &SshTerminalManager::sessionError,
        this,
        [this](const QString &terminalId, const QString &message) {
            const auto found = m_terminalUiSessions.find(terminalId);
            if (found != m_terminalUiSessions.end())
            {
                found->second.statusLabel->setText(
                    QString("SSH 终端错误：%1").arg(message));
            }
        });
    connect(
        m_sshTerminalManager.get(),
        &SshTerminalManager::sessionStopped,
        this,
        [this](const QString &terminalId) {
            const auto found = m_terminalUiSessions.find(terminalId);
            if (found == m_terminalUiSessions.end())
            {
                return;
            }
            found->second.active = false;
            found->second.statusLabel->setText("SSH 终端已停止。");
            if (found->second.closeRequested)
            {
                removeTerminalTab(terminalId);
            }
        });
    connect(
        m_sshTerminalManager.get(),
        &SshTerminalManager::allSessionsStopped,
        this,
        [this]() {
            if (m_closePending && m_activeBackgroundTaskCount == 0)
            {
                QTimer::singleShot(0, this, [this]() {
                    close();
                });
            }
        });
}

void MainWindow::openSshTerminal(const SiteProfile &profile)
{
    if (profile.protocol != RemoteProtocol::Sftp
        || m_sshTerminalManager == nullptr
        || m_terminalTabs == nullptr
        || m_bottomTabs == nullptr)
    {
        showWarningMessage(
            "无法打开 SSH 终端",
            "只有 SFTP 站点支持 SSH 终端。");
        return;
    }

    const QString terminalId = m_sshTerminalManager->openSession(profile);
    if (terminalId.isEmpty())
    {
        showCriticalMessage(
            "SSH 终端启动失败",
            m_sshTerminalManager->lastError());
        return;
    }

    auto *page = new QWidget(m_terminalTabs);
    page->setObjectName("sshTerminalPage");
    page->setProperty("terminalId", terminalId);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 12, 12, 12);
    auto *status = new QLabel("正在启动 SSH 终端…", page);
    status->setObjectName("sshTerminalStatus");
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status->setWordWrap(true);
    auto *boundary = new QLabel(
        "会话进程和安全通道已接入；正式终端内容将在 Terminal Engine 与自定义绘制层接入后显示。",
        page);
    boundary->setObjectName("sshTerminalRenderBoundary");
    boundary->setWordWrap(true);
    layout->addWidget(status);
    layout->addWidget(boundary);
    layout->addStretch(1);

    TerminalTab terminalTab;
    terminalTab.page = page;
    terminalTab.statusLabel = status;
    m_terminalUiSessions.emplace(terminalId, terminalTab);
    const QString title = QString::fromStdString(
        profile.name.empty() ? profile.host : profile.name);
    const int index = m_terminalTabs->addTab(page, title);
    m_terminalTabs->setCurrentIndex(index);
    m_bottomTabs->setCurrentWidget(m_terminalTabs->parentWidget());
}

void MainWindow::closeTerminalTab(int index)
{
    if (m_terminalTabs == nullptr
        || index < 0
        || index >= m_terminalTabs->count())
    {
        return;
    }
    QWidget *page = m_terminalTabs->widget(index);
    const QString terminalId = page == nullptr
        ? QString()
        : page->property("terminalId").toString();
    const auto found = m_terminalUiSessions.find(terminalId);
    if (found == m_terminalUiSessions.end())
    {
        return;
    }
    if (!found->second.active)
    {
        removeTerminalTab(terminalId);
        return;
    }
    found->second.closeRequested = true;
    found->second.statusLabel->setText("正在关闭 SSH 终端…");
    if (!m_sshTerminalManager->requestClose(terminalId))
    {
        found->second.statusLabel->setText(
            QString("关闭失败：%1").arg(m_sshTerminalManager->lastError()));
    }
}

void MainWindow::removeTerminalTab(const QString &terminalId)
{
    const auto found = m_terminalUiSessions.find(terminalId);
    if (found == m_terminalUiSessions.end())
    {
        return;
    }
    QWidget *page = found->second.page;
    const int index = m_terminalTabs == nullptr
        ? -1
        : m_terminalTabs->indexOf(page);
    if (index >= 0)
    {
        m_terminalTabs->removeTab(index);
    }
    m_terminalUiSessions.erase(found);
    if (page != nullptr)
    {
        page->deleteLater();
    }
}
