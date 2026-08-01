#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "terminal/SshTerminalManager.h"

#include "terminal/TerminalBrokerClient.h"

#include <QByteArray>
#include <QMetaObject>
#include <QPointer>
#include <QUuid>

#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <utility>
#include <vector>

namespace dirbridge::terminal {

using broker::EventReadResult;
using broker::Frame;
using broker::FrameType;
using broker::StartRequest;
using broker::TerminalBrokerClient;

namespace {

std::uint32_t read32(const std::vector<std::uint8_t> &bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

QString fromUtf8(const std::string &value)
{
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

} // namespace

SshTerminalRuntimePaths defaultSshTerminalRuntimePaths(
    const std::filesystem::path &applicationDirectory,
    const std::filesystem::path &workingDirectory)
{
    SshTerminalRuntimePaths paths;
    paths.brokerExecutable = applicationDirectory
        / L"DirBridgeTerminalBroker.exe";
    paths.askPassHelper = applicationDirectory / L"DirBridgeSshAskPass.exe";
    paths.workingDirectory = workingDirectory;

    wchar_t windowsDirectory[32768]{};
    const UINT length = GetWindowsDirectoryW(
        windowsDirectory,
        static_cast<UINT>(std::size(windowsDirectory)));
    if (length > 0 && length < std::size(windowsDirectory))
    {
        paths.sshExecutable = std::filesystem::path(windowsDirectory)
            / L"System32" / L"OpenSSH" / L"ssh.exe";
    }
    return paths;
}

struct SshTerminalManager::Session
{
    QString id;
    std::unique_ptr<TerminalBrokerClient> client;
    std::thread reader;
    std::atomic<bool> closeRequested{false};
};

struct SshTerminalManager::Impl
{
    explicit Impl(SshTerminalRuntimePaths runtimePaths)
        : paths(std::move(runtimePaths))
    {
    }

    SshTerminalRuntimePaths paths;
    std::map<QString, std::unique_ptr<Session>> sessions;
    QString error;
    bool shuttingDown = false;
};

SshTerminalManager::SshTerminalManager(
    SshTerminalRuntimePaths paths,
    QObject *parent)
    : QObject(parent),
      impl_(std::make_unique<Impl>(std::move(paths)))
{
}

SshTerminalManager::~SshTerminalManager()
{
    impl_->shuttingDown = true;
    for (auto &[id, session] : impl_->sessions)
    {
        Q_UNUSED(id);
        session->closeRequested = true;
        session->client->terminate();
    }
    for (auto &[id, session] : impl_->sessions)
    {
        Q_UNUSED(id);
        if (session->reader.joinable())
        {
            session->reader.join();
        }
    }
    impl_->sessions.clear();
}

QString SshTerminalManager::openSession(const SiteProfile &profile)
{
    if (impl_->shuttingDown)
    {
        setError("终端管理器正在关闭。");
        return {};
    }
    if (profile.protocol != RemoteProtocol::Sftp || profile.host.empty())
    {
        setError("只有完整的 SFTP 站点可以打开 SSH 终端。");
        return {};
    }

    StartRequest request;
    request.ssh.displayName = profile.name;
    request.ssh.host = profile.host;
    request.ssh.port = profile.port;
    if (!profile.username.empty())
    {
        request.ssh.username = profile.username;
    }
    request.ssh.authentication = profile.password.empty()
        ? SshAuthenticationMode::SystemDefault
        : SshAuthenticationMode::StoredPassword;
    request.sshExecutable = impl_->paths.sshExecutable;
    request.workingDirectory = impl_->paths.workingDirectory;
    request.askPassHelper = impl_->paths.askPassHelper;

    auto session = std::make_unique<Session>();
    session->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session->client = std::make_unique<TerminalBrokerClient>();
    try
    {
        const bool started = profile.password.empty()
            ? session->client->start(impl_->paths.brokerExecutable, request)
            : session->client->start(
                impl_->paths.brokerExecutable,
                request,
                StoredPasswordLease(profile.password));
        if (!started)
        {
            setError(fromUtf8(session->client->error()));
            return {};
        }
    }
    catch (const std::exception &exception)
    {
        setError(fromUtf8(exception.what()));
        return {};
    }

    Session *sessionPointer = session.get();
    const QString terminalId = session->id;
    impl_->sessions.emplace(terminalId, std::move(session));
    sessionPointer->reader = std::thread([this, sessionPointer]() {
        runReader(sessionPointer);
    });
    return terminalId;
}

bool SshTerminalManager::sendInput(
    const QString &terminalId,
    const QByteArray &bytes)
{
    Session *session = findSession(terminalId);
    if (session == nullptr || session->closeRequested)
    {
        setError("SSH 终端会话不存在或正在关闭。");
        return false;
    }
    const auto *begin = reinterpret_cast<const std::uint8_t *>(bytes.constData());
    if (!session->client->sendInput(
            std::vector<std::uint8_t>(begin, begin + bytes.size())))
    {
        setError(fromUtf8(session->client->error()));
        return false;
    }
    return true;
}

bool SshTerminalManager::resize(
    const QString &terminalId,
    std::uint16_t columns,
    std::uint16_t rows)
{
    Session *session = findSession(terminalId);
    if (session == nullptr || session->closeRequested)
    {
        setError("SSH 终端会话不存在或正在关闭。");
        return false;
    }
    if (!session->client->resize(columns, rows))
    {
        setError(fromUtf8(session->client->error()));
        return false;
    }
    return true;
}

bool SshTerminalManager::requestClose(const QString &terminalId)
{
    Session *session = findSession(terminalId);
    if (session == nullptr)
    {
        setError("SSH 终端会话不存在。");
        return false;
    }
    if (session->closeRequested.exchange(true))
    {
        return true;
    }
    if (!session->client->close())
    {
        setError(fromUtf8(session->client->error()));
        return false;
    }
    return true;
}

void SshTerminalManager::beginShutdown()
{
    if (impl_->shuttingDown)
    {
        return;
    }
    impl_->shuttingDown = true;
    for (auto &[id, session] : impl_->sessions)
    {
        Q_UNUSED(id);
        if (!session->closeRequested.exchange(true))
        {
            session->client->close();
        }
    }
    if (impl_->sessions.empty())
    {
        Q_EMIT allSessionsStopped();
    }
}

int SshTerminalManager::sessionCount() const noexcept
{
    return static_cast<int>(impl_->sessions.size());
}

bool SshTerminalManager::isShuttingDown() const noexcept
{
    return impl_->shuttingDown;
}

QString SshTerminalManager::lastError() const
{
    return impl_->error;
}

void SshTerminalManager::runReader(Session *session)
{
    QString readerError;
    bool sawStopped = false;
    for (;;)
    {
        Frame frame;
        const EventReadResult result = session->client->readEvent(frame);
        if (result != EventReadResult::Event)
        {
            if (!sawStopped && result == EventReadResult::Error)
            {
                readerError = fromUtf8(session->client->error());
            }
            else if (!sawStopped && !session->closeRequested)
            {
                readerError = "Broker 事件通道意外关闭。";
            }
            break;
        }

        const QString terminalId = session->id;
        QPointer<SshTerminalManager> manager(this);
        if (frame.type == FrameType::Ready)
        {
            QMetaObject::invokeMethod(this, [manager, terminalId]() {
                if (manager)
                {
                    Q_EMIT manager->sessionReady(terminalId);
                }
            }, Qt::QueuedConnection);
        }
        else if (frame.type == FrameType::Output)
        {
            const QByteArray bytes(
                reinterpret_cast<const char *>(frame.payload.data()),
                static_cast<int>(frame.payload.size()));
            QMetaObject::invokeMethod(this, [manager, terminalId, bytes]() {
                if (manager)
                {
                    Q_EMIT manager->sessionOutput(terminalId, bytes);
                }
            }, Qt::QueuedConnection);
        }
        else if (frame.type == FrameType::Exit && frame.payload.size() == 4)
        {
            const quint32 exitCode = read32(frame.payload);
            const bool closeRequested = session->closeRequested;
            QMetaObject::invokeMethod(
                this,
                [manager, terminalId, exitCode, closeRequested]() {
                    if (manager)
                    {
                        Q_EMIT manager->sessionExited(
                            terminalId, exitCode, closeRequested);
                    }
                },
                Qt::QueuedConnection);
        }
        else if (frame.type == FrameType::Error)
        {
            const QString message = QString::fromUtf8(
                reinterpret_cast<const char *>(frame.payload.data()),
                static_cast<int>(frame.payload.size()));
            QMetaObject::invokeMethod(this, [manager, terminalId, message]() {
                if (manager)
                {
                    Q_EMIT manager->sessionError(terminalId, message);
                }
            }, Qt::QueuedConnection);
        }
        else if (frame.type == FrameType::Stopped)
        {
            sawStopped = true;
            QMetaObject::invokeMethod(this, [manager, terminalId]() {
                if (manager)
                {
                    Q_EMIT manager->sessionStopped(terminalId);
                }
            }, Qt::QueuedConnection);
            break;
        }
    }

    std::uint32_t brokerExitCode = 0;
    if (!session->client->waitForBroker(
            std::chrono::seconds(5), brokerExitCode)
        && readerError.isEmpty())
    {
        readerError = fromUtf8(session->client->error());
    }
    const QString terminalId = session->id;
    QPointer<SshTerminalManager> manager(this);
    QMetaObject::invokeMethod(this, [manager, terminalId, readerError, sawStopped]() {
        if (manager)
        {
            manager->finishReader(terminalId, readerError, sawStopped);
        }
    }, Qt::QueuedConnection);
}

void SshTerminalManager::finishReader(
    const QString &terminalId,
    const QString &readerError,
    bool sawStopped)
{
    auto found = impl_->sessions.find(terminalId);
    if (found == impl_->sessions.end())
    {
        return;
    }
    if (found->second->reader.joinable())
    {
        found->second->reader.join();
    }
    if (!readerError.isEmpty())
    {
        Q_EMIT sessionError(terminalId, readerError);
    }
    if (!sawStopped)
    {
        Q_EMIT sessionStopped(terminalId);
    }
    impl_->sessions.erase(found);
    if (impl_->shuttingDown && impl_->sessions.empty())
    {
        Q_EMIT allSessionsStopped();
    }
}

SshTerminalManager::Session *SshTerminalManager::findSession(
    const QString &terminalId) const noexcept
{
    const auto found = impl_->sessions.find(terminalId);
    return found == impl_->sessions.end() ? nullptr : found->second.get();
}

void SshTerminalManager::setError(const QString &message)
{
    impl_->error = message;
}

} // namespace dirbridge::terminal
