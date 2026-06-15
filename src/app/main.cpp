#include <QApplication>
#include <QAction>
#include <QCommandLineParser>
#include <QComboBox>
#include <QDebug>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <QTreeWidget>

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>

#include "config/SiteProfile.h"
#include "core/CurlProtocolCheck.h"
#include "core/DependencyCheck.h"
#include "core/FakeRemoteFileSystem.h"
#include "logging/AppLogger.h"
#include "protocol/CurlRemoteFileSystem.h"
#include "ui/MainWindow.h"

namespace
{
/**
 * @brief Finds a required child widget by object name and reports a smoke-test error when missing.
 * @param window Main window to search.
 * @param objectName Qt object name expected by the smoke test.
 * @return true when the child exists.
 */
template <typename Widget>
bool requireChild(MainWindow &window, const char *objectName)
{
    Widget *child = window.findChild<Widget *>(objectName);
    if (child == nullptr)
    {
        QTextStream(stderr) << "Missing UI object: " << objectName << Qt::endl;
        return false;
    }
    return true;
}

/**
 * @brief Verifies that remote workflow widgets and default quick-connect values exist.
 * @param window Main window under test.
 * @return true when all required UI objects and default values are present.
 */
bool checkRemoteUiObjects(MainWindow &window)
{
    bool ok = true;
    ok = requireChild<QComboBox>(window, "quickProtocolCombo") && ok;
    ok = requireChild<QLineEdit>(window, "quickHostEdit") && ok;
    ok = requireChild<QLineEdit>(window, "quickPortEdit") && ok;
    ok = requireChild<QLineEdit>(window, "quickUserEdit") && ok;
    ok = requireChild<QLineEdit>(window, "quickPasswordEdit") && ok;
    ok = requireChild<QLineEdit>(window, "quickRemotePathEdit") && ok;
    ok = requireChild<QPushButton>(window, "quickConnectButton") && ok;
    ok = requireChild<QLineEdit>(window, "remotePathEdit") && ok;
    ok = requireChild<QTreeWidget>(window, "remoteFileTree") && ok;
    ok = requireChild<QTableWidget>(window, "remoteFileTable") && ok;
    ok = requireChild<QLabel>(window, "remoteStateLabel") && ok;
    QPushButton *cancelTransferButton = window.findChild<QPushButton *>("transferCancelButton");
    QPushButton *retryTransferButton = window.findChild<QPushButton *>("transferRetryButton");
    QPushButton *clearFinishedButton = window.findChild<QPushButton *>("transferClearFinishedButton");
    if (cancelTransferButton == nullptr)
    {
        QTextStream(stderr) << "Missing UI object: transferCancelButton" << Qt::endl;
        ok = false;
    }
    if (retryTransferButton == nullptr)
    {
        QTextStream(stderr) << "Missing UI object: transferRetryButton" << Qt::endl;
        ok = false;
    }
    if (clearFinishedButton == nullptr)
    {
        QTextStream(stderr) << "Missing UI object: transferClearFinishedButton" << Qt::endl;
        ok = false;
    }

    QTreeWidget *transferTable = window.findChild<QTreeWidget *>("transferTable");
    if (transferTable == nullptr)
    {
        QTextStream(stderr) << "Missing UI object: transferTable" << Qt::endl;
        ok = false;
    }
    else if (transferTable->topLevelItemCount() == 0)
    {
        QTextStream(stderr) << "Transfer table has no initial rows" << Qt::endl;
        ok = false;
    }
    ok = requireChild<QTreeWidget>(window, "logView") && ok;

    QComboBox *quickProtocolCombo = window.findChild<QComboBox *>("quickProtocolCombo");
    if (quickProtocolCombo != nullptr && quickProtocolCombo->currentText() != "SFTP")
    {
        QTextStream(stderr) << "Quick connect default protocol is not SFTP" << Qt::endl;
        ok = false;
    }

    QLineEdit *quickPortEdit = window.findChild<QLineEdit *>("quickPortEdit");
    if (quickPortEdit != nullptr && quickPortEdit->text().trimmed().isEmpty())
    {
        QTextStream(stderr) << "Quick connect port is empty" << Qt::endl;
        ok = false;
    }
    else if (quickPortEdit != nullptr && quickPortEdit->text().trimmed() != "22")
    {
        QTextStream(stderr) << "Quick connect default port is not 22" << Qt::endl;
        ok = false;
    }

    QLineEdit *quickRemotePathEdit = window.findChild<QLineEdit *>("quickRemotePathEdit");
    if (quickRemotePathEdit != nullptr && quickRemotePathEdit->text().trimmed().isEmpty())
    {
        QTextStream(stderr) << "Quick connect remote path is empty" << Qt::endl;
        ok = false;
    }
    else if (quickRemotePathEdit != nullptr && quickRemotePathEdit->text().trimmed() != "/")
    {
        QTextStream(stderr) << "Quick connect default remote path is not /" << Qt::endl;
        ok = false;
    }

    if (transferTable != nullptr && transferTable->topLevelItemCount() > 0)
    {
        QTreeWidgetItem *firstItem = transferTable->topLevelItem(0);
        if (transferTable->columnCount() < 10)
        {
            QTextStream(stderr) << "Transfer table column count is less than 10" << Qt::endl;
            ok = false;
        }
        if (firstItem->text(0).trimmed().isEmpty()
            || firstItem->text(1).trimmed().isEmpty()
            || firstItem->text(2).trimmed().isEmpty()
            || firstItem->text(6).trimmed().isEmpty()
            || firstItem->text(8).trimmed().isEmpty())
        {
            QTextStream(stderr) << "Transfer table initial row is incomplete" << Qt::endl;
            ok = false;
        }

        transferTable->setCurrentItem(firstItem);
        if (cancelTransferButton != nullptr && cancelTransferButton->isEnabled())
        {
            QTextStream(stderr) << "Cancel button should be disabled for canceled initial row" << Qt::endl;
            ok = false;
        }
        if (retryTransferButton != nullptr && !retryTransferButton->isEnabled())
        {
            QTextStream(stderr) << "Retry button should be enabled for canceled initial row" << Qt::endl;
            ok = false;
        }
        if (clearFinishedButton != nullptr && !clearFinishedButton->isEnabled())
        {
            QTextStream(stderr) << "Clear finished button should be enabled for canceled initial row" << Qt::endl;
            ok = false;
        }
    }

    return ok;
}

/**
 * @brief Finds a QAction by visible text, ignoring Qt mnemonic markers.
 * @param window Main window whose actions should be searched.
 * @param text Visible action text to match.
 * @return Matching action, or nullptr when not found.
 */
QAction *findActionByText(MainWindow &window, const QString &text)
{
    const QList<QAction *> actions = window.findChildren<QAction *>();
    for (QAction *action : actions)
    {
        if (action != nullptr && action->text().remove('&') == text)
        {
            return action;
        }
    }

    return nullptr;
}

/**
 * @brief Finds a row in a remote file table by the display name column.
 * @param table Remote table widget.
 * @param name File or directory display name.
 * @return Zero-based row index, or -1 when absent.
 */
int findTableRowByName(QTableWidget *table, const QString &name)
{
    if (table == nullptr)
    {
        return -1;
    }

    for (int row = 0; row < table->rowCount(); ++row)
    {
        QTableWidgetItem *item = table->item(row, 0);
        if (item != nullptr && item->text() == name)
        {
            return row;
        }
    }

    return -1;
}

/**
 * @brief Checks whether the transfer table contains a row with the expected task state.
 * @param table Transfer queue table.
 * @param name Transfer task display name.
 * @param direction Localized direction text, such as 上传 or 下载.
 * @param status Localized status text, such as 已完成.
 * @return true when a matching transfer row exists.
 */
bool hasTransferRow(QTreeWidget *table, const QString &name, const QString &direction, const QString &status)
{
    if (table == nullptr)
    {
        return false;
    }

    for (int index = 0; index < table->topLevelItemCount(); ++index)
    {
        QTreeWidgetItem *item = table->topLevelItem(index);
        if (item != nullptr
            && item->text(0) == name
            && item->text(2) == direction
            && item->text(3) == status)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief Reads an environment variable for integration smoke tests.
 * @param name Environment variable name.
 * @param required Whether missing values should be reported as test errors.
 * @return Variable value as UTF-8 text, or an empty string when absent.
 */
QString environmentValue(const char *name, bool required = true)
{
    const char *value = std::getenv(name);
    if (value == nullptr || QString::fromUtf8(value).isEmpty())
    {
        if (required)
        {
            QTextStream(stderr) << "Missing environment variable: " << name << Qt::endl;
        }
        return {};
    }

    return QString::fromUtf8(value);
}

/**
 * @brief Joins a remote directory and child name for smoke-test path assertions.
 * @param directory Remote parent directory.
 * @param name Remote child item name.
 * @return Normalized remote child path.
 */
QString joinRemotePathForCheck(QString directory, const QString &name)
{
    if (directory.isEmpty())
    {
        directory = "/";
    }
    if (!directory.endsWith('/'))
    {
        directory.append('/');
    }
    return directory + name;
}

/**
 * @brief Drives the main window quick-connect workflow and validates navigation state.
 * @param window Main window under test.
 * @param protocol Protocol text selected in the quick-connect combo box.
 * @param host Remote host.
 * @param port Remote port.
 * @param user Remote username.
 * @param password Remote password.
 * @param remotePath Initial remote path to load.
 * @param expectedNames Expected entries in the remote table.
 * @param useFakeBackend Whether to inject the in-memory fake backend before connecting.
 * @return true when connection, navigation, and disconnect state checks pass.
 */
bool checkRemoteUiWorkflow(
    MainWindow &window,
    const QString &protocol,
    const QString &host,
    const QString &port,
    const QString &user,
    const QString &password,
    const QString &remotePath,
    const QStringList &expectedNames,
    bool useFakeBackend)
{
    bool ok = checkRemoteUiObjects(window);
    if (useFakeBackend)
    {
        window.setRemoteFileSystemForTesting(std::make_unique<FakeRemoteFileSystem>());
    }

    QComboBox *protocolCombo = window.findChild<QComboBox *>("quickProtocolCombo");
    QLineEdit *hostEdit = window.findChild<QLineEdit *>("quickHostEdit");
    QLineEdit *portEdit = window.findChild<QLineEdit *>("quickPortEdit");
    QLineEdit *userEdit = window.findChild<QLineEdit *>("quickUserEdit");
    QLineEdit *passwordEdit = window.findChild<QLineEdit *>("quickPasswordEdit");
    QLineEdit *quickRemotePathEdit = window.findChild<QLineEdit *>("quickRemotePathEdit");
    QPushButton *connectButton = window.findChild<QPushButton *>("quickConnectButton");
    QLineEdit *remotePathEdit = window.findChild<QLineEdit *>("remotePathEdit");
    QLabel *remoteStateLabel = window.findChild<QLabel *>("remoteStateLabel");
    QTableWidget *remoteTable = window.findChild<QTableWidget *>("remoteFileTable");
    QPushButton *remoteUpButton = window.findChild<QPushButton *>("remoteUpButton");
    QAction *disconnectAction = findActionByText(window, "断开");

    if (protocolCombo == nullptr || hostEdit == nullptr || portEdit == nullptr || userEdit == nullptr
        || passwordEdit == nullptr || quickRemotePathEdit == nullptr || connectButton == nullptr
        || remotePathEdit == nullptr || remoteStateLabel == nullptr || remoteTable == nullptr
        || remoteUpButton == nullptr || disconnectAction == nullptr)
    {
        QTextStream(stderr) << "Remote UI workflow prerequisites are incomplete" << Qt::endl;
        return false;
    }

    protocolCombo->setCurrentText(protocol);
    hostEdit->setText(host);
    portEdit->setText(port);
    userEdit->setText(user);
    passwordEdit->setText(password);
    quickRemotePathEdit->setText(remotePath);
    connectButton->click();
    QApplication::processEvents();

    if (remotePathEdit->text() != remotePath)
    {
        QTextStream(stderr) << "Remote path after connect is unexpected: " << remotePathEdit->text() << Qt::endl;
        ok = false;
    }
    if (!remoteStateLabel->text().contains("已连接"))
    {
        QTextStream(stderr) << "Remote state label does not show connected: " << remoteStateLabel->text() << Qt::endl;
        ok = false;
    }
    for (const QString &name : expectedNames)
    {
        if (findTableRowByName(remoteTable, name) < 0)
        {
            QTextStream(stderr) << "Remote table does not show expected item: " << name << Qt::endl;
            ok = false;
        }
    }

    const QString directoryName = expectedNames.isEmpty() ? QString() : expectedNames.first();
    const QString expectedChildPath = joinRemotePathForCheck(remotePath, directoryName);
    remotePathEdit->setText(expectedChildPath);
    QMetaObject::invokeMethod(remotePathEdit, "returnPressed", Qt::DirectConnection);
    QApplication::processEvents();
    if (remotePathEdit->text() != expectedChildPath)
    {
        QTextStream(stderr) << "Remote address bar jump is unexpected: " << remotePathEdit->text() << Qt::endl;
        ok = false;
    }

    remotePathEdit->setText(joinRemotePathForCheck(remotePath, "dirbridge_missing_path_for_smoke"));
    QMetaObject::invokeMethod(remotePathEdit, "returnPressed", Qt::DirectConnection);
    QApplication::processEvents();
    if (remotePathEdit->text() != expectedChildPath)
    {
        QTextStream(stderr) << "Missing remote address changed the current path unexpectedly: " << remotePathEdit->text() << Qt::endl;
        ok = false;
    }

    remoteUpButton->click();
    QApplication::processEvents();
    if (remotePathEdit->text() != remotePath)
    {
        QTextStream(stderr) << "Remote path after address-bar up navigation is unexpected: " << remotePathEdit->text() << Qt::endl;
        ok = false;
    }

    const int directoryRow = findTableRowByName(remoteTable, directoryName);
    if (directoryRow < 0
        || !QMetaObject::invokeMethod(remoteTable, "cellDoubleClicked", Qt::DirectConnection, Q_ARG(int, directoryRow), Q_ARG(int, 0)))
    {
        QTextStream(stderr) << "Remote directory double click could not be simulated" << Qt::endl;
        ok = false;
    }
    QApplication::processEvents();
    if (remotePathEdit->text() != expectedChildPath)
    {
        QTextStream(stderr) << "Remote path after double click is unexpected: " << remotePathEdit->text() << Qt::endl;
        ok = false;
    }

    remoteUpButton->click();
    QApplication::processEvents();
    if (remotePathEdit->text() != remotePath)
    {
        QTextStream(stderr) << "Remote path after up navigation is unexpected: " << remotePathEdit->text() << Qt::endl;
        ok = false;
    }

    disconnectAction->trigger();
    QApplication::processEvents();
    if (!remoteStateLabel->text().contains("断开") || remotePathEdit->text() != "/")
    {
        QTextStream(stderr) << "Remote panel was not reset after disconnect" << Qt::endl;
        ok = false;
    }

    return ok;
}

/**
 * @brief Connects one fake remote session through the quick-connect controls.
 * @param window Main window under test.
 * @param remotePath Initial path to load in the new session.
 * @return true when the current remote tab reaches the requested path.
 */
bool connectFakeRemoteSession(MainWindow &window, const QString &remotePath)
{
    window.setRemoteFileSystemForTesting(std::make_unique<FakeRemoteFileSystem>());

    QComboBox *protocolCombo = window.findChild<QComboBox *>("quickProtocolCombo");
    QLineEdit *hostEdit = window.findChild<QLineEdit *>("quickHostEdit");
    QLineEdit *portEdit = window.findChild<QLineEdit *>("quickPortEdit");
    QLineEdit *userEdit = window.findChild<QLineEdit *>("quickUserEdit");
    QLineEdit *passwordEdit = window.findChild<QLineEdit *>("quickPasswordEdit");
    QLineEdit *quickRemotePathEdit = window.findChild<QLineEdit *>("quickRemotePathEdit");
    QPushButton *connectButton = window.findChild<QPushButton *>("quickConnectButton");
    QTabWidget *remoteTabs = window.findChild<QTabWidget *>("remoteTabs");
    if (protocolCombo == nullptr || hostEdit == nullptr || portEdit == nullptr || userEdit == nullptr
        || passwordEdit == nullptr || quickRemotePathEdit == nullptr || connectButton == nullptr || remoteTabs == nullptr)
    {
        QTextStream(stderr) << "Fake multi-session prerequisites are incomplete" << Qt::endl;
        return false;
    }

    protocolCombo->setCurrentText("SFTP");
    hostEdit->setText("fake-host");
    portEdit->setText("22");
    userEdit->setText("testuser");
    passwordEdit->clear();
    quickRemotePathEdit->setText(remotePath);
    connectButton->click();
    QApplication::processEvents();

    QWidget *currentRemotePanel = remoteTabs->currentWidget();
    QLineEdit *remotePathEdit = currentRemotePanel == nullptr ? nullptr : currentRemotePanel->findChild<QLineEdit *>("remotePathEdit");
    QLabel *remoteStateLabel = currentRemotePanel == nullptr ? nullptr : currentRemotePanel->findChild<QLabel *>("remoteStateLabel");
    if (remotePathEdit == nullptr || remoteStateLabel == nullptr)
    {
        QTextStream(stderr) << "New fake remote session panel is incomplete" << Qt::endl;
        return false;
    }
    if (remotePathEdit->text() != remotePath || !remoteStateLabel->text().contains("已连接"))
    {
        QTextStream(stderr) << "New fake remote session did not connect to expected path" << Qt::endl;
        return false;
    }
    return true;
}

/**
 * @brief Verifies that multiple remote tabs keep independent state.
 * @param window Main window under test.
 * @return true when two additional fake sessions can coexist independently.
 */
bool checkRemoteMultiSessionWorkflow(MainWindow &window)
{
    QTabWidget *remoteTabs = window.findChild<QTabWidget *>("remoteTabs");
    QAction *disconnectAction = findActionByText(window, "断开");
    if (remoteTabs == nullptr || disconnectAction == nullptr)
    {
        QTextStream(stderr) << "Remote multi-session prerequisites are incomplete" << Qt::endl;
        return false;
    }

    const int initialCount = remoteTabs->count();
    const QString firstPath = "/home/testuser/remote_test";
    const QString secondPath = "/home/testuser";
    if (!connectFakeRemoteSession(window, firstPath))
    {
        return false;
    }
    const int firstIndex = remoteTabs->currentIndex();
    if (!connectFakeRemoteSession(window, secondPath))
    {
        return false;
    }
    const int secondIndex = remoteTabs->currentIndex();
    if (remoteTabs->count() != initialCount + 2 || firstIndex == secondIndex)
    {
        QTextStream(stderr) << "Remote tabs were not added independently" << Qt::endl;
        return false;
    }

    remoteTabs->setCurrentIndex(firstIndex);
    QApplication::processEvents();
    auto *firstPathEdit = remoteTabs->currentWidget()->findChild<QLineEdit *>("remotePathEdit");
    auto *firstStateLabel = remoteTabs->currentWidget()->findChild<QLabel *>("remoteStateLabel");
    if (firstPathEdit == nullptr || firstStateLabel == nullptr || firstPathEdit->text() != firstPath || !firstStateLabel->text().contains("已连接"))
    {
        QTextStream(stderr) << "First remote tab did not preserve connected state" << Qt::endl;
        return false;
    }

    remoteTabs->setCurrentIndex(secondIndex);
    QApplication::processEvents();
    disconnectAction->trigger();
    QApplication::processEvents();
    auto *secondStateLabel = remoteTabs->currentWidget()->findChild<QLabel *>("remoteStateLabel");
    if (secondStateLabel == nullptr || !secondStateLabel->text().contains("断开"))
    {
        QTextStream(stderr) << "Disconnect did not affect current second remote tab" << Qt::endl;
        return false;
    }

    remoteTabs->setCurrentIndex(firstIndex);
    QApplication::processEvents();
    firstStateLabel = remoteTabs->currentWidget()->findChild<QLabel *>("remoteStateLabel");
    if (firstStateLabel == nullptr || !firstStateLabel->text().contains("已连接"))
    {
        QTextStream(stderr) << "Disconnect leaked into first remote tab" << Qt::endl;
        return false;
    }

    return true;
}

/**
 * @brief Runs the remote UI workflow smoke test against the in-memory fake backend.
 * @param window Main window under test.
 * @return true when the fake workflow passes.
 */
bool checkRemoteUiWorkflow(MainWindow &window)
{
    const bool baseWorkflowOk = checkRemoteUiWorkflow(
        window,
        "SFTP",
        "fake-host",
        "22",
        "testuser",
        "",
        "/home/testuser/remote_test",
        {"download", "upload", "edit", "readme.txt"},
        true);
    return baseWorkflowOk && checkRemoteMultiSessionWorkflow(window);
}

/**
 * @brief Runs the remote UI workflow smoke test against a real FTP/SFTP server.
 * @param window Main window under test.
 * @return true when the live connection and navigation workflow passes.
 */
bool checkLiveRemoteUiWorkflow(MainWindow &window)
{
    const QString protocol = environmentValue("DIRBRIDGE_TEST_PROTOCOL").toUpper();
    const QString host = environmentValue("DIRBRIDGE_TEST_HOST");
    const QString port = environmentValue("DIRBRIDGE_TEST_PORT");
    const QString user = environmentValue("DIRBRIDGE_TEST_USER");
    const QString password = environmentValue("DIRBRIDGE_TEST_PASSWORD");
    const QString remotePath = environmentValue("DIRBRIDGE_TEST_PATH");
    if (protocol.isEmpty() || host.isEmpty() || port.isEmpty() || user.isEmpty() || password.isEmpty() || remotePath.isEmpty())
    {
        return false;
    }

    const QString expectedCsv = environmentValue("DIRBRIDGE_TEST_EXPECTED_DIRS", false);
    const QStringList expectedNames = expectedCsv.isEmpty()
        ? QStringList({"download", "edit", "update"})
        : expectedCsv.split(',', Qt::SkipEmptyParts);

    return checkRemoteUiWorkflow(window, protocol, host, port, user, password, remotePath, expectedNames, false);
}

/**
 * @brief Connects the main window to a real remote server through the quick-connect UI.
 * @param window Main window under test.
 * @param protocol Protocol text selected in the quick-connect combo box.
 * @param host Remote host.
 * @param port Remote port.
 * @param user Remote username.
 * @param password Remote password.
 * @param remotePath Initial remote path to load.
 * @return true when the remote panel reaches the connected state.
 */
bool connectLiveRemoteUi(MainWindow &window, const QString &protocol, const QString &host, const QString &port, const QString &user, const QString &password, const QString &remotePath)
{
    QComboBox *protocolCombo = window.findChild<QComboBox *>("quickProtocolCombo");
    QLineEdit *hostEdit = window.findChild<QLineEdit *>("quickHostEdit");
    QLineEdit *portEdit = window.findChild<QLineEdit *>("quickPortEdit");
    QLineEdit *userEdit = window.findChild<QLineEdit *>("quickUserEdit");
    QLineEdit *passwordEdit = window.findChild<QLineEdit *>("quickPasswordEdit");
    QLineEdit *quickRemotePathEdit = window.findChild<QLineEdit *>("quickRemotePathEdit");
    QPushButton *connectButton = window.findChild<QPushButton *>("quickConnectButton");
    QLineEdit *remotePathEdit = window.findChild<QLineEdit *>("remotePathEdit");
    QLabel *remoteStateLabel = window.findChild<QLabel *>("remoteStateLabel");

    if (protocolCombo == nullptr || hostEdit == nullptr || portEdit == nullptr || userEdit == nullptr
        || passwordEdit == nullptr || quickRemotePathEdit == nullptr || connectButton == nullptr
        || remotePathEdit == nullptr || remoteStateLabel == nullptr)
    {
        QTextStream(stderr) << "Live remote UI connect prerequisites are incomplete" << Qt::endl;
        return false;
    }

    protocolCombo->setCurrentText(protocol);
    hostEdit->setText(host);
    portEdit->setText(port);
    userEdit->setText(user);
    passwordEdit->setText(password);
    quickRemotePathEdit->setText(remotePath);
    connectButton->click();
    QApplication::processEvents();

    if (remotePathEdit->text() != remotePath || !remoteStateLabel->text().contains("已连接"))
    {
        QTextStream(stderr) << "Live remote UI connect did not reach connected state" << Qt::endl;
        return false;
    }

    return true;
}

/**
 * @brief Builds a live-test site profile from environment variables.
 * @return Site profile for the current live FTP/SFTP smoke test.
 */
SiteProfile liveProfileFromEnvironment()
{
    SiteProfile profile;
    profile.protocol = remoteProtocolFromString(environmentValue("DIRBRIDGE_TEST_PROTOCOL").toStdString());
    profile.host = environmentValue("DIRBRIDGE_TEST_HOST").toStdString();
    profile.port = static_cast<std::uint16_t>(environmentValue("DIRBRIDGE_TEST_PORT").toUShort());
    profile.username = environmentValue("DIRBRIDGE_TEST_USER").toStdString();
    profile.password = environmentValue("DIRBRIDGE_TEST_PASSWORD").toStdString();
    profile.defaultRemotePath = environmentValue("DIRBRIDGE_TEST_PATH").toStdString();
    profile.name = "live-ui-smoke";
    profile.encoding = "UTF-8";
    return profile;
}

/**
 * @brief Runs the live refresh smoke test by mutating the server outside the UI.
 * @param window Main window under test.
 * @return true when refresh shows external create and delete changes.
 */
bool checkLiveRemoteRefreshWorkflow(MainWindow &window)
{
    const QString protocol = environmentValue("DIRBRIDGE_TEST_PROTOCOL").toUpper();
    const QString host = environmentValue("DIRBRIDGE_TEST_HOST");
    const QString port = environmentValue("DIRBRIDGE_TEST_PORT");
    const QString user = environmentValue("DIRBRIDGE_TEST_USER");
    const QString password = environmentValue("DIRBRIDGE_TEST_PASSWORD");
    const QString remotePath = environmentValue("DIRBRIDGE_TEST_PATH");
    if (protocol.isEmpty() || host.isEmpty() || port.isEmpty() || user.isEmpty() || password.isEmpty() || remotePath.isEmpty())
    {
        return false;
    }
    if (!connectLiveRemoteUi(window, protocol, host, port, user, password, remotePath))
    {
        return false;
    }

    QTableWidget *remoteTable = window.findChild<QTableWidget *>("remoteFileTable");
    QPushButton *refreshButton = window.findChild<QPushButton *>("remoteRefreshButton");
    if (remoteTable == nullptr || refreshButton == nullptr)
    {
        QTextStream(stderr) << "Live refresh UI prerequisites are incomplete" << Qt::endl;
        return false;
    }

    CurlRemoteFileSystem remote;
    const SiteProfile profile = liveProfileFromEnvironment();
    RemoteOperationResult result = remote.connect(profile);
    if (!result.success)
    {
        QTextStream(stderr) << "Live refresh backend connect failed: " << QString::fromStdString(result.message) << Qt::endl;
        return false;
    }

    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const QString directoryName = QString("dirbridge_ui_refresh_%1").arg(millis);
    const QString directoryPath = joinRemotePathForCheck(remotePath, directoryName);

    result = remote.createDirectory(directoryPath.toStdString());
    if (!result.success)
    {
        QTextStream(stderr) << "Live refresh create failed: " << QString::fromStdString(result.message) << Qt::endl;
        return false;
    }

    refreshButton->click();
    QApplication::processEvents();
    if (findTableRowByName(remoteTable, directoryName) < 0)
    {
        QTextStream(stderr) << "Refresh did not show externally created directory" << Qt::endl;
        remote.remove(directoryPath.toStdString());
        return false;
    }

    result = remote.remove(directoryPath.toStdString());
    if (!result.success)
    {
        QTextStream(stderr) << "Live refresh cleanup failed: " << QString::fromStdString(result.message) << Qt::endl;
        return false;
    }

    refreshButton->click();
    QApplication::processEvents();
    if (findTableRowByName(remoteTable, directoryName) >= 0)
    {
        QTextStream(stderr) << "Refresh did not remove externally deleted directory" << Qt::endl;
        return false;
    }

    return true;
}

/**
 * @brief Runs the live upload/download smoke test through the main window transfer UI.
 * @param window Main window under test.
 * @return true when upload, download, transfer-table updates, and cleanup pass.
 */
bool checkLiveRemoteTransferWorkflow(MainWindow &window)
{
    const QString protocol = environmentValue("DIRBRIDGE_TEST_PROTOCOL").toUpper();
    const QString host = environmentValue("DIRBRIDGE_TEST_HOST");
    const QString port = environmentValue("DIRBRIDGE_TEST_PORT");
    const QString user = environmentValue("DIRBRIDGE_TEST_USER");
    const QString password = environmentValue("DIRBRIDGE_TEST_PASSWORD");
    const QString remotePath = environmentValue("DIRBRIDGE_TEST_PATH");
    if (protocol.isEmpty() || host.isEmpty() || port.isEmpty() || user.isEmpty() || password.isEmpty() || remotePath.isEmpty())
    {
        return false;
    }

    if (!connectLiveRemoteUi(window, protocol, host, port, user, password, remotePath))
    {
        return false;
    }

    QTableWidget *remoteTable = window.findChild<QTableWidget *>("remoteFileTable");
    QTreeWidget *transferTable = window.findChild<QTreeWidget *>("transferTable");
    if (remoteTable == nullptr || transferTable == nullptr)
    {
        QTextStream(stderr) << "Live transfer UI prerequisites are incomplete" << Qt::endl;
        return false;
    }

    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const QString fileName = QString("dirbridge_ui_transfer_%1.txt").arg(millis);
    const std::filesystem::path localTestRoot = std::filesystem::temp_directory_path() / "dirbridge-ui-transfer-smoke";
    std::filesystem::create_directories(localTestRoot);
    window.setLocalPathForTesting(QString::fromStdString(localTestRoot.string()));

    const std::filesystem::path localUploadPath = localTestRoot / fileName.toStdString();
    {
        std::ofstream output(localUploadPath, std::ios::binary | std::ios::trunc);
        output << "DirBridge live UI transfer smoke\n";
    }

    const QString remoteFilePath = joinRemotePathForCheck(remotePath, fileName);
    window.uploadLocalFileForTesting(QString::fromStdString(localUploadPath.string()));
    QApplication::processEvents();
    if (!hasTransferRow(transferTable, fileName, "上传", "已完成"))
    {
        QTextStream(stderr) << "Upload transfer row was not completed" << Qt::endl;
        return false;
    }
    if (findTableRowByName(remoteTable, fileName) < 0)
    {
        QTextStream(stderr) << "Uploaded file is not visible in remote table" << Qt::endl;
        return false;
    }

    window.downloadRemoteFileForTesting(remoteFilePath);
    QApplication::processEvents();
    if (!hasTransferRow(transferTable, fileName, "下载", "已完成"))
    {
        QTextStream(stderr) << "Download transfer row was not completed" << Qt::endl;
        return false;
    }

    window.removeRemotePathForTesting(remoteFilePath);
    QApplication::processEvents();
    if (findTableRowByName(remoteTable, fileName) >= 0)
    {
        QTextStream(stderr) << "Remote cleanup did not remove uploaded file from table" << Qt::endl;
        return false;
    }

    QAction *disconnectAction = findActionByText(window, "断开");
    if (disconnectAction != nullptr)
    {
        disconnectAction->trigger();
        QApplication::processEvents();
    }

    return true;
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("DirBridge");
    QApplication::setApplicationVersion("0.1.0");

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
