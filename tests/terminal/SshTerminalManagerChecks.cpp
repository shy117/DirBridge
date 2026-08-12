#include "terminal/SshTerminalManager.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <filesystem>
#include <iostream>
#include <set>

using dirbridge::terminal::SshTerminalManager;
using dirbridge::terminal::SshTerminalRuntimePaths;

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 3)
    {
        return 64;
    }

    const std::filesystem::path fakeBroker = std::filesystem::absolute(argv[1]);
    const std::filesystem::path terminalEngine = std::filesystem::absolute(argv[2]);
    SshTerminalRuntimePaths paths;
    paths.brokerExecutable = fakeBroker;
    paths.askPassHelper = fakeBroker;
    paths.sshExecutable = fakeBroker;
    paths.terminalEngineLibrary = terminalEngine;
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
    site.sshRsaHostKeyCompatibility = true;

    SshTerminalRuntimePaths missingPaths;
    missingPaths.brokerExecutable = fakeBroker.parent_path()
        / L"missing-terminal-broker.exe";
    missingPaths.askPassHelper = fakeBroker;
    missingPaths.sshExecutable = fakeBroker;
    missingPaths.terminalEngineLibrary = terminalEngine;
    missingPaths.workingDirectory = std::filesystem::current_path();
    SshTerminalManager missingManager(std::move(missingPaths));
    if (!missingManager.openSession(site).isEmpty()
        || !missingManager.lastError().contains("SSH Broker"))
    {
        return 2;
    }

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
            dirbridge::terminal::TerminalGeometry geometry;
            geometry.columns = 100;
            geometry.rows = 30;
            ready = manager.resize(id, geometry)
                && manager.sendText(id, QByteArray("pwd\r"));
        });
    QObject::connect(
        &manager,
        &SshTerminalManager::sessionSnapshot,
        &application,
        [&](const QString &id,
            const dirbridge::terminal::TerminalSnapshotPtr &snapshot) {
            if (id == terminalId && snapshot && !snapshot->rows.empty())
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
        cancellationPaths.terminalEngineLibrary = terminalEngine;
        cancellationPaths.workingDirectory = std::filesystem::current_path();
        SshTerminalManager cancellationManager(std::move(cancellationPaths));
        cancellationStarted = !cancellationManager.openSession(site).isEmpty();
    }

    bool hostKeyConflictPassed = false;
    {
        SshTerminalRuntimePaths conflictPaths;
        conflictPaths.brokerExecutable = fakeBroker;
        conflictPaths.askPassHelper = fakeBroker;
        conflictPaths.sshExecutable = fakeBroker;
        conflictPaths.terminalEngineLibrary = terminalEngine;
        conflictPaths.workingDirectory = std::filesystem::current_path();
        SshTerminalManager conflictManager(std::move(conflictPaths));
        QEventLoop conflictLoop;
        bool conflictDetected = false;
        bool conflictStopped = false;
        bool conflictError = false;
        QString conflictId;
        SiteProfile conflictSite = site;
        conflictSite.host = "host-key-conflict.invalid";
        conflictSite.password.clear();

        QObject::connect(&conflictManager,
            &SshTerminalManager::hostKeyConflictDetected,
            &conflictLoop,
            [&](const QString &id, const QString &fingerprint) {
                conflictDetected = id == conflictId
                    && fingerprint == "SHA256:DirBridgeHostKeyConflictTest=";
            });
        QObject::connect(&conflictManager,
            &SshTerminalManager::sessionStopped,
            &conflictLoop,
            [&](const QString &id) {
                conflictStopped = id == conflictId;
                QTimer::singleShot(20, &conflictLoop, &QEventLoop::quit);
            });
        QObject::connect(&conflictManager,
            &SshTerminalManager::sessionError,
            &conflictLoop,
            [&](const QString &, const QString &) {
                conflictError = true;
                conflictLoop.quit();
            });
        conflictId = conflictManager.openSession(conflictSite);
        QTimer::singleShot(5000, &conflictLoop, [&]() {
            conflictError = true;
            conflictLoop.quit();
        });
        if (!conflictId.isEmpty())
        {
            conflictLoop.exec();
        }
        hostKeyConflictPassed = !conflictError
            && conflictDetected && conflictStopped
            && conflictManager.sessionCount() == 0;
    }

    bool stressPassed = false;
    {
        SshTerminalRuntimePaths stressPaths;
        stressPaths.brokerExecutable = fakeBroker;
        stressPaths.askPassHelper = fakeBroker;
        stressPaths.sshExecutable = fakeBroker;
        stressPaths.terminalEngineLibrary = terminalEngine;
        stressPaths.workingDirectory = std::filesystem::current_path();
        SshTerminalManager stressManager(std::move(stressPaths));
        QEventLoop stressLoop;
        std::set<QString> sessionIds;
        std::set<QString> closeSent;
        int stoppedCount = 0;
        bool stressError = false;
        constexpr int stressSessionCount = 20;

        QObject::connect(&stressManager,
            &SshTerminalManager::sessionReady,
            &application,
            [&](const QString &id) {
                dirbridge::terminal::TerminalGeometry geometry;
                geometry.columns = 100;
                geometry.rows = 30;
                if (sessionIds.count(id) == 0
                    || !stressManager.resize(id, geometry)
                    || !stressManager.sendText(id, QByteArray("pwd\r")))
                {
                    stressError = true;
                    stressLoop.quit();
                }
            });
        QObject::connect(&stressManager,
            &SshTerminalManager::sessionSnapshot,
            &application,
            [&](const QString &id,
                const dirbridge::terminal::TerminalSnapshotPtr &snapshot) {
                std::string text;
                if (snapshot)
                {
                    for (const auto &row : snapshot->rows)
                    {
                        for (const auto &cell : row)
                        {
                            text += cell.text;
                        }
                    }
                }
                if (text.find("ok") != std::string::npos
                    && closeSent.insert(id).second
                    && !stressManager.requestClose(id))
                {
                    stressError = true;
                    stressLoop.quit();
                }
            });
        QObject::connect(&stressManager,
            &SshTerminalManager::sessionError,
            &application,
            [&](const QString &, const QString &) {
                stressError = true;
                stressLoop.quit();
            });
        QObject::connect(&stressManager,
            &SshTerminalManager::sessionStopped,
            &application,
            [&](const QString &) {
                ++stoppedCount;
                if (stoppedCount == stressSessionCount)
                {
                    QTimer::singleShot(20, &application, [&]() {
                        stressManager.beginShutdown();
                    });
                }
            });
        QObject::connect(&stressManager,
            &SshTerminalManager::allSessionsStopped,
            &stressLoop,
            &QEventLoop::quit);

        for (int index = 0; index < stressSessionCount; ++index)
        {
            const QString id = stressManager.openSession(site);
            if (id.isEmpty())
            {
                stressError = true;
                break;
            }
            sessionIds.insert(id);
        }
        QTimer::singleShot(10000, &stressLoop, [&]() {
            stressError = true;
            stressLoop.quit();
        });
        if (!stressError)
        {
            stressLoop.exec();
        }
        stressPassed = !stressError
            && stoppedCount == stressSessionCount
            && closeSent.size() == stressSessionCount
            && stressManager.sessionCount() == 0;
    }

    const bool passed = ready && output && exited && closeWasRequested
        && stopped && allStopped && cancellationStarted && stressPassed && !error
        && hostKeyConflictPassed && manager.sessionCount() == 0;
    std::cout << (passed ? "[PASS] " : "[FAIL] ")
              << "Qt SSH terminal manager lifecycle\n";
    return passed ? 0 : 3;
}
