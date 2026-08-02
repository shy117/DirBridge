#include "ui/MainWindow.h"

#include "terminal/SshTerminalManager.h"
#include "ui/TerminalWidget.h"

#include <QCoreApplication>
#include <QDir>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <filesystem>

using dirbridge::terminal::SshTerminalManager;
using dirbridge::terminal::TerminalGeometry;
using dirbridge::terminal::TerminalKeyEvent;
using dirbridge::terminal::TerminalMouseEvent;
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
                found->second.terminal->setStatus(
                    "SSH 进程已启动，正在等待远端输出。");
            }
        });
    connect(
        m_sshTerminalManager.get(),
        &SshTerminalManager::sessionSnapshot,
        this,
        [this](const QString &terminalId,
            const dirbridge::terminal::TerminalSnapshotPtr &snapshot) {
            const auto found = m_terminalUiSessions.find(terminalId);
            if (found != m_terminalUiSessions.end())
            {
                found->second.terminal->setSnapshot(snapshot);
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
            found->second.terminal->setStatus(closeRequested
                    ? "SSH 终端正在关闭…"
                    : QString("SSH 进程已退出（代码 %1）。").arg(exitCode),
                !closeRequested && exitCode != 0);
        });
    connect(
        m_sshTerminalManager.get(),
        &SshTerminalManager::sessionError,
        this,
        [this](const QString &terminalId, const QString &message) {
            const auto found = m_terminalUiSessions.find(terminalId);
            if (found != m_terminalUiSessions.end())
            {
                found->second.terminal->setStatus(
                    QString("SSH 终端错误：%1").arg(message), true);
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
            found->second.terminal->setStatus("SSH 终端已停止。");
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
    layout->setContentsMargins(0, 0, 0, 0);
    auto *terminal = new TerminalWidget(page);
    terminal->setStatus("正在启动 SSH 终端…");
    layout->addWidget(terminal);

    connect(terminal, &TerminalWidget::keyInput, this,
        [this, terminalId](const TerminalKeyEvent &event) {
            m_sshTerminalManager->sendKey(terminalId, event);
        });
    connect(terminal, &TerminalWidget::textInput, this,
        [this, terminalId](const QByteArray &utf8) {
            m_sshTerminalManager->sendText(terminalId, utf8);
        });
    connect(terminal, &TerminalWidget::pasteInput, this,
        [this, terminalId](const QByteArray &utf8) {
            m_sshTerminalManager->sendPaste(terminalId, utf8);
        });
    connect(terminal, &TerminalWidget::mouseInput, this,
        [this, terminalId](const TerminalMouseEvent &event) {
            m_sshTerminalManager->sendMouse(terminalId, event);
        });
    connect(terminal, &TerminalWidget::scrollRequested, this,
        [this, terminalId](int lines) {
            m_sshTerminalManager->scrollLines(terminalId, lines);
        });
    connect(terminal, &TerminalWidget::resizeRequested, this,
        [this, terminalId](const TerminalGeometry &geometry) {
            m_sshTerminalManager->resize(terminalId, geometry);
        });

    TerminalTab terminalTab;
    terminalTab.page = page;
    terminalTab.terminal = terminal;
    m_terminalUiSessions.emplace(terminalId, terminalTab);
    const QString title = QString::fromStdString(
        profile.name.empty() ? profile.host : profile.name);
    const int index = m_terminalTabs->addTab(page, title);
    m_terminalTabs->setCurrentIndex(index);
    m_bottomTabs->setCurrentWidget(m_terminalTabs->parentWidget());
    terminal->setFocus(Qt::OtherFocusReason);
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
    found->second.terminal->setStatus("正在关闭 SSH 终端…");
    if (!m_sshTerminalManager->requestClose(terminalId))
    {
        found->second.terminal->setStatus(
            QString("关闭失败：%1").arg(m_sshTerminalManager->lastError()),
            true);
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
    if (m_terminalTabs != nullptr && m_terminalTabs->count() == 0)
    {
        setTerminalWorkspaceMaximized(false);
    }
}
