#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QIcon>
#include <QTextStream>
#include <QTimer>

#include "app/UiSmokeTests.h"
#include "core/CurlProtocolCheck.h"
#include "core/DependencyCheck.h"
#include "logging/AppLogger.h"
#include "ui/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(":/icons/app/dirbridge.ico"));
    QApplication::setApplicationName("DirBridge");
    QApplication::setApplicationVersion("0.5.9");

    QCommandLineParser parser;
    parser.setApplicationDescription("DirBridge remote folder manager");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption checkCurlOption("check-curl", "Check libcurl protocol support and exit.");
    QCommandLineOption checkDepsOption("check-deps", "Check libcurl, JSON and logging support and exit.");
    QCommandLineOption smokeTestOption("smoke-test", "Show the main window briefly and exit.");
    QCommandLineOption uiRemoteSmokeTestOption("ui-remote-smoke-test", "Check remote UI object wiring and exit.");
    QCommandLineOption uiRemoteWorkflowSmokeTestOption("ui-remote-workflow-smoke-test", "Check remote UI state workflow with fake backend and exit.");
    QCommandLineOption uiRemoteLiveSmokeTestOption("ui-remote-live-smoke-test", "Check remote UI state workflow with a real FTP/SFTP server and exit.");
    QCommandLineOption uiRemoteLiveRefreshSmokeTestOption("ui-remote-live-refresh-smoke-test", "Check remote UI refresh with a real FTP/SFTP server and exit.");
    QCommandLineOption uiRemoteLiveTransferSmokeTestOption("ui-remote-live-transfer-smoke-test", "Check upload/download transfer UI workflow with a real FTP/SFTP server and exit.");
    parser.addOption(checkCurlOption);
    parser.addOption(checkDepsOption);
    parser.addOption(smokeTestOption);
    parser.addOption(uiRemoteSmokeTestOption);
    parser.addOption(uiRemoteWorkflowSmokeTestOption);
    parser.addOption(uiRemoteLiveSmokeTestOption);
    parser.addOption(uiRemoteLiveRefreshSmokeTestOption);
    parser.addOption(uiRemoteLiveTransferSmokeTestOption);
    parser.process(app);

    const CurlProtocolCheckResult curlCheck = checkCurlProtocols();

    if (!curlCheck.hasFtp || !curlCheck.hasSftp)
    {
        QTextStream(stdout) << QString::fromStdString(formatCurlProtocolCheck(curlCheck)) << Qt::endl;
        QTextStream(stderr) << "The linked libcurl build must support both ftp and sftp." << Qt::endl;
        return 2;
    }

    if (parser.isSet(checkCurlOption))
    {
        QTextStream(stdout) << QString::fromStdString(formatCurlProtocolCheck(curlCheck)) << Qt::endl;
        return 0;
    }

    const DependencyCheckResult dependencyCheck = checkDependencies("config", "logs");

    if (parser.isSet(checkDepsOption))
    {
        QTextStream(stdout) << QString::fromStdString(formatDependencyCheck(dependencyCheck)) << Qt::endl;
        return dependenciesReady(dependencyCheck) ? 0 : 3;
    }

    MainWindow window(dependencyCheck);
    window.show();

    if (parser.isSet(uiRemoteSmokeTestOption))
    {
        const bool ok = checkRemoteUiObjects(window);
        AppLogger::shutdown();
        return ok ? 0 : 4;
    }

    if (parser.isSet(uiRemoteWorkflowSmokeTestOption))
    {
        window.setDialogsSuppressedForTesting(true);
        const bool ok = checkRemoteUiWorkflow(window);
        AppLogger::shutdown();
        return ok ? 0 : 5;
    }

    if (parser.isSet(uiRemoteLiveSmokeTestOption))
    {
        window.setDialogsSuppressedForTesting(true);
        const bool ok = checkLiveRemoteUiWorkflow(window);
        AppLogger::shutdown();
        return ok ? 0 : 6;
    }

    if (parser.isSet(uiRemoteLiveRefreshSmokeTestOption))
    {
        window.setDialogsSuppressedForTesting(true);
        const bool ok = checkLiveRemoteRefreshWorkflow(window);
        AppLogger::shutdown();
        return ok ? 0 : 7;
    }

    if (parser.isSet(uiRemoteLiveTransferSmokeTestOption))
    {
        window.setDialogsSuppressedForTesting(true);
        const bool ok = checkLiveRemoteTransferWorkflow(window);
        AppLogger::shutdown();
        return ok ? 0 : 8;
    }

    if (parser.isSet(smokeTestOption))
    {
        QTimer::singleShot(1200, &app, &QCoreApplication::quit);
    }

    const int exitCode = app.exec();
    AppLogger::shutdown();
    return exitCode;
}
