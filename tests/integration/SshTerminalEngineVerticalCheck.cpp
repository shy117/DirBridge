#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "config/SiteStore.h"
#include "terminal/SshTerminalManager.h"

#include <QCoreApplication>
#include <QTimer>

#include <algorithm>
#include <filesystem>
#include <iostream>

using namespace dirbridge::terminal;

namespace {

std::string snapshotText(const TerminalSnapshotPtr &snapshot)
{
    std::string text;
    for (const auto &row : snapshot->rows)
    {
        for (const auto &cell : row)
        {
            text += cell.text;
        }
        text.push_back('\n');
    }
    return text;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 6)
    {
        std::cerr << "usage: check <broker> <askpass> <ghostty> <config> <site>\n";
        return 64;
    }

    const std::filesystem::path config = std::filesystem::absolute(argv[4]);
    auto sites = SiteStore(config).load();
    const std::string siteName = argv[5];
    const auto found = std::find_if(sites.begin(), sites.end(),
        [&siteName](const SiteProfile &profile) {
            return profile.name == siteName;
        });
    if (found == sites.end() || found->protocol != RemoteProtocol::Sftp
        || found->password.empty())
    {
        return 2;
    }

    SshTerminalRuntimePaths paths;
    paths.brokerExecutable = std::filesystem::absolute(argv[1]);
    paths.askPassHelper = std::filesystem::absolute(argv[2]);
    paths.terminalEngineLibrary = std::filesystem::absolute(argv[3]);
    paths.workingDirectory = std::filesystem::current_path();
    wchar_t windowsDirectory[32768]{};
    const UINT length = GetWindowsDirectoryW(
        windowsDirectory, static_cast<UINT>(std::size(windowsDirectory)));
    if (length == 0 || length >= std::size(windowsDirectory))
    {
        return 3;
    }
    paths.sshExecutable = std::filesystem::path(windowsDirectory)
        / L"System32" / L"OpenSSH" / L"ssh.exe";

    SshTerminalManager manager(std::move(paths));
    QString terminalId;
    bool ready = false;
    bool markerSeen = false;
    bool stopped = false;
    bool failed = false;
    constexpr auto marker = "DIRBRIDGE_ENGINE_UBUNTU_OK";

    QObject::connect(&manager, &SshTerminalManager::sessionReady,
        [&](const QString &id) {
            ready = id == terminalId;
            TerminalGeometry geometry;
            geometry.columns = 120;
            geometry.rows = 40;
            if (!ready || !manager.resize(id, geometry)
                || !manager.sendText(id,
                    QByteArray("printf 'DIRBRIDGE_ENGINE_UBUNTU_OK\\n'\r")))
            {
                failed = true;
                application.quit();
            }
        });
    QObject::connect(&manager, &SshTerminalManager::sessionSnapshot,
        [&](const QString &id, const TerminalSnapshotPtr &snapshot) {
            if (id == terminalId
                && snapshotText(snapshot).find(marker) != std::string::npos)
            {
                markerSeen = true;
                if (!manager.requestClose(id))
                {
                    failed = true;
                    application.quit();
                }
            }
        });
    QObject::connect(&manager, &SshTerminalManager::sessionError,
        [&](const QString &, const QString &message) {
            std::cerr << message.toStdString() << '\n';
            failed = true;
            application.quit();
        });
    QObject::connect(&manager, &SshTerminalManager::sessionStopped,
        [&](const QString &id) {
            stopped = id == terminalId;
            application.quit();
        });

    terminalId = manager.openSession(*found);
    if (terminalId.isEmpty())
    {
        std::cerr << manager.lastError().toStdString() << '\n';
        return 4;
    }
    QTimer::singleShot(60000, &application, [&]() {
        failed = true;
        application.quit();
    });
    application.exec();
    const bool passed = ready && markerSeen && stopped && !failed;
    std::cout << (passed ? "[PASS] " : "[FAIL] ")
              << "manager + Ghostty + Broker + ConPTY + OpenSSH + Ubuntu\n";
    return passed ? 0 : 5;
}
