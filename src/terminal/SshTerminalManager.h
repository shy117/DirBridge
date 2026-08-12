#ifndef DIRBRIDGE_TERMINAL_SSHTERMINALMANAGER_H
#define DIRBRIDGE_TERMINAL_SSHTERMINALMANAGER_H

#include "config/SiteProfile.h"
#include "terminal/TerminalTypes.h"

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace dirbridge::terminal {

struct SshTerminalRuntimePaths
{
    std::filesystem::path brokerExecutable;
    std::filesystem::path askPassHelper;
    std::filesystem::path sshExecutable;
    std::filesystem::path terminalEngineLibrary;
    std::filesystem::path workingDirectory;
};

SshTerminalRuntimePaths defaultSshTerminalRuntimePaths(
    const std::filesystem::path &applicationDirectory,
    const std::filesystem::path &workingDirectory);

class SshTerminalManager final : public QObject
{
    Q_OBJECT

public:
    explicit SshTerminalManager(
        SshTerminalRuntimePaths paths,
        QObject *parent = nullptr);
    ~SshTerminalManager() override;

    QString openSession(const SiteProfile &profile);
    bool sendInput(const QString &terminalId, const QByteArray &bytes);
    bool sendKey(
        const QString &terminalId,
        const TerminalKeyEvent &event);
    bool sendText(const QString &terminalId, const QByteArray &utf8);
    bool sendPaste(const QString &terminalId, const QByteArray &utf8);
    bool sendMouse(
        const QString &terminalId,
        const TerminalMouseEvent &event);
    bool scrollLines(const QString &terminalId, int lines);
    bool resize(
        const QString &terminalId,
        const TerminalGeometry &geometry);
    bool resize(
        const QString &terminalId,
        std::uint16_t columns,
        std::uint16_t rows);
    bool requestClose(const QString &terminalId);
    void beginShutdown();

    int sessionCount() const noexcept;
    bool isShuttingDown() const noexcept;
    QString lastError() const;

Q_SIGNALS:
    void sessionReady(const QString &terminalId);
    void sessionSnapshot(
        const QString &terminalId,
        dirbridge::terminal::TerminalSnapshotPtr snapshot);
    void sessionExited(
        const QString &terminalId,
        quint32 sshExitCode,
        bool closeRequested);
    void hostKeyConflictDetected(
        const QString &terminalId,
        const QString &fingerprint);
    void sessionError(const QString &terminalId, const QString &message);
    void sessionStopped(const QString &terminalId);
    void allSessionsStopped();

private:
    struct Session;
    void runReader(Session *session);
    void finishReader(
        const QString &terminalId,
        const QString &readerError,
        bool sawStopped);
    void scheduleSnapshot(Session *session);
    void publishSnapshot(const QString &terminalId);
    bool sendEncoded(Session *session, std::vector<std::uint8_t> bytes);
    Session *findSession(const QString &terminalId) const noexcept;
    void setError(const QString &message);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dirbridge::terminal

#endif // DIRBRIDGE_TERMINAL_SSHTERMINALMANAGER_H
