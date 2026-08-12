#include "ui/MainWindow.h"

#include "terminal/SshTerminalManager.h"
#include "ui/TerminalWidget.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
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

namespace
{
QString knownHostsLookupTarget(const SiteProfile &profile)
{
    const QString host = QString::fromStdString(profile.host);
    return profile.port == 0 || profile.port == 22
        ? host
        : QString("[%1]:%2").arg(host).arg(profile.port);
}

QString locateSshKeygen()
{
    QString path = QStandardPaths::findExecutable("ssh-keygen.exe");
    if (!path.isEmpty())
    {
        return path;
    }
    const QString systemRoot = qEnvironmentVariable("SystemRoot");
    const QString candidate = QDir(systemRoot).filePath("System32/OpenSSH/ssh-keygen.exe");
    return QFileInfo(candidate).isFile() ? candidate : QString();
}
}

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
            if (found->second.hostKeyConflictDetected)
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
        &SshTerminalManager::hostKeyConflictDetected,
        this,
        [this](const QString &terminalId, const QString &fingerprint) {
            const auto found = m_terminalUiSessions.find(terminalId);
            if (found != m_terminalUiSessions.end())
            {
                found->second.hostKeyConflictDetected = true;
                found->second.hostKeyFingerprint = fingerprint;
                found->second.terminal->setStatus(
                    "SSH 主机密钥已变化，连接已停止。请确认设备身份后再恢复。",
                    true);
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
            if (!found->second.hostKeyConflictDetected)
            {
                found->second.terminal->setStatus("SSH 终端已停止。");
            }
            if (found->second.closeRequested)
            {
                removeTerminalTab(terminalId);
            }
            else if (found->second.hostKeyConflictDetected)
            {
                QTimer::singleShot(0, this, [this, terminalId]() {
                    handleSshHostKeyConflict(terminalId);
                });
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
    terminalTab.profile = profile;
    m_terminalUiSessions.emplace(terminalId, terminalTab);
    const QString title = QString::fromStdString(
        profile.name.empty() ? profile.host : profile.name);
    const int index = m_terminalTabs->addTab(page, title);
    m_terminalTabs->setCurrentIndex(index);
    m_bottomTabs->setCurrentWidget(m_terminalTabs->parentWidget());
    terminal->setFocus(Qt::OtherFocusReason);
}

void MainWindow::handleSshHostKeyConflict(const QString &terminalId)
{
    const auto found = m_terminalUiSessions.find(terminalId);
    if (found == m_terminalUiSessions.end())
    {
        return;
    }
    const SiteProfile profile = found->second.profile;
    const QString fingerprint = found->second.hostKeyFingerprint;
    const QString target = knownHostsLookupTarget(profile);

    QMessageBox dialog(this);
    dialog.setWindowTitle("SSH 主机密钥已变化");
    dialog.setIcon(QMessageBox::Warning);
    dialog.setText("远程设备提供的 SSH 主机密钥与本机旧记录不一致。");
    QString detail = QString(
        "主机：%1\n新指纹：%2\n\n"
        "这可能是设备重装或密钥更新，也可能是中间人攻击。"
        "请先通过可信渠道核对新指纹。").arg(
            target,
            fingerprint.isEmpty() ? "未能从 OpenSSH 输出中提取" : fingerprint);
    if (fingerprint.isEmpty())
    {
        detail += "\n\n为避免误删可信记录，请手动执行 ssh-keygen -R 并重新连接。";
    }
    dialog.setInformativeText(detail);
    QPushButton *replaceButton = nullptr;
    if (!fingerprint.isEmpty())
    {
        replaceButton = dialog.addButton(
            "确认设备可信并替换旧记录",
            QMessageBox::AcceptRole);
    }
    dialog.addButton("取消", QMessageBox::RejectRole);
    dialog.exec();
    if (replaceButton == nullptr || dialog.clickedButton() != replaceButton)
    {
        return;
    }

    const QString sshKeygen = locateSshKeygen();
    if (sshKeygen.isEmpty())
    {
        showCriticalMessage("无法恢复 SSH 连接", "未找到系统 ssh-keygen.exe。请手动删除对应的 known_hosts 旧记录。");
        return;
    }

    QProcess process;
    process.start(sshKeygen, {"-R", target});
    if (!process.waitForStarted(5000) || !process.waitForFinished(10000)
        || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    {
        const QString detail = QString::fromUtf8(process.readAllStandardError()).trimmed();
        showCriticalMessage(
            "无法替换 SSH 主机密钥记录",
            detail.isEmpty() ? "ssh-keygen 未能删除该主机的旧记录。" : detail);
        return;
    }

    appendLog("INFO", QString("已删除 SSH 主机旧密钥记录：%1").arg(target));
    removeTerminalTab(terminalId);
    openSshTerminal(profile);
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
