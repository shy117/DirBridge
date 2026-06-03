#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QTextStream>
#include <QTimer>

#include "core/CurlProtocolCheck.h"
#include "ui/MainWindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("XFolder");
    QApplication::setApplicationVersion("0.1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("XFolder remote folder manager");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption checkCurlOption("check-curl", "Check libcurl protocol support and exit.");
    QCommandLineOption smokeTestOption("smoke-test", "Show the main window briefly and exit.");
    parser.addOption(checkCurlOption);
    parser.addOption(smokeTestOption);
    parser.process(app);

    const CurlProtocolCheckResult curlCheck = checkCurlProtocols();
    QTextStream(stdout) << formatCurlProtocolCheck(curlCheck) << Qt::endl;

    if (!curlCheck.hasFtp || !curlCheck.hasSftp)
    {
        QTextStream(stderr) << "The linked libcurl build must support both ftp and sftp." << Qt::endl;
        return 2;
    }

    if (parser.isSet(checkCurlOption))
    {
        return 0;
    }

    MainWindow window(curlCheck);
    window.show();

    if (parser.isSet(smokeTestOption))
    {
        QTimer::singleShot(1200, &app, &QCoreApplication::quit);
    }

    return app.exec();
}
