#include "app/UiSmokeTests.h"

#include "config/SiteProfile.h"
#include "core/FakeRemoteFileSystem.h"
#include "protocol/CurlRemoteFileSystem.h"
#include "ui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTabWidget>
#include <QTextStream>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

QAction *findActionByText(MainWindow &window, const QString &text);

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
    ok = requireChild<QTabWidget>(window, "remoteTabs") && ok;
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
    ok = requireChild<QTreeWidget>(window, "logView") && ok;
    ok = requireChild<QTreeWidget>(window, "sessionManagerTree") && ok;

    QTabWidget *remoteTabs = window.findChild<QTabWidget *>("remoteTabs");
    if (remoteTabs != nullptr && remoteTabs->count() != 0)
    {
        QTextStream(stderr) << "Remote tabs should start without placeholder pages" << Qt::endl;
        ok = false;
    }
    if (findActionByText(window, "关于 DirBridge") == nullptr)
    {
        QTextStream(stderr) << "About DirBridge action is missing" << Qt::endl;
        ok = false;
    }

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

    if (transferTable != nullptr)
    {
        if (transferTable->columnCount() < 10)
        {
            QTextStream(stderr) << "Transfer table column count is less than 10" << Qt::endl;
            ok = false;
        }
        if (transferTable->topLevelItemCount() != 0)
        {
            QTextStream(stderr) << "Transfer table should not contain placeholder rows" << Qt::endl;
            ok = false;
        }

        if (cancelTransferButton != nullptr && cancelTransferButton->isEnabled())
        {
            QTextStream(stderr) << "Cancel button should be disabled without a selected transfer" << Qt::endl;
            ok = false;
        }
        if (retryTransferButton != nullptr && retryTransferButton->isEnabled())
        {
            QTextStream(stderr) << "Retry button should be disabled without a selected transfer" << Qt::endl;
            ok = false;
        }
        if (clearFinishedButton != nullptr && clearFinishedButton->isEnabled())
        {
            QTextStream(stderr) << "Clear finished button should be disabled without transfer history" << Qt::endl;
            ok = false;
        }
    }

    return ok;
}

/**
 * @brief Pumps the Qt event loop until a remote panel reports the requested connected path.
 * @param remotePanel Panel widget that owns the remote path and state labels.
 * @param remotePath Expected connected remote path.
 * @return true when the panel reaches the connected state before the timeout.
 */
bool waitForRemoteConnected(QWidget *remotePanel, const QString &remotePath)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        QLineEdit *remotePathEdit = remotePanel == nullptr ? nullptr : remotePanel->findChild<QLineEdit *>("remotePathEdit");
        QLabel *remoteStateLabel = remotePanel == nullptr ? nullptr : remotePanel->findChild<QLabel *>("remoteStateLabel");
        if (remotePathEdit != nullptr && remoteStateLabel != nullptr
            && remotePathEdit->text() == remotePath
            && (remoteStateLabel->text().contains("个项目") || remoteStateLabel->text().contains("已连接")))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

/**
 * @brief Pumps the Qt event loop until a remote panel reports a disconnected or canceled state.
 * @param remotePanel Panel widget that owns the remote state label.
 * @return true when the panel leaves the connecting state before the timeout.
 */
bool waitForRemoteDisconnected(QWidget *remotePanel)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        QLabel *remoteStateLabel = remotePanel == nullptr ? nullptr : remotePanel->findChild<QLabel *>("remoteStateLabel");
        if (remoteStateLabel != nullptr
            && (remoteStateLabel->text().contains("断开")
                || remoteStateLabel->text().contains("已取消")
                || remoteStateLabel->text().contains("连接已取消")))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

class SlowFakeRemoteFileSystem : public FakeRemoteFileSystem
{
public:
    RemoteOperationResult connect(const SiteProfile &profile) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return FakeRemoteFileSystem::connect(profile);
    }

    std::vector<FileItem> listDirectory(const std::string &path) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return FakeRemoteFileSystem::listDirectory(path);
    }
};

/**
 * @brief Checks whether a tree contains visible text.
 * @param tree Tree widget to scan.
 * @param text Text fragment to find.
 * @return true when any item contains the fragment.
 */
