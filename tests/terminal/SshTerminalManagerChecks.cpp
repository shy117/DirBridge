#include "terminal/SshTerminalManager.h"

#include <QCoreApplication>
#include <QTimer>

#include <filesystem>
#include <iostream>

using dirbridge::terminal::SshTerminalManager;
using dirbridge::terminal::SshTerminalRuntimePaths;

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2)
    {
        return 64;
    }

    const std::filesystem::path fakeBroker = std::filesystem::absolute(argv[1]);
    SshTerminalRuntimePaths paths;
    paths.brokerExecutable = fakeBroker;
    paths.askPassHelper = fakeBroker;
    paths.sshExecutable = fakeBroker;
    paths.workingDirectory = std::filesystem::current_path();
    SshTerminalManager manager(std::move(paths));

    SiteProfile ftp;
    ftp.protocol = RemoteProtocol::Ftp;
    ftp.host = "example.invalid";
    if (!manager.openSession(ftp).isEmpty() || manager.lastError().isEmpty())
    {
        return 1;
    }

    SiteProfile site;
    site.id = "manager-check";
    site.name = "Manager Check";
    site.protocol = RemoteProtocol::Sftp;
    site.host = "example.invalid";
    site.port = 22;
    site.username = "tester";
    site.password = "saved-test-secret";

    bool ready = false;
    bool output = false;
    bool exited = false;
    bool closeWasRequested = false;
    bool stopped = false;
    bool error = false;
    bool allStopped = false;
    QString terminalId;

    QObject::connect(
        &manager,
        &SshTerminalManager::sessionReady,
        &application,
        [&](const QString &id) {
            if (id != terminalId)
            {
                error = true;
                application.quit();
                return;
            }
            ready = manager.resize(id, 100, 30)
                && manager.sendInput(id, QByteArray("pwd\r"));
        });
    QObject::connect(
        &manager,
        &SshTerminalManager::sessionOutput,
        &application,
        [&](const QString &id, const QByteArray &bytes) {
            if (id == terminalId && bytes == QByteArray("ok"))
            {
                output = true;
                if (!manager.requestClose(id))
                {
                    error = true;
                    application.quit();
                }
            }
        });
    QObject::connect(
        &manager,
        &SshTerminalManager::sessionExited,
        &application,
        [&](const QString &id, quint32 exitCode, bool requested) {
            exited = id == terminalId && exitCode == 0;
            closeWasRequested = requested;
        });
    QObject::connect(
        &manager,
        &SshTerminalManager::sessionError,
        &application,
        [&](const QString &, const QString &message) {
            std::cerr << message.toStdString() << '\n';
            error = true;
            application.quit();
        });
    QObject::connect(
        &manager,
        &SshTerminalManager::sessionStopped,
        &application,
        [&](const QString &id) {
            stopped = id == terminalId;
            QTimer::singleShot(20, &application, [&]() {
                manager.beginShutdown();
            });
        });
    QObject::connect(
        &manager,
        &SshTerminalManager::allSessionsStopped,
        &application,
        [&]() {
            allStopped = true;
            application.quit();
        });

    terminalId = manager.openSession(site);
    if (terminalId.isEmpty() || manager.sessionCount() != 1)
    {
        std::cerr << manager.lastError().toStdString() << '\n';
        return 2;
    }
    QTimer::singleShot(5000, &application, [&]() {
        error = true;
        application.quit();
    });
    application.exec();

    bool cancellationStarted = false;
    {
        SshTerminalRuntimePaths cancellationPaths;
        cancellationPaths.brokerExecutable = fakeBroker;
        cancellationPaths.askPassHelper = fakeBroker;
        cancellationPaths.sshExecutable = fakeBroker;
        cancellationPaths.workingDirectory = std::filesystem::current_path();
        SshTerminalManager cancellationManager(std::move(cancellationPaths));
        cancellationStarted = !cancellationManager.openSession(site).isEmpty();
    }

    const bool passed = ready && output && exited && closeWasRequested
        && stopped && allStopped && cancellationStarted && !error
        && manager.sessionCount() == 0;
    std::cout << (passed ? "[PASS] " : "[FAIL] ")
              << "Qt SSH terminal manager lifecycle\n";
    return passed ? 0 : 3;
}
