#include "app/UiSmokeTests.h"

#include "config/SiteProfile.h"
#include "core/DependencyCheck.h"
#include "core/FakeRemoteFileSystem.h"
#include "protocol/CurlRemoteFileSystem.h"
#include "ui/FileChangeMonitor.h"
#include "ui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QStatusBar>
#include <QStringList>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTabWidget>
#include <QTabBar>
#include <QTextStream>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

QAction *findActionByText(MainWindow &window, const QString &text);
bool checkAboutDialog(MainWindow &window);

/**
 * @brief 按对象名查找必须存在的子控件，缺失时输出冒烟测试错误。
 * @param window 要搜索的主窗口。
 * @param objectName 冒烟测试期望存在的 Qt 对象名。
 * @return 找到子控件时返回 true。
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
 * @brief 验证远程工作流相关控件和快速连接默认值是否存在。
 * @param window 待测试的主窗口。
 * @return 所有必需 UI 对象和默认值都存在时返回 true。
 */
bool checkRemoteUiObjects(MainWindow &window)
{
    bool ok = true;
    {
        QTemporaryDir temporaryDirectory;
        if (!temporaryDirectory.isValid())
        {
            QTextStream(stderr) << "Unable to create temporary directory for external edit monitor smoke test" << Qt::endl;
            ok = false;
        }
        else
        {
            const QString filePath = temporaryDirectory.filePath("remote-edit.json");
            {
                QSaveFile initialFile(filePath);
                if (!initialFile.open(QIODevice::WriteOnly) || initialFile.write("{\"version\":0}") < 0 || !initialFile.commit())
                {
                    QTextStream(stderr) << "Unable to prepare external edit monitor smoke file" << Qt::endl;
                    ok = false;
                }
            }

            if (ok)
            {
                FileChangeMonitor monitor;
                int stableChangeCount = 0;
                QEventLoop loop;
                QTimer timeout;
                timeout.setSingleShot(true);
                QObject::connect(&monitor, &FileChangeMonitor::stableFileChanged, &loop, [&](const QString &changedPath) {
                    if (changedPath == filePath)
                    {
                        ++stableChangeCount;
                    }
                });
                QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
                monitor.startMonitoring(filePath);

                const auto replaceFile = [filePath](const QByteArray &content) {
                    QSaveFile replacement(filePath);
                    return replacement.open(QIODevice::WriteOnly)
                        && replacement.write(content) == content.size()
                        && replacement.commit();
                };
                QTimer::singleShot(0, &loop, [replaceFile]() {
                    replaceFile("{\"version\":1}");
                });
                QTimer::singleShot(100, &loop, [replaceFile]() {
                    replaceFile("{\"version\":2}");
                });
                timeout.start(1800);
                loop.exec();
                if (stableChangeCount != 1)
                {
                    QTextStream(stderr) << "External edit monitor should coalesce atomic saves into one stable change" << Qt::endl;
                    ok = false;
                }

                stableChangeCount = 0;
                QEventLoop unrelatedChangeLoop;
                QTimer unrelatedChangeTimeout;
                unrelatedChangeTimeout.setSingleShot(true);
                QObject::connect(&unrelatedChangeTimeout, &QTimer::timeout, &unrelatedChangeLoop, &QEventLoop::quit);
                QTimer::singleShot(0, &unrelatedChangeLoop, [&temporaryDirectory]() {
                    QSaveFile unrelatedFile(temporaryDirectory.filePath("upload-snapshot.tmp"));
                    if (unrelatedFile.open(QIODevice::WriteOnly))
                    {
                        unrelatedFile.write("snapshot");
                        unrelatedFile.commit();
                    }
                });
                unrelatedChangeTimeout.start(900);
                unrelatedChangeLoop.exec();
                if (stableChangeCount != 0)
                {
                    QTextStream(stderr) << "External edit monitor should ignore unrelated cache-directory changes" << Qt::endl;
                    ok = false;
                }
            }
        }
    }
    ok = requireChild<QComboBox>(window, "quickProtocolCombo") && ok;
    ok = requireChild<QLineEdit>(window, "quickHostEdit") && ok;
    ok = requireChild<QLineEdit>(window, "quickPortEdit") && ok;
    ok = requireChild<QLineEdit>(window, "quickUserEdit") && ok;
    ok = requireChild<QLineEdit>(window, "quickPasswordEdit") && ok;
    ok = requireChild<QLineEdit>(window, "quickRemotePathEdit") && ok;
    ok = requireChild<QPushButton>(window, "quickConnectButton") && ok;
    ok = requireChild<QTabWidget>(window, "remoteTabs") && ok;
    ok = requireChild<QTabWidget>(window, "bottomTabs") && ok;
    ok = requireChild<QTabWidget>(window, "terminalTabs") && ok;
    QTabWidget *terminalTabs = window.findChild<QTabWidget *>("terminalTabs");
    if (terminalTabs != nullptr && terminalTabs->count() != 0)
    {
        QTextStream(stderr) << "Terminal tabs should start empty" << Qt::endl;
        ok = false;
    }
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
    if (remoteTabs != nullptr && !remoteTabs->isHidden())
    {
        QTextStream(stderr) << "Remote tabs should be hidden before the first remote session" << Qt::endl;
        ok = false;
    }
    if (window.statusBar() == nullptr
        || window.statusBar()->currentMessage().contains("libcurl")
        || window.statusBar()->currentMessage().contains("JSON ready")
        || window.statusBar()->currentMessage().contains("logging ready"))
    {
        QTextStream(stderr) << "Status bar should show user-facing startup text" << Qt::endl;
        ok = false;
    }
    if (findActionByText(window, "关于 DirBridge") == nullptr)
    {
        QTextStream(stderr) << "About DirBridge action is missing" << Qt::endl;
        ok = false;
    }
    else if (!checkAboutDialog(window))
    {
        QTextStream(stderr) << "About DirBridge dialog content is unexpected" << Qt::endl;
        ok = false;
    }
    QMenuBar *menuBar = window.menuBar();
    if (menuBar == nullptr)
    {
        QTextStream(stderr) << "Menu bar is missing" << Qt::endl;
        ok = false;
    }
    else
    {
        QMenu *fileMenu = nullptr;
        for (QAction *action : menuBar->actions())
        {
            const QString text = action == nullptr ? QString() : QString(action->text()).remove('&');
            if (text.startsWith("命令"))
            {
                QTextStream(stderr) << "Command menu should be removed" << Qt::endl;
                ok = false;
            }
            if (text.startsWith("文件"))
            {
                fileMenu = action->menu();
            }
        }
        int visibleFileActions = 0;
        QString lastFileActionText;
        if (fileMenu != nullptr)
        {
            for (QAction *action : fileMenu->actions())
            {
                if (action != nullptr && !action->isSeparator())
                {
                    ++visibleFileActions;
                    lastFileActionText = QString(action->text()).remove('&');
                }
            }
        }
        if (fileMenu == nullptr || visibleFileActions != 1 || lastFileActionText != "新建站点")
        {
            QTextStream(stderr) << "File menu should only contain New Site" << Qt::endl;
            ok = false;
        }
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
        if (transferTable->headerItem() == nullptr
            || transferTable->headerItem()->text(7) != "速度"
            || transferTable->headerItem()->text(8) != "估计剩余"
            || transferTable->headerItem()->text(9) != "经过时间")
        {
            QTextStream(stderr) << "Transfer table timing headers are missing" << Qt::endl;
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
 * @brief 持续驱动 Qt 事件循环，直到远程面板显示目标已连接路径。
 * @param remotePanel 持有远程路径框和状态标签的面板控件。
 * @param remotePath 期望连接成功后的远程路径。
 * @return 面板在超时前进入已连接状态时返回 true。
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
 * @brief 持续驱动 Qt 事件循环，直到远程面板显示已断开或已取消状态。
 * @param remotePanel 持有远程状态标签的面板控件。
 * @return 面板在超时前退出连接中状态时返回 true。
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
 * @brief 检查树控件中是否包含指定可见文本。
 * @param tree 要扫描的树控件。
 * @param text 要查找的文本片段。
 * @return 任意节点包含该文本片段时返回 true。
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
 * @brief 按可见文本查找 QAction，并忽略 Qt 助记符标记。
 * @param window 要搜索动作的主窗口。
 * @param text 要匹配的可见动作文本。
 * @return 匹配到的动作；未找到时返回 nullptr。
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
 * @brief 按显示名称列在远程文件表格中查找行。
 * @param table 远程表格控件。
 * @param name 文件或目录的显示名称。
 * @return 从 0 开始的行索引；不存在时返回 -1。
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
 * @brief 检查传输表格中是否存在符合预期状态的任务行。
 * @param table 传输队列表格。
 * @param name 传输任务显示名称。
 * @param direction 本地化方向文本，例如“上传”或“下载”。
 * @param status 本地化状态文本，例如“已完成”。
 * @return 存在匹配的传输行时返回 true。
 */
bool hasTransferRow(QTreeWidget *table, const QString &name, const QString &direction, const QString &status)
{
    if (table == nullptr)
    {
        return false;
    }

    const QString arrow = direction == "上传" ? "->" : "<-";
    QTreeWidgetItemIterator iterator(table);
    while (*iterator != nullptr)
    {
        QTreeWidgetItem *item = *iterator;
        if (item != nullptr
            && item->text(0) == name
            && item->text(1) == status
            && item->text(5) == arrow)
        {
            return true;
        }
        ++iterator;
    }

    return false;
}

bool waitForTransferRow(QTreeWidget *table, const QString &name, const QString &direction, const QString &status)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        if (hasTransferRow(table, name, direction, status))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool waitForTableRowAbsent(QTableWidget *table, const QString &name)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        if (findTableRowByName(table, name) < 0)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool checkAboutDialog(MainWindow &window)
{
    QAction *aboutAction = findActionByText(window, "关于 DirBridge");
    if (aboutAction == nullptr)
    {
        return false;
    }

    bool checked = false;
    QTimer::singleShot(0, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr || dialog->objectName() != "aboutDialog")
        {
            return;
        }
        QString text;
        const QList<QLabel *> labels = dialog->findChildren<QLabel *>();
        for (QLabel *label : labels)
        {
            if (label != nullptr)
            {
                text += label->text();
            }
        }
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        checked = text.contains("版本")
            && text.contains("许可证")
            && text.contains("GitHub")
            && text.contains("作者")
            && !text.contains("阶段")
            && buttons != nullptr
            && buttons->button(QDialogButtonBox::Close)->text() == "关闭";
        if (buttons != nullptr)
        {
            buttons->button(QDialogButtonBox::Close)->click();
        }
        else
        {
            dialog->reject();
        }
    });
    aboutAction->trigger();
    QApplication::processEvents();
    return checked;
}

/**
 * @brief 验证文件面板导航按钮是否只显示图标但仍具备可发现性。
 * @param panel 持有该按钮的面板。
 * @param objectName 期望的按钮对象名。
 * @return 按钮隐藏文本但保留图标和提示时返回 true。
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

    const QString arrow = direction == "上传" ? "->" : "<-";
    for (int index = 0; index < table->topLevelItemCount(); ++index)
    {
        QTreeWidgetItem *item = table->topLevelItem(index);
        if (item != nullptr
            && item->text(0) == name
            && item->text(1) == status
            && item->text(5) == arrow)
        {
            return item;
        }
    }

    return nullptr;
}

bool hasProgressBar(QTreeWidget *table, QTreeWidgetItem *item)
{
    return table != nullptr
        && item != nullptr
        && qobject_cast<QProgressBar *>(table->itemWidget(item, 2)) != nullptr;
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
            << item->text(1) << " | "
            << item->text(2) << " | "
            << item->text(3) << " | "
            << item->text(4) << " | "
            << item->text(5) << " | "
            << item->text(6)
            << Qt::endl;
        ++iterator;
    }
}

/**
 * @brief 读取集成冒烟测试使用的环境变量。
 * @param name 环境变量名称。
 * @param required 缺失值时是否上报为测试错误。
 * @return 变量的 UTF-8 文本值；不存在时返回空字符串。
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
 * @brief 为冒烟测试路径断言拼接远程目录和子项名称。
 * @param directory 远程父目录。
 * @param name 远程子项名称。
 * @return 规范化后的远程子路径。
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
 * @brief 验证远程文件外部编辑的下载、监听和自动上传闭环。
 * @param window 已连接到假远程后端的主窗口。
 * @param remotePath 当前远程目录。
 * @param remoteTable 当前远程文件表格。
 * @return 自动上传后远程文件大小更新且不产生普通传输任务时返回 true。
 */
bool checkExternalEditWorkflow(MainWindow &window,
                               const QString &remotePath,
                               QTableWidget *remoteTable)
{
    if (remoteTable == nullptr)
    {
        QTextStream(stderr) << "External edit smoke UI objects are missing" << Qt::endl;
        return false;
    }

    const int readmeRow = findTableRowByName(remoteTable, "readme.txt");
    if (remoteTable->columnCount() != 6 || readmeRow < 0 || remoteTable->item(readmeRow, 1) == nullptr)
    {
        QTextStream(stderr) << "External edit smoke source file or table layout is unexpected" << Qt::endl;
        return false;
    }

    bool editMenuChecked = false;
    const QPoint contextMenuPosition = remoteTable->visualItemRect(remoteTable->item(readmeRow, 0)).center();
    QTimer::singleShot(50, remoteTable, [&editMenuChecked]() {
        auto *contextMenu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (contextMenu == nullptr)
        {
            return;
        }

        QStringList actionTexts;
        for (QAction *action : contextMenu->actions())
        {
            if (action != nullptr)
            {
                actionTexts.append(action->text());
            }
        }
        editMenuChecked = actionTexts.contains("编辑")
            && !actionTexts.contains("重新同步")
            && !actionTexts.contains("保存本地副本")
            && !actionTexts.contains("结束编辑");
        contextMenu->close();
    });
    QMetaObject::invokeMethod(
        remoteTable,
        "customContextMenuRequested",
        Qt::DirectConnection,
        Q_ARG(QPoint, contextMenuPosition));
    if (!editMenuChecked)
    {
        QTextStream(stderr) << "External edit context menu still contains obsolete actions" << Qt::endl;
        return false;
    }

    const QString originalSize = remoteTable->item(readmeRow, 1)->text();
    QString openedCachePath;
    window.setExternalEditorLauncherForTesting([&openedCachePath](const QString &filePath) {
        openedCachePath = filePath;
        return QFileInfo::exists(filePath);
    });

    window.editRemoteFileForTesting(joinRemotePathForCheck(remotePath, "readme.txt"));
    const auto openDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (openedCachePath.isEmpty() && std::chrono::steady_clock::now() < openDeadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (openedCachePath.isEmpty())
    {
        QTextStream(stderr) << "External edit smoke did not launch the cached file" << Qt::endl;
        return false;
    }

    const QByteArray editedContent("external edit smoke updated content\n");
    QSaveFile editedFile(openedCachePath);
    if (!editedFile.open(QIODevice::WriteOnly)
        || editedFile.write(editedContent) != editedContent.size()
        || !editedFile.commit())
    {
        QTextStream(stderr) << "External edit smoke could not save the cached file" << Qt::endl;
        return false;
    }

    const auto uploadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool uploaded = false;
    while (std::chrono::steady_clock::now() < uploadDeadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        const int refreshedRow = findTableRowByName(remoteTable, "readme.txt");
        if (refreshedRow >= 0
            && remoteTable->item(refreshedRow, 1) != nullptr
            && remoteTable->item(refreshedRow, 1)->text() != originalSize)
        {
            uploaded = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (!uploaded)
    {
        QTextStream(stderr) << "External edit smoke did not synchronize the changed file" << Qt::endl;
        return false;
    }

    QTreeWidget *transferTable = window.findChild<QTreeWidget *>("transferTable");
    if (transferTable == nullptr || transferTable->topLevelItemCount() != 0)
    {
        QTextStream(stderr) << "External edit smoke should not create ordinary transfer queue rows" << Qt::endl;
        return false;
    }
    return true;
}

/**
 * @brief 驱动主窗口的快速连接流程，并验证导航状态是否正确。
 * @param window 待测试的主窗口。
 * @param protocol 在快速连接协议下拉框中选择的协议文本。
 * @param host 远程主机地址。
 * @param port 远程端口。
 * @param user 远程用户名。
 * @param password 远程密码。
 * @param remotePath 初始要加载的远程路径。
 * @param expectedNames 远程表格中期望出现的条目列表。
 * @param useFakeBackend 连接前是否注入内存假后端。
 * @return 连接、导航和断开状态检查全部通过时返回 true。
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

    if (useFakeBackend)
    {
        ok = checkExternalEditWorkflow(window, remotePath, remoteTable) && ok;
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
 * @brief 通过快速连接控件建立一个假远程会话。
 * @param window 待测试的主窗口。
 * @param remotePath 新会话初始要加载的路径。
 * @return 当前远程标签页到达目标路径时返回 true。
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

bool checkQuickSaveCreatesSeparateSite(MainWindow &window)
{
    QTreeWidget *sessionTree = window.findChild<QTreeWidget *>("sessionManagerTree");
    QComboBox *protocolCombo = window.findChild<QComboBox *>("quickProtocolCombo");
    QLineEdit *hostEdit = window.findChild<QLineEdit *>("quickHostEdit");
    QLineEdit *portEdit = window.findChild<QLineEdit *>("quickPortEdit");
    QLineEdit *userEdit = window.findChild<QLineEdit *>("quickUserEdit");
    QLineEdit *passwordEdit = window.findChild<QLineEdit *>("quickPasswordEdit");
    QLineEdit *quickRemotePathEdit = window.findChild<QLineEdit *>("quickRemotePathEdit");
    QPushButton *saveButton = window.findChild<QPushButton *>("quickSaveSiteButton");
    if (sessionTree == nullptr || protocolCombo == nullptr || hostEdit == nullptr || portEdit == nullptr
        || userEdit == nullptr || passwordEdit == nullptr || quickRemotePathEdit == nullptr || saveButton == nullptr)
    {
        QTextStream(stderr) << "Quick-save new-site prerequisites are incomplete" << Qt::endl;
        return false;
    }

    SiteProfile existing;
    existing.id = "ui-preserve-existing-site";
    existing.name = "SFTP_192.168.8.128";
    existing.group = "虚拟机测试";
    existing.protocol = RemoteProtocol::Sftp;
    existing.host = "192.168.8.128";
    existing.port = 22;
    existing.username = "testuser";
    existing.password = "old-password";
    existing.defaultRemotePath = "/home/testuser/remote_test";
    existing.encoding = "UTF-8";
    window.saveSiteForTesting(existing);
    QApplication::processEvents();

    window.setRemoteFileSystemForTesting(std::make_unique<FakeRemoteFileSystem>());
    protocolCombo->setCurrentText("SFTP");
    hostEdit->setText("192.168.8.128");
    portEdit->setText("22");
    userEdit->setText("testuser");
    passwordEdit->setText("new-password");
    quickRemotePathEdit->setText("/home/testuser/remote_test");
    bool dialogChecked = false;
    QTimer::singleShot(0, [&]() {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (dialog == nullptr || dialog->objectName() != "siteProfileDialog")
        {
            return;
        }
        auto *nameEdit = dialog->findChild<QLineEdit *>("siteNameEdit");
        auto *groupEdit = dialog->findChild<QLineEdit *>("siteGroupEdit");
        auto *buttons = dialog->findChild<QDialogButtonBox *>();
        if (nameEdit == nullptr || groupEdit == nullptr || buttons == nullptr
            || buttons->button(QDialogButtonBox::Ok)->text() != "确定"
            || buttons->button(QDialogButtonBox::Cancel)->text() != "取消")
        {
            return;
        }
        nameEdit->setText("SFTP_新增同IP");
        groupEdit->setText("未分组");
        dialogChecked = true;
        buttons->button(QDialogButtonBox::Ok)->click();
    });
    saveButton->click();
    QApplication::processEvents();

    if (!dialogChecked)
    {
        QTextStream(stderr) << "Quick-save did not show a custom site dialog with Chinese buttons" << Qt::endl;
        return false;
    }
    if (!treeContainsText(sessionTree, "虚拟机测试") || !treeContainsText(sessionTree, "SFTP_192.168.8.128"))
    {
        QTextStream(stderr) << "Quick-save changed the existing site name or group" << Qt::endl;
        return false;
    }
    if (!treeContainsText(sessionTree, "SFTP_新增同IP"))
    {
        QTextStream(stderr) << "Quick-save did not create a separate same-host site" << Qt::endl;
        return false;
    }

    std::string createdSiteId;
    QTreeWidgetItemIterator iterator(sessionTree);
    while (*iterator != nullptr)
    {
        if ((*iterator)->text(0) == "SFTP_新增同IP")
        {
            createdSiteId = (*iterator)->data(0, Qt::UserRole + 2).toString().toStdString();
            break;
        }
        ++iterator;
    }
    if (!createdSiteId.empty())
    {
        window.removeSiteForTesting(createdSiteId);
    }
    window.removeSiteForTesting(existing.id);
    QApplication::processEvents();
    return true;
}

/**
 * @brief 验证连接中状态、重复连接拦截和逻辑取消是否正常。
 * @param window 待测试的主窗口。
 * @return 界面能正确暴露并清理进行中的连接状态时返回 true。
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
    if (protocolCombo == nullptr || hostEdit == nullptr || portEdit == nullptr || userEdit == nullptr
        || passwordEdit == nullptr || quickRemotePathEdit == nullptr || connectButton == nullptr
        || remoteTabs == nullptr)
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

    connectButton->click();
    if (!waitForRemoteDisconnected(connectingPanel) || connectButton->text() != "连接")
    {
        QTextStream(stderr) << "Remote connection cancellation did not reset UI state" << Qt::endl;
        return false;
    }

    return true;
}

/**
 * @brief 验证窗口关闭会等待使用远程后端的准备任务安全结束。
 * @return 关闭请求不会提前销毁仍有后台任务的主窗口时返回 true。
 */
bool checkWindowCloseDuringUploadPreparation()
{
    auto *window = new MainWindow(DependencyCheckResult{});
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->setDialogsSuppressedForTesting(true);
    window->setRemoteFileSystemForTesting(std::make_unique<SlowFakeRemoteFileSystem>());

    QComboBox *protocolCombo = window->findChild<QComboBox *>("quickProtocolCombo");
    QLineEdit *hostEdit = window->findChild<QLineEdit *>("quickHostEdit");
    QLineEdit *portEdit = window->findChild<QLineEdit *>("quickPortEdit");
    QLineEdit *userEdit = window->findChild<QLineEdit *>("quickUserEdit");
    QLineEdit *remotePathEdit = window->findChild<QLineEdit *>("quickRemotePathEdit");
    QPushButton *connectButton = window->findChild<QPushButton *>("quickConnectButton");
    QTabWidget *remoteTabs = window->findChild<QTabWidget *>("remoteTabs");
    if (protocolCombo == nullptr || hostEdit == nullptr || portEdit == nullptr || userEdit == nullptr
        || remotePathEdit == nullptr || connectButton == nullptr || remoteTabs == nullptr)
    {
        delete window;
        QTextStream(stderr) << "Close-lifecycle smoke prerequisites are incomplete" << Qt::endl;
        return false;
    }

    protocolCombo->setCurrentText("SFTP");
    hostEdit->setText("close-lifecycle-host");
    portEdit->setText("22");
    userEdit->setText("testuser");
    remotePathEdit->setText("/home/testuser/remote_test");
    connectButton->click();
    if (!waitForRemoteConnected(remoteTabs->currentWidget(), "/home/testuser/remote_test"))
    {
        delete window;
        QTextStream(stderr) << "Close-lifecycle smoke could not connect fake remote session" << Qt::endl;
        return false;
    }

    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "dirbridge-close-lifecycle-smoke";
    const std::filesystem::path localFile = tempRoot / "upload.txt";
    std::filesystem::create_directories(tempRoot);
    {
        std::ofstream output(localFile, std::ios::binary | std::ios::trunc);
        output << "close lifecycle smoke";
    }

    window->uploadLocalFileForTesting(QString::fromStdString(localFile.string()));
    QApplication::processEvents();

    QPointer<MainWindow> guardedWindow(window);
    window->close();
    QApplication::processEvents();
    if (guardedWindow == nullptr)
    {
        std::filesystem::remove_all(tempRoot);
        QTextStream(stderr) << "Window closed before upload preparation completed" << Qt::endl;
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (guardedWindow != nullptr && std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::filesystem::remove_all(tempRoot);
    if (guardedWindow != nullptr)
    {
        delete guardedWindow.data();
        QTextStream(stderr) << "Window did not finish close-lifecycle coordination" << Qt::endl;
        return false;
    }

    return true;
}

/**
 * @brief 验证 v0.5.0 会话管理器的分组和最近会话行为。
 * @param window 待测试的主窗口。
 * @return 已保存站点、分组和最近会话能在假远程标签页下正常工作时返回 true。
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
    QStringList siteContextActions;
    QTimer::singleShot(0, [&siteContextActions]() {
        auto *contextMenu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (contextMenu == nullptr)
        {
            return;
        }
        for (QAction *action : contextMenu->actions())
        {
            siteContextActions.push_back(action->text());
        }
        contextMenu->close();
    });
    QMetaObject::invokeMethod(
        sessionTree,
        "customContextMenuRequested",
        Qt::DirectConnection,
        Q_ARG(QPoint, sessionTree->visualItemRect(groupedItem).center()));
    if (!siteContextActions.contains("打开 SSH 终端"))
    {
        QTextStream(stderr) << "Saved SFTP site context menu is missing SSH terminal action" << Qt::endl;
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

    QStringList remoteTabContextActions;
    QTimer::singleShot(0, [&remoteTabContextActions]() {
        auto *contextMenu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (contextMenu == nullptr)
        {
            return;
        }
        for (QAction *action : contextMenu->actions())
        {
            remoteTabContextActions.push_back(action->text());
        }
        contextMenu->close();
    });
    QTabBar *remoteTabBar = remoteTabs->tabBar();
    QMetaObject::invokeMethod(
        remoteTabBar,
        "customContextMenuRequested",
        Qt::DirectConnection,
        Q_ARG(QPoint, remoteTabBar->tabRect(remoteTabs->currentIndex()).center()));
    if (!remoteTabContextActions.contains("打开 SSH 终端"))
    {
        QTextStream(stderr) << "SFTP remote tab context menu is missing SSH terminal action" << Qt::endl;
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
    if (initialTabCount == 0 && !remoteTabs->isHidden())
    {
        QTextStream(stderr) << "Remote tabs should be hidden after closing the last session" << Qt::endl;
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
 * @brief 验证多个远程标签页能否保持彼此独立的状态。
 * @param window 待测试的主窗口。
 * @return 两个额外的假远程会话能够独立共存时返回 true。
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
 * @brief 验证假后端下的目录上传/下载、远程移动和递归远程删除。
 * @param window 待测试的主窗口。
 * @return 目录操作能够通过正常的 MainWindow 工作流完成时返回 true。
 */
bool checkRemoteDirectoryOperationWorkflow(MainWindow &window)
{
    const QString remotePath = "/home/testuser/remote_test";
    if (!connectFakeRemoteSession(window, remotePath))
    {
        return false;
    }
    QTreeWidget *transferTable = window.findChild<QTreeWidget *>("transferTable");
    if (transferTable == nullptr)
    {
        QTextStream(stderr) << "Directory operation transfer table is missing" << Qt::endl;
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
    if (!waitForTransferRow(transferTable, "dirbridge-folder", "上传", "已完成"))
    {
        QTextStream(stderr) << "Directory upload did not complete before conflict retry" << Qt::endl;
        dumpTransferRows(transferTable);
        return false;
    }
    const QString uploadedDirectory = "/home/testuser/remote_test/dirbridge-folder";
    const QString movedDirectory = "/home/testuser/remote_test/upload/dirbridge-folder";
    window.moveRemotePathsForTesting({uploadedDirectory}, "/home/testuser/remote_test/upload");
    QApplication::processEvents();

    window.downloadRemotePathForTesting(movedDirectory);
    if (!waitForTransferRow(transferTable, "dirbridge-folder", "下载", "已完成"))
    {
        QTextStream(stderr) << "Directory download did not complete" << Qt::endl;
        dumpTransferRows(transferTable);
        return false;
    }
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
    if (uploadParent == nullptr || uploadParent->text(2) != "100%" || uploadParent->childCount() < 2
        || uploadParent->isExpanded() || !hasProgressBar(transferTable, uploadParent)
        || uploadParent->text(9).isEmpty())
    {
        QTextStream(stderr) << "Directory upload parent transfer row is incomplete" << Qt::endl;
        return false;
    }
    if (downloadParent == nullptr || downloadParent->text(2) != "100%" || downloadParent->childCount() < 2
        || downloadParent->isExpanded() || !hasProgressBar(transferTable, downloadParent)
        || downloadParent->text(9).isEmpty())
    {
        QTextStream(stderr) << "Directory download parent transfer row is incomplete" << Qt::endl;
        return false;
    }
    if (!hasProgressBar(transferTable, uploadParent->child(0)) || !hasProgressBar(transferTable, downloadParent->child(0)))
    {
        QTextStream(stderr) << "Directory child transfer row does not use a progress bar" << Qt::endl;
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
    if (!waitForTableRowAbsent(remoteTable, "dirbridge-folder"))
    {
        QTextStream(stderr) << "Recursive remote delete did not remove moved directory" << Qt::endl;
        return false;
    }

    return true;
}

/**
 * @brief 针对内存假后端运行远程 UI 工作流冒烟测试。
 * @param window 待测试的主窗口。
 * @return 假后端工作流通过时返回 true。
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
        && checkQuickSaveCreatesSeparateSite(window)
        && checkRemoteConnectionControlWorkflow(window)
        && checkSessionManagerWorkflow(window)
        && checkRemoteMultiSessionWorkflow(window)
        && checkRemoteDirectoryOperationWorkflow(window)
        && checkWindowCloseDuringUploadPreparation();
}

/**
 * @brief 针对真实 FTP/SFTP 服务器运行远程 UI 工作流冒烟测试。
 * @param window 待测试的主窗口。
 * @return 真实连接与导航工作流通过时返回 true。
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
 * @brief 使用站点管理器中已保存的凭据验证真实远程连接。
 * @param window 待测试的主窗口。
 * @param siteName 已保存站点的显示名称。
 * @return 成功加载远程目录时返回 true。
 */
bool checkSavedSiteRemoteUiWorkflow(MainWindow &window, const QString &siteName)
{
    QTreeWidget *sessionTree = window.findChild<QTreeWidget *>("sessionManagerTree");
    QTabWidget *remoteTabs = window.findChild<QTabWidget *>("remoteTabs");
    if (sessionTree == nullptr || remoteTabs == nullptr)
    {
        QTextStream(stderr) << "Saved-site smoke UI objects are missing" << Qt::endl;
        return false;
    }

    QTreeWidgetItem *siteItem = nullptr;
    QTreeWidgetItemIterator iterator(sessionTree);
    while (*iterator != nullptr)
    {
        if ((*iterator)->text(0) == siteName)
        {
            siteItem = *iterator;
            break;
        }
        ++iterator;
    }
    if (siteItem == nullptr)
    {
        QTextStream(stderr) << "Saved site is not present in session manager: " << siteName << Qt::endl;
        return false;
    }

    QMetaObject::invokeMethod(
        sessionTree,
        "itemDoubleClicked",
        Qt::DirectConnection,
        Q_ARG(QTreeWidgetItem *, siteItem),
        Q_ARG(int, 0));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        QWidget *remotePanel = remoteTabs->currentWidget();
        QLineEdit *remotePathEdit = remotePanel == nullptr ? nullptr : remotePanel->findChild<QLineEdit *>("remotePathEdit");
        QLabel *remoteStateLabel = remotePanel == nullptr ? nullptr : remotePanel->findChild<QLabel *>("remoteStateLabel");
        if (remotePathEdit != nullptr && !remotePathEdit->text().isEmpty()
            && remoteStateLabel != nullptr && remoteStateLabel->text().contains("个项目"))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    QTextStream(stderr) << "Saved site did not connect and load its remote directory: " << siteName << Qt::endl;
    return false;
}

/**
 * @brief 通过快速连接 UI 将主窗口连接到真实远程服务器。
 * @param window 待测试的主窗口。
 * @param protocol 在快速连接协议下拉框中选择的协议文本。
 * @param host 远程主机地址。
 * @param port 远程端口。
 * @param user 远程用户名。
 * @param password 远程密码。
 * @param remotePath 初始要加载的远程路径。
 * @return 远程面板进入已连接状态时返回 true。
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
 * @brief 根据环境变量构建真实测试用站点配置。
 * @return 当前真实 FTP/SFTP 冒烟测试使用的站点配置。
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
 * @brief 通过在 UI 外部修改服务器状态来运行真实刷新冒烟测试。
 * @param window 待测试的主窗口。
 * @return 刷新能够显示外部创建和删除变化时返回 true。
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
 * @brief 通过主窗口传输 UI 运行真实上传/下载冒烟测试。
 * @param window 待测试的主窗口。
 * @return 上传、下载、传输表更新和清理全部通过时返回 true。
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
    if (!waitForTransferRow(transferTable, fileName, "上传", "已完成"))
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
    if (!waitForTransferRow(transferTable, fileName, "下载", "已完成"))
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