bool treeContainsText(QTreeWidget *tree, const QString &text)
{
    if (tree == nullptr)
    {
        return false;
    }

    QTreeWidgetItemIterator iterator(tree);
    while (*iterator != nullptr)
    {
        if ((*iterator)->text(0).contains(text))
        {
            return true;
        }
        ++iterator;
    }
    return false;
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

bool remoteTreeContainsText(QTreeWidget *tree, const QString &text)
{
    if (tree == nullptr)
    {
        return false;
    }

    QTreeWidgetItemIterator iterator(tree);
    while (*iterator != nullptr)
    {
        if ((*iterator)->text(0) == text)
        {
            return true;
        }
        ++iterator;
    }
    return false;
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

    QTreeWidgetItemIterator iterator(table);
    while (*iterator != nullptr)
    {
        QTreeWidgetItem *item = *iterator;
        if (item != nullptr
            && item->text(0) == name
            && item->text(2) == direction
            && item->text(3) == status)
        {
            return true;
        }
        ++iterator;
    }

    return false;
}

/**
 * @brief Verifies that a file-panel navigation button is icon-only but still discoverable.
 * @param panel Panel that owns the button.
 * @param objectName Expected button object name.
 * @return true when the button keeps icon and tooltip while hiding text.
 */
bool checkIconOnlyNavigationButton(QWidget *panel, const char *objectName)
{
    QPushButton *button = panel == nullptr ? nullptr : panel->findChild<QPushButton *>(objectName);
    if (button == nullptr)
    {
        QTextStream(stderr) << "Missing navigation button: " << objectName << Qt::endl;
        return false;
    }
    if (!button->text().isEmpty())
    {
        QTextStream(stderr) << "Navigation button should be icon-only: " << objectName << Qt::endl;
        return false;
    }
    if (button->icon().isNull() || button->toolTip().isEmpty())
    {
        QTextStream(stderr) << "Navigation button should keep icon and tooltip: " << objectName << Qt::endl;
        return false;
    }
    return true;
}

QTreeWidgetItem *findTopLevelTransferRow(QTreeWidget *table, const QString &name, const QString &direction, const QString &status)
{
    if (table == nullptr)
    {
        return nullptr;
    }

    for (int index = 0; index < table->topLevelItemCount(); ++index)
    {
        QTreeWidgetItem *item = table->topLevelItem(index);
        if (item != nullptr
            && item->text(0) == name
            && item->text(2) == direction
            && item->text(3) == status)
        {
            return item;
        }
    }

    return nullptr;
}

void dumpTransferRows(QTreeWidget *table)
{
    if (table == nullptr)
    {
        QTextStream(stderr) << "Transfer table is missing" << Qt::endl;
        return;
    }

    QTreeWidgetItemIterator iterator(table);
    while (*iterator != nullptr)
    {
        QTreeWidgetItem *item = *iterator;
        QTextStream(stderr)
            << "Transfer row: "
            << item->text(0) << " | "
            << item->text(2) << " | "
            << item->text(3) << " | "
            << item->text(4) << " | "
            << item->text(8) << " | "
            << item->text(9)
            << Qt::endl;
        ++iterator;
    }
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
    QTabWidget *remoteTabs = window.findChild<QTabWidget *>("remoteTabs");
    QAction *disconnectAction = findActionByText(window, "断开");

    if (protocolCombo == nullptr || hostEdit == nullptr || portEdit == nullptr || userEdit == nullptr
        || passwordEdit == nullptr || quickRemotePathEdit == nullptr || connectButton == nullptr
        || remoteTabs == nullptr || disconnectAction == nullptr)
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

    if (!waitForRemoteConnected(remoteTabs->currentWidget(), remotePath))
    {
        QTextStream(stderr) << "Remote path after connect is unexpected" << Qt::endl;
        ok = false;
    }
    QWidget *remotePanel = remoteTabs->currentWidget();
    QLineEdit *remotePathEdit = remotePanel == nullptr ? nullptr : remotePanel->findChild<QLineEdit *>("remotePathEdit");
    QLabel *remoteStateLabel = remotePanel == nullptr ? nullptr : remotePanel->findChild<QLabel *>("remoteStateLabel");
    QTableWidget *remoteTable = remotePanel == nullptr ? nullptr : remotePanel->findChild<QTableWidget *>("remoteFileTable");
    QTreeWidget *remoteTree = remotePanel == nullptr ? nullptr : remotePanel->findChild<QTreeWidget *>("remoteFileTree");
    QPushButton *remoteUpButton = remotePanel == nullptr ? nullptr : remotePanel->findChild<QPushButton *>("remoteUpButton");
    if (remotePathEdit == nullptr || remoteStateLabel == nullptr || remoteTable == nullptr || remoteTree == nullptr || remoteUpButton == nullptr)
    {
        QTextStream(stderr) << "Connected remote panel prerequisites are incomplete" << Qt::endl;
        return false;
    }
    ok = checkIconOnlyNavigationButton(remotePanel, "remoteBackButton") && ok;
    ok = checkIconOnlyNavigationButton(remotePanel, "remoteForwardButton") && ok;
    ok = checkIconOnlyNavigationButton(remotePanel, "remoteUpButton") && ok;
    if (!remoteStateLabel->text().contains("个项目"))
    {
        QTextStream(stderr) << "Remote state label does not show item count: " << remoteStateLabel->text() << Qt::endl;
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
    for (const QString &name : expectedNames)
    {
        if (name != "readme.txt" && !remoteTreeContainsText(remoteTree, name))
        {
            QTextStream(stderr) << "Remote tree does not show expected sibling directory: " << name << Qt::endl;
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
    if (!waitForRemoteConnected(currentRemotePanel, remotePath))
    {
        QTextStream(stderr) << "New fake remote session did not connect to expected path" << Qt::endl;
        return false;
    }
    return true;
}

/**
 * @brief Verifies connecting state, duplicate-connect prevention, and logical cancellation.
 * @param window Main window under test.
 * @return true when the UI exposes and clears an in-flight connection correctly.
 */
bool checkRemoteConnectionControlWorkflow(MainWindow &window)
{
    window.setRemoteFileSystemForTesting(std::make_unique<SlowFakeRemoteFileSystem>());

    QComboBox *protocolCombo = window.findChild<QComboBox *>("quickProtocolCombo");
    QLineEdit *hostEdit = window.findChild<QLineEdit *>("quickHostEdit");
    QLineEdit *portEdit = window.findChild<QLineEdit *>("quickPortEdit");
    QLineEdit *userEdit = window.findChild<QLineEdit *>("quickUserEdit");
    QLineEdit *passwordEdit = window.findChild<QLineEdit *>("quickPasswordEdit");
    QLineEdit *quickRemotePathEdit = window.findChild<QLineEdit *>("quickRemotePathEdit");
    QPushButton *connectButton = window.findChild<QPushButton *>("quickConnectButton");
    QTabWidget *remoteTabs = window.findChild<QTabWidget *>("remoteTabs");
    QAction *connectAction = findActionByText(window, "连接");
    if (protocolCombo == nullptr || hostEdit == nullptr || portEdit == nullptr || userEdit == nullptr
        || passwordEdit == nullptr || quickRemotePathEdit == nullptr || connectButton == nullptr
        || remoteTabs == nullptr || connectAction == nullptr)
    {
        QTextStream(stderr) << "Remote connection-control prerequisites are incomplete" << Qt::endl;
        return false;
    }

    const int initialTabCount = remoteTabs->count();
    protocolCombo->setCurrentText("SFTP");
    hostEdit->setText("slow-fake-host");
    portEdit->setText("22");
    userEdit->setText("testuser");
    passwordEdit->clear();
    quickRemotePathEdit->setText("/home/testuser/remote_test");
    connectButton->click();
    QApplication::processEvents();

    QWidget *connectingPanel = remoteTabs->currentWidget();
    QLabel *remoteStateLabel = connectingPanel == nullptr ? nullptr : connectingPanel->findChild<QLabel *>("remoteStateLabel");
    if (remoteTabs->count() != initialTabCount + 1 || remoteStateLabel == nullptr
        || !remoteStateLabel->text().contains("正在连接") || connectButton->text() != "取消")
    {
        QTextStream(stderr) << "Remote connection did not expose connecting state" << Qt::endl;
        return false;
    }

    connectAction->trigger();
    QApplication::processEvents();
    if (remoteTabs->count() != initialTabCount + 1)
    {
        QTextStream(stderr) << "Duplicate connect should not create another remote tab" << Qt::endl;
        return false;
    }

    connectButton->click();
    if (!waitForRemoteDisconnected(connectingPanel) || connectButton->text() != "连接")
    {
        QTextStream(stderr) << "Remote connection cancellation did not reset UI state" << Qt::endl;
        return false;
    }

    return true;
}

/**
 * @brief Verifies v0.5.0 session manager grouping and recent-session behavior.
 * @param window Main window under test.
 * @return true when saved sites, grouping, and recent sessions work with fake remote tabs.
 */
bool checkSessionManagerWorkflow(MainWindow &window)
{
    QTreeWidget *sessionTree = window.findChild<QTreeWidget *>("sessionManagerTree");
    QTabWidget *remoteTabs = window.findChild<QTabWidget *>("remoteTabs");
    if (sessionTree == nullptr || remoteTabs == nullptr)
    {
        QTextStream(stderr) << "Session manager prerequisites are incomplete" << Qt::endl;
        return false;
    }

    const int initialTabCount = remoteTabs->count();

    SiteProfile grouped;
    grouped.id = "ui-grouped-site";
    grouped.name = "UI Grouped Site";
    grouped.group = "生产";
    grouped.protocol = RemoteProtocol::Sftp;
    grouped.host = "fake-host";
    grouped.port = 22;
    grouped.username = "testuser";
    grouped.defaultRemotePath = "/home/testuser/remote_test";
    grouped.encoding = "UTF-8";
    window.saveSiteForTesting(grouped);

    SiteProfile ungrouped = grouped;
    ungrouped.id = "ui-ungrouped-site";
    ungrouped.name = "UI Ungrouped Site";
    ungrouped.group.clear();
    window.saveSiteForTesting(ungrouped);
    QApplication::processEvents();

    if (!treeContainsText(sessionTree, "生产") || !treeContainsText(sessionTree, "未分组")
        || !treeContainsText(sessionTree, grouped.name.c_str()))
    {
        QTextStream(stderr) << "Session manager did not show saved site groups" << Qt::endl;
        return false;
    }
    if (!window.renameSiteGroupForTesting("生产", "运维"))
    {
        QTextStream(stderr) << "Session manager group rename did not update sites" << Qt::endl;
        return false;
    }
    QApplication::processEvents();
    if (!treeContainsText(sessionTree, "运维") || !treeContainsText(sessionTree, grouped.name.c_str()))
    {
        QTextStream(stderr) << "Session manager did not show renamed group" << Qt::endl;
        return false;
    }
    if (remoteTabs->count() != initialTabCount)
    {
        QTextStream(stderr) << "Saved sites should not restore remote tabs automatically" << Qt::endl;
        return false;
    }

    window.setRemoteFileSystemForTesting(std::make_unique<FakeRemoteFileSystem>());
    QTreeWidgetItemIterator iterator(sessionTree);
    QTreeWidgetItem *groupedItem = nullptr;
    while (*iterator != nullptr)
    {
        if ((*iterator)->text(0).contains("UI Grouped Site"))
        {
            groupedItem = *iterator;
            break;
        }
        ++iterator;
    }
    if (groupedItem == nullptr)
    {
        QTextStream(stderr) << "Grouped site item is missing" << Qt::endl;
        return false;
    }
    sessionTree->setCurrentItem(groupedItem);
    QMetaObject::invokeMethod(sessionTree, "itemDoubleClicked", Qt::DirectConnection, Q_ARG(QTreeWidgetItem *, groupedItem), Q_ARG(int, 0));
    QApplication::processEvents();

    if (!waitForRemoteConnected(remoteTabs->currentWidget(), "/home/testuser/remote_test"))
    {
        QTextStream(stderr) << "Session manager site double click did not connect remote session" << Qt::endl;
        return false;
    }

    if (remoteTabs->count() != initialTabCount + 1 || !treeContainsText(sessionTree, "最近会话")
        || !treeContainsText(sessionTree, "/home/testuser/remote_test") || !treeContainsText(sessionTree, "当前"))
    {
        QTextStream(stderr) << "Session manager did not record current recent session" << Qt::endl;
        return false;
    }
    if (!window.renameSiteGroupForTesting("运维", "运行组"))
    {
        QTextStream(stderr) << "Session manager group rename did not update active sessions" << Qt::endl;
        return false;
    }
    QApplication::processEvents();
    if (!treeContainsText(sessionTree, "运行组") || !treeContainsText(sessionTree, "当前"))
    {
        QTextStream(stderr) << "Renamed group did not keep current session state" << Qt::endl;
        return false;
    }

    const int connectedIndex = remoteTabs->currentIndex();
    QMetaObject::invokeMethod(remoteTabs, "tabCloseRequested", Qt::DirectConnection, Q_ARG(int, connectedIndex));
    QApplication::processEvents();
    if (remoteTabs->count() != initialTabCount)
    {
        QTextStream(stderr) << "Remote tab close did not remove current session tab" << Qt::endl;
        return false;
    }

    window.removeSiteForTesting(grouped.id);
    QApplication::processEvents();
    if (treeContainsText(sessionTree, "UI Grouped Site") && !treeContainsText(sessionTree, "站点已删除"))
    {
        QTextStream(stderr) << "Deleted recent session should be marked unavailable" << Qt::endl;
        return false;
    }

    window.removeSiteForTesting(ungrouped.id);
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
    if (firstPathEdit == nullptr || firstStateLabel == nullptr || firstPathEdit->text() != firstPath || !firstStateLabel->text().contains("个项目"))
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
    if (firstStateLabel == nullptr || !firstStateLabel->text().contains("个项目"))
    {
        QTextStream(stderr) << "Disconnect leaked into first remote tab" << Qt::endl;
        return false;
    }

    return true;
}

/**
 * @brief Verifies directory upload/download, remote move, and recursive remote delete with the fake backend.
 * @param window Main window under test.
 * @return true when directory operations complete through normal MainWindow workflows.
 */
bool checkRemoteDirectoryOperationWorkflow(MainWindow &window)
{
    const QString remotePath = "/home/testuser/remote_test";
    if (!connectFakeRemoteSession(window, remotePath))
    {
        return false;
    }

    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "dirbridge-ui-directory-smoke";
    const std::filesystem::path uploadRoot = tempRoot / "upload-src";
    const std::filesystem::path downloadRoot = tempRoot / "download-target";
    const std::filesystem::path localDirectory = uploadRoot / "dirbridge-folder";
    const std::filesystem::path nestedDirectory = localDirectory / "nested";
    const std::filesystem::path deepDirectory = nestedDirectory / "level2" / "level3";
    const std::filesystem::path nestedFile = nestedDirectory / "inside.txt";
    const std::filesystem::path deepFile = deepDirectory / "deep.txt";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(deepDirectory);
    {
        std::ofstream output(nestedFile, std::ios::binary | std::ios::trunc);
        output << "DirBridge directory smoke\n";
    }
    {
        std::ofstream output(deepFile, std::ios::binary | std::ios::trunc);
        output << "DirBridge deep directory smoke\n";
    }
    std::filesystem::create_directories(downloadRoot);
    window.setLocalPathForTesting(QString::fromStdString(downloadRoot.u8string()));

    window.uploadLocalPathForTesting(QString::fromStdString(localDirectory.u8string()));
    QApplication::processEvents();
    window.uploadLocalPathForTesting(QString::fromStdString(localDirectory.u8string()));
    QApplication::processEvents();

    const QString uploadedDirectory = "/home/testuser/remote_test/dirbridge-folder";
    const QString movedDirectory = "/home/testuser/remote_test/upload/dirbridge-folder";
    window.moveRemotePathsForTesting({uploadedDirectory}, "/home/testuser/remote_test/upload");
    QApplication::processEvents();

    window.downloadRemotePathForTesting(movedDirectory);
    QApplication::processEvents();
    QTreeWidget *transferTable = window.findChild<QTreeWidget *>("transferTable");
    const std::filesystem::path downloadedFile = downloadRoot / "dirbridge-folder" / "nested" / "inside.txt";
    if (!std::filesystem::is_regular_file(downloadedFile))
    {
        QTextStream(stderr) << "Downloaded remote directory does not contain nested file" << Qt::endl;
        dumpTransferRows(transferTable);
        return false;
    }
    const std::filesystem::path downloadedDeepFile = downloadRoot / "dirbridge-folder" / "nested" / "level2" / "level3" / "deep.txt";
    if (!std::filesystem::is_regular_file(downloadedDeepFile))
    {
        QTextStream(stderr) << "Downloaded remote directory does not contain deep nested file" << Qt::endl;
        dumpTransferRows(transferTable);
        return false;
    }
    QTreeWidgetItem *uploadParent = findTopLevelTransferRow(transferTable, "dirbridge-folder", "上传", "已完成");
    QTreeWidgetItem *downloadParent = findTopLevelTransferRow(transferTable, "dirbridge-folder", "下载", "已完成");
    if (uploadParent == nullptr || uploadParent->text(4) != "100%" || uploadParent->childCount() < 2)
    {
        QTextStream(stderr) << "Directory upload parent transfer row is incomplete" << Qt::endl;
        return false;
    }
    if (downloadParent == nullptr || downloadParent->text(4) != "100%" || downloadParent->childCount() < 2)
    {
        QTextStream(stderr) << "Directory download parent transfer row is incomplete" << Qt::endl;
        return false;
    }

    window.removeRemotePathForTesting(movedDirectory);
    QApplication::processEvents();

    QLineEdit *remotePathEdit = window.findChild<QLineEdit *>("remotePathEdit");
    QTableWidget *remoteTable = window.findChild<QTableWidget *>("remoteFileTable");
    if (remotePathEdit == nullptr || remoteTable == nullptr)
    {
        QTextStream(stderr) << "Directory operation smoke UI objects are missing" << Qt::endl;
        return false;
    }

    remotePathEdit->setText("/home/testuser/remote_test/upload");
    QMetaObject::invokeMethod(remotePathEdit, "returnPressed", Qt::DirectConnection);
    QApplication::processEvents();
    if (findTableRowByName(remoteTable, "dirbridge-folder") >= 0)
    {
        QTextStream(stderr) << "Recursive remote delete did not remove moved directory" << Qt::endl;
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
    return baseWorkflowOk
        && checkRemoteConnectionControlWorkflow(window)
        && checkSessionManagerWorkflow(window)
        && checkRemoteMultiSessionWorkflow(window)
        && checkRemoteDirectoryOperationWorkflow(window);
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
    QTabWidget *remoteTabs = window.findChild<QTabWidget *>("remoteTabs");

    if (protocolCombo == nullptr || hostEdit == nullptr || portEdit == nullptr || userEdit == nullptr
        || passwordEdit == nullptr || quickRemotePathEdit == nullptr || connectButton == nullptr
        || remoteTabs == nullptr)
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

    if (!waitForRemoteConnected(remoteTabs->currentWidget(), remotePath))
    {
        QTextStream(stderr) << "Live remote UI connect did not reach connected state" << Qt::endl;
        return false;
    }
    QWidget *remotePanel = remoteTabs->currentWidget();
    QLineEdit *remotePathEdit = remotePanel == nullptr ? nullptr : remotePanel->findChild<QLineEdit *>("remotePathEdit");
    QLabel *remoteStateLabel = remotePanel == nullptr ? nullptr : remotePanel->findChild<QLabel *>("remoteStateLabel");
    if (remotePathEdit == nullptr || remoteStateLabel == nullptr || remotePathEdit->text() != remotePath || !remoteStateLabel->text().contains("个项目"))
    {
        QTextStream(stderr) << "Live remote connected panel is incomplete" << Qt::endl;
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

