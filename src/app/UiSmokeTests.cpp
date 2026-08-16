#include "app/UiSmokeTests.h"

#include "config/SiteProfile.h"
#include "config/SiteStore.h"
#include "core/DependencyCheck.h"
#include "core/FakeRemoteFileSystem.h"
#include "protocol/CurlRemoteFileSystem.h"
#include "ui/FileChangeMonitor.h"
#include "ui/FilePanel.h"
#include "ui/MainWindow.h"
#include "ui/panel_shared.h"

#ifdef _WIN32
#include "platform/windows/WindowsShellDataObject.h"

#include <ole2.h>
#include <shlobj.h>
#include <shldisp.h>
#include <windows.h>
#endif

#include <QAction>
#include <QAbstractItemDelegate>
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDockWidget>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMetaObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QSplitter>
#include <QStatusBar>
#include <QStorageInfo>
#include <QToolButton>
#include <QStringList>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTabWidget>
#include <QTabBar>
#include <QTextStream>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

QAction *findActionByText(MainWindow &window, const QString &text);
bool checkAboutDialog(MainWindow &window);

namespace window_shared
{
QString userFacingRemoteError(const QString &detail);
}

bool containsRemotePath(const std::vector<FileItem> &items, const std::string &path, FileItemType type)
{
    for (const FileItem &item : items)
    {
        if (item.path == path && item.type == type)
        {
            return true;
        }
    }
    return false;
}

bool waitForRemotePath(
    FakeRemoteFileSystem *fileSystem,
    const QString &directory,
    const QString &path,
    FileItemType type)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (fileSystem != nullptr && std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        try
        {
            if (containsRemotePath(fileSystem->listDirectory(directory.toStdString()), path.toStdString(), type))
            {
                return true;
            }
        }
        catch (const std::exception &)
        {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool waitForRemoteFileSize(
    FakeRemoteFileSystem *fileSystem,
    const QString &directory,
    const QString &path,
    std::int64_t expectedSize)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (fileSystem != nullptr && std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        try
        {
            const std::vector<FileItem> items = fileSystem->listDirectory(directory.toStdString());
            const auto found = std::find_if(items.begin(), items.end(), [&path](const FileItem &item) {
                return item.path == path.toStdString() && item.type == FileItemType::File;
            });
            if (found != items.end() && found->size == expectedSize)
            {
                return true;
            }
        }
        catch (const std::exception &)
        {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool waitForRemoteDirectoryReplacement(
    FakeRemoteFileSystem *fileSystem,
    const QString &parentDirectory,
    const QString &targetDirectory,
    const QString &requiredChild,
    const QString &removedChild)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (fileSystem != nullptr && std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        try
        {
            const std::vector<FileItem> parentItems = fileSystem->listDirectory(parentDirectory.toStdString());
            const bool hasArtifacts = std::any_of(parentItems.begin(), parentItems.end(), [](const FileItem &item) {
                return item.name.rfind(".dirbridge-", 0) == 0;
            });
            const std::vector<FileItem> targetItems = fileSystem->listDirectory(targetDirectory.toStdString());
            if (!hasArtifacts
                && containsRemotePath(targetItems, requiredChild.toStdString(), FileItemType::Directory)
                && !containsRemotePath(targetItems, removedChild.toStdString(), FileItemType::File))
            {
                return true;
            }
        }
        catch (const std::exception &)
        {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

bool waitForLocalDirectoryReplacement(
    const std::filesystem::path &parentDirectory,
    const std::filesystem::path &targetDirectory,
    const std::filesystem::path &requiredChild,
    const std::filesystem::path &removedChild)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        bool hasArtifacts = false;
        std::error_code error;
        for (std::filesystem::directory_iterator iterator(parentDirectory, error), end;
             !error && iterator != end;
             iterator.increment(error))
        {
            if (iterator->path().filename().string().rfind(".dirbridge-", 0) == 0)
            {
                hasArtifacts = true;
                break;
            }
        }
        if (!error
            && !hasArtifacts
            && std::filesystem::is_directory(targetDirectory)
            && std::filesystem::is_regular_file(requiredChild)
            && !std::filesystem::exists(removedChild))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

/**
 * @brief 测量一次性展示大量远程文件时的 UI 主线程耗时。
 * @return 填充 2500 个远程文件所需的毫秒数。
 */
qint64 measureLargeRemoteDirectoryPopulation()
{
    FilePanel panel(FilePanel::Mode::RemotePlaceholder);
    panel.resize(900, 600);
    panel.show();
    QApplication::processEvents();

    std::vector<FileItem> items;
    items.reserve(2500);
    for (int index = 0; index < 2500; ++index)
    {
        const std::string name = "remote-file-" + std::to_string(index) + ".txt";
        items.push_back({name,
                         "/performance/" + name,
                         FileItemType::File,
                         1024 + index,
                         "2026-08-03 12:00:00",
                         "-rw-r--r--",
                         "tester"});
    }

    QElapsedTimer timer;
    timer.start();
    panel.setRemoteItems("/performance", items, QString(), false);
    return timer.elapsed();
}

/**
 * @brief 验证本地文件项目的内联重命名和常见远程错误提示分类。
 * @return 工作流与提示分类均符合预期时返回 true。
 */
bool checkFileCreateRenameWorkflow()
{
    if (window_shared::userFacingRemoteError("target already exists") != "目标项目已存在，请使用其他名称。"
        || window_shared::userFacingRemoteError("Permission denied") != "权限不足，请检查账号权限或目标目录权限。"
        || window_shared::userFacingRemoteError("directory is not empty") != "目录非空，请先清理目录内容。"
        || window_shared::userFacingRemoteError("invalid path") != "名称或路径无效，请检查输入内容。"
        || window_shared::userFacingRemoteError("unknown command") != "服务器不支持此操作。"
        || window_shared::userFacingRemoteError("Login denied") != "认证失败，请检查用户名、密码或认证配置。")
    {
        QTextStream(stderr) << "Remote operation error categories are incomplete" << Qt::endl;
        return false;
    }

    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid())
    {
        QTextStream(stderr) << "Unable to create temporary directory for file create/rename smoke test" << Qt::endl;
        return false;
    }

    const QString originalPath = QDir(temporaryDirectory.path()).filePath("original.txt");
    QFile originalFile(originalPath);
    if (!originalFile.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        QTextStream(stderr) << "Unable to create file for inline rename smoke test" << Qt::endl;
        return false;
    }
    originalFile.close();

    FilePanel panel(FilePanel::Mode::Local);
    panel.resize(900, 600);
    panel.show();
    panel.setLocalPathForTesting(temporaryDirectory.path());
    QApplication::processEvents();

    auto *table = panel.findChild<QTableWidget *>("localFileTable");
    auto *shortcut = panel.findChild<QShortcut *>("localRenameShortcut");
    if (table == nullptr || shortcut == nullptr)
    {
        QTextStream(stderr) << "Local file create/rename smoke UI objects are missing" << Qt::endl;
        return false;
    }
    const QStringList expectedFileHeaders{"名称", "大小", "类型", "修改时间", "权限"};
    if (table->columnCount() != expectedFileHeaders.size())
    {
        QTextStream(stderr) << "Local file table should contain five columns" << Qt::endl;
        return false;
    }
    for (int column = 0; column < expectedFileHeaders.size(); ++column)
    {
        if (table->horizontalHeaderItem(column) == nullptr
            || table->horizontalHeaderItem(column)->text() != expectedFileHeaders.at(column))
        {
            QTextStream(stderr) << "Local file table headers are unexpected" << Qt::endl;
            return false;
        }
    }

    int originalRow = -1;
    for (int row = 0; row < table->rowCount(); ++row)
    {
        QTableWidgetItem *nameItem = table->item(row, 0);
        if (nameItem != nullptr && nameItem->data(Qt::UserRole).toString() == originalPath)
        {
            originalRow = row;
            break;
        }
    }
    if (originalRow < 0)
    {
        QTextStream(stderr) << "Inline rename smoke source row is missing" << Qt::endl;
        return false;
    }

    table->setCurrentCell(
        originalRow,
        0,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection);
    QApplication::processEvents();
    auto *editor = panel.findChild<QLineEdit *>("inlineRenameEditor");
    if (editor == nullptr)
    {
        QTextStream(stderr) << "Inline rename editor was not created" << Qt::endl;
        return false;
    }
    const QString renamedPath = QDir(temporaryDirectory.path()).filePath("renamed.txt");
    editor->setText("renamed.txt");
    auto *delegate = table->itemDelegate();
    delegate->commitData(editor);
    delegate->closeEditor(editor, QAbstractItemDelegate::NoHint);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    if (!QFileInfo::exists(renamedPath) || QFileInfo::exists(originalPath))
    {
        QTextStream(stderr) << "Inline rename did not update the local filesystem" << Qt::endl;
        return false;
    }

    int renamedRow = -1;
    for (int row = 0; row < table->rowCount(); ++row)
    {
        QTableWidgetItem *nameItem = table->item(row, 0);
        if (nameItem != nullptr && nameItem->data(Qt::UserRole).toString() == renamedPath)
        {
            renamedRow = row;
            break;
        }
    }
    if (renamedRow < 0)
    {
        QTextStream(stderr) << "Renamed local row is missing after refresh" << Qt::endl;
        return false;
    }

    table->setCurrentCell(
        renamedRow,
        0,
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QMetaObject::invokeMethod(shortcut, "activated", Qt::DirectConnection);
    QApplication::processEvents();
    editor = panel.findChild<QLineEdit *>("inlineRenameEditor");
    if (editor == nullptr)
    {
        QTextStream(stderr) << "Inline rename editor was not recreated" << Qt::endl;
        return false;
    }
    editor->setText("cancelled.txt");
    delegate->closeEditor(editor, QAbstractItemDelegate::RevertModelCache);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    if (!QFileInfo::exists(renamedPath) || QFileInfo::exists(QDir(temporaryDirectory.path()).filePath("cancelled.txt")))
    {
        QTextStream(stderr) << "Esc did not cancel inline rename" << Qt::endl;
        return false;
    }

    return true;
}

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
    const qint64 largeDirectoryElapsed = measureLargeRemoteDirectoryPopulation();
    if (largeDirectoryElapsed > 500)
    {
        QTextStream(stderr) << "Large remote directory UI population took "
                            << largeDirectoryElapsed << " ms" << Qt::endl;
        ok = false;
    }
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
    ok = requireChild<QTableWidget>(window, "localFileTable") && ok;
    ok = requireChild<QTabWidget>(window, "remoteTabs") && ok;
    ok = requireChild<QTabWidget>(window, "bottomTabs") && ok;
    ok = requireChild<QTabWidget>(window, "terminalTabs") && ok;
    QTabWidget *terminalTabs = window.findChild<QTabWidget *>("terminalTabs");
    if (terminalTabs != nullptr && terminalTabs->count() != 0)
    {
        QTextStream(stderr) << "Terminal tabs should start empty" << Qt::endl;
        ok = false;
    }
    QTabWidget *bottomTabs = window.findChild<QTabWidget *>("bottomTabs");
    QWidget *terminalHost = window.findChild<QWidget *>("terminalHost");
    QSplitter *workspaceSplitter =
        window.findChild<QSplitter *>("workspaceSplitter");
    QSplitter *fileSplitter = window.findChild<QSplitter *>("fileSplitter");
    QDockWidget *sessionDock = window.findChild<QDockWidget *>("SessionManagerDock");
    QToolButton *terminalMaximizeButton =
        window.findChild<QToolButton *>("terminalMaximizeButton");
    if (bottomTabs == nullptr || terminalHost == nullptr
        || workspaceSplitter == nullptr
        || fileSplitter == nullptr || sessionDock == nullptr
        || terminalMaximizeButton == nullptr)
    {
        QTextStream(stderr) << "Terminal workspace controls are incomplete" << Qt::endl;
        ok = false;
    }
    else
    {
        const QList<int> originalSizes = workspaceSplitter->sizes();
        bottomTabs->setCurrentWidget(terminalHost);
        QApplication::processEvents();
        terminalMaximizeButton->click();
        QApplication::processEvents();
        if (!fileSplitter->isHidden() || sessionDock->isHidden())
        {
            QTextStream(stderr) << "Terminal maximize should only hide the file workspace" << Qt::endl;
            ok = false;
        }
        terminalMaximizeButton->click();
        QApplication::processEvents();
        if (fileSplitter->isHidden()
            || workspaceSplitter->sizes() != originalSizes)
        {
            QTextStream(stderr) << "Terminal restore should show the file workspace and restore its size" << Qt::endl;
            ok = false;
        }
        terminalMaximizeButton->click();
        QApplication::processEvents();
        bottomTabs->setCurrentIndex(0);
        QApplication::processEvents();
        if (fileSplitter->isHidden() || !terminalMaximizeButton->isHidden())
        {
            QTextStream(stderr) << "Leaving the terminal page should restore the file workspace" << Qt::endl;
            ok = false;
        }
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
    QTreeWidget *logView = window.findChild<QTreeWidget *>("logView");
    if (logView == nullptr || logView->topLevelItemCount() == 0)
    {
        QTextStream(stderr) << "Log view should contain startup entries" << Qt::endl;
        ok = false;
    }
    else
    {
        const int previousBottomTab = bottomTabs == nullptr ? -1 : bottomTabs->currentIndex();
        if (bottomTabs != nullptr)
        {
            bottomTabs->setCurrentWidget(logView);
            QApplication::processEvents();
        }

        QTreeWidgetItem *logItem = logView->topLevelItem(0);
        logView->setCurrentItem(logItem);
        const QString expectedLogText = QString("%1\t%2\t%3")
                                            .arg(logItem->text(0), logItem->text(1), logItem->text(2));
        QApplication::clipboard()->clear();
        bool copyActionFound = false;
        QTimer::singleShot(0, logView, [&copyActionFound]() {
            auto *contextMenu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (contextMenu == nullptr)
            {
                return;
            }
            for (QAction *action : contextMenu->actions())
            {
                if (action != nullptr && action->text() == "复制")
                {
                    copyActionFound = true;
                    action->trigger();
                    break;
                }
            }
            contextMenu->close();
        });
        QMetaObject::invokeMethod(
            logView,
            "customContextMenuRequested",
            Qt::DirectConnection,
            Q_ARG(QPoint, logView->visualItemRect(logItem).center()));
        if (!copyActionFound || QApplication::clipboard()->text() != expectedLogText)
        {
            QTextStream(stderr) << "Log context menu did not copy the selected entry" << Qt::endl;
            ok = false;
        }
        if (bottomTabs != nullptr && previousBottomTab >= 0)
        {
            bottomTabs->setCurrentIndex(previousBottomTab);
        }
    }
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

    QTableWidget *localFileTable = window.findChild<QTableWidget *>("localFileTable");
    if (localFileTable != nullptr)
    {
        QHeaderView *header = localFileTable->horizontalHeader();
        const int initialNameWidth = localFileTable->columnWidth(0);
        if (header == nullptr
            || header->sectionResizeMode(0) != QHeaderView::Interactive
            || initialNameWidth != 175)
        {
            QTextStream(stderr) << "File name column should use a 175px interactive default width" << Qt::endl;
            ok = false;
        }
        else
        {
            header->resizeSection(0, 280);
            if (localFileTable->columnWidth(0) != 280)
            {
                QTextStream(stderr) << "File name column should allow manual resizing" << Qt::endl;
                ok = false;
            }
            header->resizeSection(0, initialNameWidth);
        }
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
        const QList<int> expectedTransferWidths{200, 60, 140, 140, 140, 40, 140, 80, 80, 80};
        for (int column = 0; column < expectedTransferWidths.size(); ++column)
        {
            if (transferTable->header()->sectionResizeMode(column) != QHeaderView::Interactive
                || transferTable->columnWidth(column) != expectedTransferWidths.at(column))
            {
                QTextStream(stderr) << "Transfer table default width mismatch at column " << column << Qt::endl;
                ok = false;
            }
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

bool waitForRemoteLoadFailure(QWidget *remotePanel, const QString &restoredPath)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        QLineEdit *remotePathEdit = remotePanel == nullptr ? nullptr : remotePanel->findChild<QLineEdit *>("remotePathEdit");
        QLabel *remoteStateLabel = remotePanel == nullptr ? nullptr : remotePanel->findChild<QLabel *>("remoteStateLabel");
        if (remotePathEdit != nullptr && remoteStateLabel != nullptr
            && remotePathEdit->text() == restoredPath
            && remoteStateLabel->text().contains("无法加载远程目录"))
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
        ++m_listDirectoryCallCount;
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return FakeRemoteFileSystem::listDirectory(path);
    }

    int listDirectoryCallCount() const
    {
        return m_listDirectoryCallCount.load();
    }

private:
    std::atomic<int> m_listDirectoryCallCount{0};
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

bool waitForTablePermission(QTableWidget *table, const QString &name, const QString &permissions)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        QApplication::processEvents();
        const int row = findTableRowByName(table, name);
        if (row >= 0 && table->item(row, 4) != nullptr && table->item(row, 4)->text() == permissions)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

QString joinRemotePathForCheck(QString directory, const QString &name);

QTreeWidgetItem *findTreeItemByPath(QTreeWidgetItem *item, const QString &path)
{
    if (item == nullptr)
    {
        return nullptr;
    }
    if (item->data(0, Qt::UserRole).toString().compare(path, Qt::CaseInsensitive) == 0)
    {
        return item;
    }
    for (int index = 0; index < item->childCount(); ++index)
    {
        if (QTreeWidgetItem *found = findTreeItemByPath(item->child(index), path); found != nullptr)
        {
            return found;
        }
    }
    return nullptr;
}

/**
 * @brief 验证一次鼠标按压只会启动一次拖拽，避免 OLE 嵌套事件循环重复进入。
 * @return 连续移动不重复启动、下一次按压仍可启动时返回 true。
 */
bool checkSingleDragStartPerMousePress()
{
    FilePanel panel(FilePanel::Mode::RemotePlaceholder);
    panel.resize(900, 600);
    panel.setRemoteSessionId("drag-gesture-session");
    panel.setRemoteItems(
        "/remote",
        {FileItem{"sample.txt", "/remote/sample.txt", FileItemType::File, 12, "", "", ""}},
        "",
        false);
    int dragStartCount = 0;
    panel.setRemoteShellDragRequestedHandler([&dragStartCount](const QList<RemoteTransferItem> &) {
        ++dragStartCount;
    });
    panel.show();
    QApplication::processEvents();

    auto *table = panel.findChild<QTableWidget *>("remoteFileTable");
    const int row = table == nullptr ? -1 : findTableRowByName(table, "sample.txt");
    if (table == nullptr || row < 0 || table->item(row, 0) == nullptr)
    {
        return false;
    }
    table->setCurrentCell(row, 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const QPoint startPosition = table->visualItemRect(table->item(row, 0)).center();
    const QPoint dragPosition = startPosition + QPoint(QApplication::startDragDistance() + 12, 0);

    auto sendPressAndMoves = [&]() {
        QMouseEvent pressEvent(
            QEvent::MouseButtonPress,
            QPointF(startPosition),
            QPointF(startPosition),
            QPointF(table->viewport()->mapToGlobal(startPosition)),
            Qt::LeftButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(table->viewport(), &pressEvent);
        QMouseEvent firstMove(
            QEvent::MouseMove,
            QPointF(dragPosition),
            QPointF(dragPosition),
            QPointF(table->viewport()->mapToGlobal(dragPosition)),
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(table->viewport(), &firstMove);
        QMouseEvent repeatedMove(
            QEvent::MouseMove,
            QPointF(dragPosition + QPoint(4, 0)),
            QPointF(dragPosition + QPoint(4, 0)),
            QPointF(table->viewport()->mapToGlobal(dragPosition + QPoint(4, 0))),
            Qt::NoButton,
            Qt::LeftButton,
            Qt::NoModifier);
        QApplication::sendEvent(table->viewport(), &repeatedMove);
    };

    sendPressAndMoves();
    if (dragStartCount != 1)
    {
        QTextStream(stderr) << "One mouse press started the remote drag more than once" << Qt::endl;
        return false;
    }

    QMouseEvent releaseEvent(
        QEvent::MouseButtonRelease,
        QPointF(dragPosition),
        QPointF(dragPosition),
        QPointF(table->viewport()->mapToGlobal(dragPosition)),
        Qt::LeftButton,
        Qt::NoButton,
        Qt::NoModifier);
    QApplication::sendEvent(table->viewport(), &releaseEvent);
    sendPressAndMoves();
    if (dragStartCount != 2)
    {
        QTextStream(stderr) << "A new mouse press could not start another remote drag" << Qt::endl;
        return false;
    }
    return true;
}

/**
 * @brief 验证本地目录刷新只增量更新变化项，避免下载完成后重建全部行。
 * @return 未变化项目的表格项和选择状态得到保留时返回 true。
 */
bool checkIncrementalLocalDirectoryRefresh()
{
    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid())
    {
        return false;
    }

    const QString stablePath = QDir(temporaryDirectory.path()).filePath("stable.txt");
    QFile stableFile(stablePath);
    if (!stableFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || stableFile.write("stable") != 6)
    {
        return false;
    }
    stableFile.close();

    FilePanel panel(FilePanel::Mode::Local);
    panel.setLocalPathForTesting(temporaryDirectory.path());
    auto *table = panel.findChild<QTableWidget *>("localFileTable");
    const int stableRow = table == nullptr ? -1 : findTableRowByName(table, "stable.txt");
    if (stableRow < 0 || table->item(stableRow, 0) == nullptr)
    {
        return false;
    }
    QTableWidgetItem *stableItem = table->item(stableRow, 0);
    table->setCurrentItem(stableItem, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);

    QFile addedFile(QDir(temporaryDirectory.path()).filePath("added.txt"));
    if (!addedFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || addedFile.write("added") != 5)
    {
        return false;
    }
    addedFile.close();
    panel.refresh();

    const int refreshedStableRow = findTableRowByName(table, "stable.txt");
    return refreshedStableRow >= 0
        && table->item(refreshedStableRow, 0) == stableItem
        && stableItem->isSelected()
        && findTableRowByName(table, "added.txt") >= 0;
}

/**
 * @brief 验证本地文件树仅接收拖放以及同盘本地项目移动。
 * @return 文件树行为和同盘移动结果符合预期时返回 true。
 */
bool checkFileTreeDropWorkflow()
{
    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid())
    {
        QTextStream(stderr) << "Unable to create temporary directory for file tree drop smoke test" << Qt::endl;
        return false;
    }

    const QString sourcePath = QDir(temporaryDirectory.path()).filePath("move-source.txt");
    const QString targetDirectory = QDir(temporaryDirectory.path()).filePath("target");
    if (!QDir().mkpath(targetDirectory))
    {
        return false;
    }
    QFile sourceFile(sourcePath);
    if (!sourceFile.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        return false;
    }
    sourceFile.write("tree drop");
    sourceFile.close();

    FilePanel localPanel(FilePanel::Mode::Local);
    localPanel.resize(900, 600);
    localPanel.show();
    localPanel.setLocalPathForTesting(temporaryDirectory.path());
    QApplication::processEvents();
    auto *localTree = localPanel.findChild<QTreeWidget *>("localFileTree");
    if (localTree == nullptr || localTree->dragEnabled() || localTree->dragDropMode() != QAbstractItemView::DropOnly)
    {
        QTextStream(stderr) << "Local file tree is not configured as a drop-only target" << Qt::endl;
        return false;
    }

    QTreeWidgetItem *targetItem = nullptr;
    for (int index = 0; index < localTree->topLevelItemCount() && targetItem == nullptr; ++index)
    {
        targetItem = findTreeItemByPath(localTree->topLevelItem(index), targetDirectory);
    }
    if (targetItem == nullptr)
    {
        QTextStream(stderr) << "Local file tree target directory is missing" << Qt::endl;
        return false;
    }

    const QPoint targetPosition = localTree->visualItemRect(targetItem).center();
    QMimeData mimeData;
    mimeData.setUrls({QUrl::fromLocalFile(sourcePath)});
    mimeData.setData(panel_shared::LocalPathMimeType, QByteArrayLiteral("1"));
    QDragEnterEvent dragEnterEvent(targetPosition, Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(localTree->viewport(), &dragEnterEvent);
    if (!dragEnterEvent.isAccepted())
    {
        QTextStream(stderr) << "Local file tree did not accept a local drag" << Qt::endl;
        return false;
    }

    QDropEvent dropEvent(QPointF(targetPosition), Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(localTree->viewport(), &dropEvent);
    QApplication::processEvents();
    const QString movedPath = QDir(targetDirectory).filePath("move-source.txt");
    if (!dropEvent.isAccepted() || QFileInfo::exists(sourcePath) || !QFileInfo::exists(movedPath))
    {
        QTextStream(stderr) << "Same-volume local tree drop did not move the source" << Qt::endl;
        return false;
    }

    localPanel.setDialogsSuppressedForTesting(true);
    QFile conflictingSource(sourcePath);
    if (!conflictingSource.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        return false;
    }
    conflictingSource.write("renamed tree drop");
    conflictingSource.close();
    QMimeData conflictingMimeData;
    conflictingMimeData.setUrls({QUrl::fromLocalFile(sourcePath)});
    conflictingMimeData.setData(panel_shared::LocalPathMimeType, QByteArrayLiteral("1"));
    QDragEnterEvent conflictingDragEnterEvent(targetPosition, Qt::CopyAction, &conflictingMimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(localTree->viewport(), &conflictingDragEnterEvent);
    QDropEvent conflictingDropEvent(QPointF(targetPosition), Qt::CopyAction, &conflictingMimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(localTree->viewport(), &conflictingDropEvent);
    QApplication::processEvents();
    const QString renamedMovedPath = QDir(targetDirectory).filePath("move-source (1).txt");
    if (!conflictingDragEnterEvent.isAccepted()
        || !conflictingDropEvent.isAccepted()
        || QFileInfo::exists(sourcePath)
        || !QFileInfo::exists(movedPath)
        || !QFileInfo::exists(renamedMovedPath))
    {
        QTextStream(stderr) << "Local tree conflict did not preserve the target and rename the moved file" << Qt::endl;
        return false;
    }

    const QString multiSourceA = QDir(temporaryDirectory.path()).filePath("multi-a.txt");
    const QString multiSourceB = QDir(temporaryDirectory.path()).filePath("multi-b.txt");
    QFile multiFileA(multiSourceA);
    QFile multiFileB(multiSourceB);
    if (!multiFileA.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || !multiFileB.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        return false;
    }
    multiFileA.close();
    multiFileB.close();
    QMimeData multiMimeData;
    multiMimeData.setUrls({QUrl::fromLocalFile(multiSourceA), QUrl::fromLocalFile(multiSourceB)});
    multiMimeData.setData(panel_shared::LocalPathMimeType, QByteArrayLiteral("1"));
    QDragEnterEvent multiDragEnterEvent(targetPosition, Qt::CopyAction, &multiMimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(localTree->viewport(), &multiDragEnterEvent);
    if (!multiDragEnterEvent.isAccepted())
    {
        QTextStream(stderr) << "Local tree did not accept an internal multi-item drag" << Qt::endl;
        return false;
    }
    QDropEvent multiDropEvent(QPointF(targetPosition), Qt::MoveAction, &multiMimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(localTree->viewport(), &multiDropEvent);
    QApplication::processEvents();
    if (!multiDropEvent.isAccepted()
        || QFileInfo::exists(multiSourceA)
        || QFileInfo::exists(multiSourceB)
        || !QFileInfo::exists(QDir(targetDirectory).filePath("multi-a.txt"))
        || !QFileInfo::exists(QDir(targetDirectory).filePath("multi-b.txt")))
    {
        QTextStream(stderr) << "Local tree did not move all selected items" << Qt::endl;
        return false;
    }

    const QString externalSourcePath = QDir(temporaryDirectory.path()).filePath("explorer-source.txt");
    QFile externalSource(externalSourcePath);
    if (!externalSource.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        return false;
    }
    externalSource.write("external copy");
    externalSource.close();
    QMimeData externalMimeData;
    externalMimeData.setUrls({QUrl::fromLocalFile(externalSourcePath)});
    QDragEnterEvent externalDragEnterEvent(targetPosition, Qt::CopyAction, &externalMimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(localTree->viewport(), &externalDragEnterEvent);
    QDropEvent externalDropEvent(QPointF(targetPosition), Qt::CopyAction, &externalMimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(localTree->viewport(), &externalDropEvent);
    QApplication::processEvents();
    if (!externalDragEnterEvent.isAccepted()
        || !externalDropEvent.isAccepted()
        || !QFileInfo::exists(externalSourcePath)
        || !QFileInfo::exists(QDir(targetDirectory).filePath("explorer-source.txt")))
    {
        QTextStream(stderr) << "External local drag did not preserve the source while copying" << Qt::endl;
        return false;
    }

    FilePanel remotePanel(FilePanel::Mode::RemotePlaceholder);
    auto *remoteTree = remotePanel.findChild<QTreeWidget *>("remoteFileTree");
    if (remoteTree == nullptr || remoteTree->dragEnabled() || remoteTree->dragDropMode() != QAbstractItemView::DropOnly)
    {
        QTextStream(stderr) << "Remote file tree is not configured as a drop-only target" << Qt::endl;
        return false;
    }
    return true;
}

void dumpTransferRows(QTreeWidget *table);

bool checkRemoteSingleItemDrops(MainWindow &window, const QString &remotePath)
{
    QTableWidget *localTable = window.findChild<QTableWidget *>("localFileTable");
    QTableWidget *remoteTable = window.findChild<QTableWidget *>("remoteFileTable");
    QTreeWidget *transferTable = window.findChild<QTreeWidget *>("transferTable");
    QTemporaryDir downloadDirectory;
    if (localTable == nullptr || remoteTable == nullptr || transferTable == nullptr || !downloadDirectory.isValid())
    {
        return false;
    }

    QJsonObject invalidPayload;
    invalidPayload.insert("version", 1);
    invalidPayload.insert("sessionId", QString());
    invalidPayload.insert("items", QJsonArray());
    QJsonObject mixedSessionPayload;
    mixedSessionPayload.insert("version", 1);
    mixedSessionPayload.insert("sessionId", "session-a");
    mixedSessionPayload.insert("items", QJsonArray{
        QJsonObject{{"path", "/a.txt"}, {"sessionId", "session-a"}, {"name", "a.txt"}},
        QJsonObject{{"path", "/b.txt"}, {"sessionId", "session-b"}, {"name", "b.txt"}}});
    if (!panel_shared::decodeRemoteTransferItems(QJsonDocument(invalidPayload).toJson(QJsonDocument::Compact)).isEmpty()
        || !panel_shared::decodeRemoteTransferItems(QJsonDocument(mixedSessionPayload).toJson(QJsonDocument::Compact)).isEmpty()
        || !panel_shared::decodeRemoteTransferItems(QByteArray(1024 * 1024 + 1, 'x')).isEmpty())
    {
        QTextStream(stderr) << "Invalid remote clipboard metadata was not rejected" << Qt::endl;
        return false;
    }
    window.setLocalPathForTesting(downloadDirectory.path());

    auto dropSingleRemoteItem = [localTable](const QString &path, bool isDirectory) {
        QJsonObject item;
        item.insert("path", path);
        item.insert("isDirectory", isDirectory);
        QJsonArray items;
        items.append(item);

        QMimeData mimeData;
        mimeData.setData(panel_shared::RemotePathMimeType, QJsonDocument(items).toJson(QJsonDocument::Compact));
        QDragEnterEvent dragEnterEvent(QPoint(8, 8), Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(localTable->viewport(), &dragEnterEvent);
        if (!dragEnterEvent.isAccepted())
        {
            return false;
        }
        QDropEvent dropEvent(QPointF(8, 8), Qt::CopyAction, &mimeData, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(localTable->viewport(), &dropEvent);
        return dropEvent.isAccepted();
    };

    if (!dropSingleRemoteItem(joinRemotePathForCheck(remotePath, "readme.txt"), false)
        || !dropSingleRemoteItem(joinRemotePathForCheck(remotePath, "download"), true))
    {
        QTextStream(stderr) << "Single remote drag was not accepted by the local panel" << Qt::endl;
        return false;
    }
    if (!waitForTransferRow(transferTable, "readme.txt", "下载", "已完成"))
    {
        QTextStream(stderr) << "Dropped remote file was not downloaded as a file" << Qt::endl;
        return false;
    }
    if (!waitForTransferRow(transferTable, "download", "下载", "已完成"))
    {
        QTextStream(stderr) << "Dropped remote directory was not downloaded as a directory" << Qt::endl;
        return false;
    }

    const QString uploadA = QDir(downloadDirectory.path()).filePath("explorer-upload-a.txt");
    const QString uploadB = QDir(downloadDirectory.path()).filePath("explorer-upload-b.txt");
    const QString uploadDirectory = QDir(downloadDirectory.path()).filePath("explorer-upload-folder");
    const QString uploadNestedFile = QDir(uploadDirectory).filePath("nested.txt");
    if (!QDir().mkpath(uploadDirectory))
    {
        return false;
    }
    QFile uploadFileA(uploadA);
    QFile uploadFileB(uploadB);
    QFile uploadNested(uploadNestedFile);
    if (!uploadFileA.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || !uploadFileB.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || !uploadNested.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        return false;
    }
    uploadFileA.write("upload a");
    uploadFileB.write("upload b");
    uploadNested.write("nested upload");
    uploadFileA.close();
    uploadFileB.close();
    uploadNested.close();
    QMimeData uploadMimeData;
    uploadMimeData.setUrls({
        QUrl::fromLocalFile(uploadA),
        QUrl::fromLocalFile(uploadB),
        QUrl::fromLocalFile(uploadDirectory)});
    const QPoint remoteBlankPosition(8, std::max(8, remoteTable->viewport()->height() - 8));
    QDragEnterEvent uploadDragEnter(remoteBlankPosition, Qt::CopyAction, &uploadMimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(remoteTable->viewport(), &uploadDragEnter);
    QDropEvent uploadDrop(QPointF(remoteBlankPosition), Qt::CopyAction, &uploadMimeData, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(remoteTable->viewport(), &uploadDrop);
    if (!uploadDragEnter.isAccepted()
        || !uploadDrop.isAccepted()
        || !QFileInfo::exists(uploadA)
        || !QFileInfo::exists(uploadB)
        || !QFileInfo(uploadDirectory).isDir()
        || !QFileInfo::exists(uploadNestedFile)
        || !waitForTransferRow(transferTable, "explorer-upload-a.txt", "上传", "已完成")
        || !waitForTransferRow(transferTable, "explorer-upload-b.txt", "上传", "已完成")
        || !waitForTransferRow(transferTable, "explorer-upload-folder", "上传", "已完成"))
    {
        QTextStream(stderr) << "External multi-item drag did not upload all local files" << Qt::endl;
        return false;
    }

#ifdef _WIN32
    const int remoteShellFolderRow = findTableRowByName(remoteTable, "explorer-upload-folder");
    if (remoteShellFolderRow < 0)
    {
        return false;
    }
    remoteTable->selectRow(remoteShellFolderRow);
    QKeyEvent copyRemoteShellFolderEvent(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(remoteTable->viewport(), &copyRemoteShellFolderEvent);
    const HRESULT folderOleInitResult = OleInitialize(nullptr);
    IDataObject *folderShellClipboard = nullptr;
    bool folderShellDataValid = copyRemoteShellFolderEvent.isAccepted()
        && SUCCEEDED(folderOleInitResult)
        && SUCCEEDED(OleGetClipboard(&folderShellClipboard))
        && folderShellClipboard != nullptr;
    FORMATETC folderDescriptorFormat{};
    folderDescriptorFormat.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    folderDescriptorFormat.dwAspect = DVASPECT_CONTENT;
    folderDescriptorFormat.lindex = -1;
    folderDescriptorFormat.tymed = TYMED_HGLOBAL;
    STGMEDIUM folderDescriptorMedium{};
    folderShellDataValid = folderShellDataValid
        && SUCCEEDED(folderShellClipboard->GetData(&folderDescriptorFormat, &folderDescriptorMedium));
    LONG nestedFileIndex = -1;
    if (folderShellDataValid)
    {
        auto *descriptor = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(folderDescriptorMedium.hGlobal));
        folderShellDataValid = descriptor != nullptr && descriptor->cItems == 2;
        if (descriptor != nullptr)
        {
            for (UINT index = 0; index < descriptor->cItems; ++index)
            {
                const QString descriptorName = QDir::fromNativeSeparators(
                    QString::fromWCharArray(descriptor->fgd[index].cFileName));
                if (descriptorName == "explorer-upload-folder/nested.txt")
                {
                    nestedFileIndex = static_cast<LONG>(index);
                    break;
                }
            }
            GlobalUnlock(folderDescriptorMedium.hGlobal);
        }
        ReleaseStgMedium(&folderDescriptorMedium);
        folderShellDataValid = folderShellDataValid && nestedFileIndex >= 0;
    }
    FORMATETC folderContentsFormat{};
    folderContentsFormat.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
    folderContentsFormat.dwAspect = DVASPECT_CONTENT;
    folderContentsFormat.lindex = nestedFileIndex;
    folderContentsFormat.tymed = TYMED_ISTREAM;
    STGMEDIUM folderContentsMedium{};
    folderShellDataValid = folderShellDataValid
        && SUCCEEDED(folderShellClipboard->GetData(&folderContentsFormat, &folderContentsMedium));
    if (folderShellDataValid)
    {
        char buffer[256]{};
        ULONG bytesRead = 0;
        folderShellDataValid = folderContentsMedium.pstm != nullptr
            && SUCCEEDED(folderContentsMedium.pstm->Read(buffer, sizeof(buffer), &bytesRead))
            && bytesRead > 0;
        ReleaseStgMedium(&folderContentsMedium);
    }
    if (folderShellClipboard != nullptr)
    {
        folderShellClipboard->Release();
    }
    if (SUCCEEDED(folderOleInitResult))
    {
        OleUninitialize();
    }
    bool shellFolderTransferGrouped = false;
    const auto shellFolderDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!shellFolderTransferGrouped && std::chrono::steady_clock::now() < shellFolderDeadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        for (int index = 0; index < transferTable->topLevelItemCount(); ++index)
        {
            QTreeWidgetItem *parent = transferTable->topLevelItem(index);
            if (parent == nullptr
                || parent->text(0) != "explorer-upload-folder"
                || parent->text(1) != "已完成"
                || parent->text(4) != "资源管理器/桌面（由 Windows 决定）"
                || parent->text(5) != "<-"
                || parent->childCount() != 1)
            {
                continue;
            }
            QTreeWidgetItem *child = parent->child(0);
            shellFolderTransferGrouped = child != nullptr
                && child->text(0) == "nested.txt"
                && child->text(1) == "已完成"
                && child->text(5) == "<-";
            if (shellFolderTransferGrouped)
            {
                break;
            }
        }
        if (!shellFolderTransferGrouped)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (!folderShellDataValid || !shellFolderTransferGrouped)
    {
        QTextStream(stderr) << "Windows Shell folder download was not grouped in the transfer table" << Qt::endl;
        dumpTransferRows(transferTable);
        return false;
    }
#endif

    const QString pasteTarget = QDir(downloadDirectory.path()).filePath("clipboard-target");
    if (!QDir().mkpath(pasteTarget))
    {
        return false;
    }
    window.setLocalPathForTesting(pasteTarget);
    const int remoteReadmeRow = findTableRowByName(remoteTable, "readme.txt");
    if (remoteReadmeRow < 0)
    {
        return false;
    }
    remoteTable->selectRow(remoteReadmeRow);
    QKeyEvent copyRemoteEvent(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(remoteTable->viewport(), &copyRemoteEvent);
    const QMimeData *remoteClipboard = QApplication::clipboard()->mimeData();
    if (!copyRemoteEvent.isAccepted()
        || remoteClipboard == nullptr
        || !remoteClipboard->hasFormat(panel_shared::RemotePathMimeType))
    {
        QTextStream(stderr) << "Remote Ctrl+C did not create transferable clipboard data" << Qt::endl;
        return false;
    }
    const QList<RemoteTransferItem> copiedItems = panel_shared::decodeRemoteTransferItems(
        remoteClipboard->data(panel_shared::RemotePathMimeType));
    if (copiedItems.size() != 1 || copiedItems.constFirst().sessionId.isEmpty())
    {
        QTextStream(stderr) << "Remote clipboard data lost its source session" << Qt::endl;
        return false;
    }

#ifdef _WIN32
    const HRESULT oleInitResult = OleInitialize(nullptr);
    IDataObject *shellClipboard = nullptr;
    bool shellDataValid = SUCCEEDED(oleInitResult)
        && SUCCEEDED(OleGetClipboard(&shellClipboard))
        && shellClipboard != nullptr;
    FORMATETC descriptorFormat{};
    descriptorFormat.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    descriptorFormat.dwAspect = DVASPECT_CONTENT;
    descriptorFormat.lindex = -1;
    descriptorFormat.tymed = TYMED_HGLOBAL;
    STGMEDIUM descriptorMedium{};
    shellDataValid = shellDataValid && SUCCEEDED(shellClipboard->GetData(&descriptorFormat, &descriptorMedium));
    if (shellDataValid)
    {
        auto *descriptor = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(descriptorMedium.hGlobal));
        shellDataValid = descriptor != nullptr
            && descriptor->cItems == 1
            && QString::fromWCharArray(descriptor->fgd[0].cFileName) == "readme.txt";
        if (descriptor != nullptr)
        {
            GlobalUnlock(descriptorMedium.hGlobal);
        }
        ReleaseStgMedium(&descriptorMedium);
    }
    FORMATETC contentsFormat{};
    contentsFormat.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
    contentsFormat.dwAspect = DVASPECT_CONTENT;
    contentsFormat.lindex = 0;
    contentsFormat.tymed = TYMED_ISTREAM;
    STGMEDIUM contentsMedium{};
    shellDataValid = shellDataValid && SUCCEEDED(shellClipboard->GetData(&contentsFormat, &contentsMedium));
    if (shellDataValid)
    {
        char buffer[256]{};
        ULONG bytesRead = 0;
        shellDataValid = contentsMedium.pstm != nullptr
            && SUCCEEDED(contentsMedium.pstm->Read(buffer, sizeof(buffer), &bytesRead))
            && bytesRead > 0;
        ReleaseStgMedium(&contentsMedium);
    }
    if (shellClipboard != nullptr)
    {
        shellClipboard->Release();
    }
    if (SUCCEEDED(oleInitResult))
    {
        OleUninitialize();
    }
    if (!shellDataValid)
    {
        QTextStream(stderr) << "Remote Ctrl+C did not expose a Windows Shell file descriptor" << Qt::endl;
        return false;
    }
    bool shellDownloadRecorded = false;
    const auto shellTransferDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!shellDownloadRecorded && std::chrono::steady_clock::now() < shellTransferDeadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        for (int index = 0; index < transferTable->topLevelItemCount(); ++index)
        {
            QTreeWidgetItem *item = transferTable->topLevelItem(index);
            if (item != nullptr
                && item->text(0) == "readme.txt"
                && item->text(1) == "已完成"
                && item->text(4) == "资源管理器/桌面（由 Windows 决定）"
                && item->text(5) == "<-"
                && item->text(6) == "/home/testuser/remote_test/readme.txt")
            {
                shellDownloadRecorded = true;
                break;
            }
        }
        if (!shellDownloadRecorded)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (!shellDownloadRecorded)
    {
        QTextStream(stderr) << "Windows Shell content download was not recorded in the transfer table" << Qt::endl;
        dumpTransferRows(transferTable);
        return false;
    }
#endif

    QKeyEvent pasteLocalEvent(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
    QApplication::sendEvent(localTable->viewport(), &pasteLocalEvent);
    const QString pastedLocalFile = QDir(pasteTarget).filePath("readme.txt");
    bool localClipboardDownloadCompleted = false;
    const auto localClipboardDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!localClipboardDownloadCompleted && std::chrono::steady_clock::now() < localClipboardDeadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        for (int index = 0; index < transferTable->topLevelItemCount(); ++index)
        {
            QTreeWidgetItem *item = transferTable->topLevelItem(index);
            if (item != nullptr
                && item->text(0) == "readme.txt"
                && item->text(1) == "已完成"
                && QDir::fromNativeSeparators(item->text(4)) == QDir::fromNativeSeparators(pastedLocalFile)
                && item->text(5) == "<-")
            {
                localClipboardDownloadCompleted = QFileInfo::exists(pastedLocalFile);
                break;
            }
        }
        if (!localClipboardDownloadCompleted)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (!pasteLocalEvent.isAccepted() || !localClipboardDownloadCompleted)
    {
        QTextStream(stderr) << "Local Ctrl+V did not download the remote clipboard item" << Qt::endl;
        return false;
    }

    const QString folderPasteTarget = QDir(downloadDirectory.path()).filePath("folder-clipboard-target");
    if (!QDir().mkpath(folderPasteTarget))
    {
        return false;
    }
    window.setLocalPathForTesting(folderPasteTarget);
    const int remoteDownloadRow = findTableRowByName(remoteTable, "download");
    if (remoteDownloadRow < 0)
    {
        return false;
    }
    remoteTable->selectRow(remoteDownloadRow);
    QKeyEvent copyRemoteDirectoryEvent(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(remoteTable->viewport(), &copyRemoteDirectoryEvent);
    const QMimeData *directoryClipboard = QApplication::clipboard()->mimeData();
    QKeyEvent pasteRemoteDirectoryEvent(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
    QApplication::sendEvent(localTable->viewport(), &pasteRemoteDirectoryEvent);
    const QString pastedLocalDirectory = QDir(folderPasteTarget).filePath("download");
    bool localDirectoryClipboardDownloadCompleted = false;
    const auto localDirectoryClipboardDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!localDirectoryClipboardDownloadCompleted
        && std::chrono::steady_clock::now() < localDirectoryClipboardDeadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        for (int index = 0; index < transferTable->topLevelItemCount(); ++index)
        {
            QTreeWidgetItem *item = transferTable->topLevelItem(index);
            if (item != nullptr
                && item->text(0) == "download"
                && item->text(1) == "已完成"
                && QDir::fromNativeSeparators(item->text(4)) == QDir::fromNativeSeparators(pastedLocalDirectory)
                && item->text(5) == "<-")
            {
                localDirectoryClipboardDownloadCompleted = QFileInfo(pastedLocalDirectory).isDir();
                break;
            }
        }
        if (!localDirectoryClipboardDownloadCompleted)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (!copyRemoteDirectoryEvent.isAccepted()
        || directoryClipboard == nullptr
        || !directoryClipboard->hasFormat(panel_shared::RemotePathMimeType)
        || !pasteRemoteDirectoryEvent.isAccepted()
        || !localDirectoryClipboardDownloadCompleted)
    {
        QTextStream(stderr) << "Remote folder copy/paste did not preserve the empty directory" << Qt::endl;
        return false;
    }

    const QString clipboardUpload = QDir(downloadDirectory.path()).filePath("clipboard-upload.txt");
    QFile clipboardUploadFile(clipboardUpload);
    if (!clipboardUploadFile.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        return false;
    }
    clipboardUploadFile.write("clipboard upload");
    clipboardUploadFile.close();
    window.setLocalPathForTesting(downloadDirectory.path());
    QApplication::processEvents();
    const int localClipboardRow = findTableRowByName(localTable, "clipboard-upload.txt");
    if (localClipboardRow < 0)
    {
        return false;
    }
    localTable->selectRow(localClipboardRow);
    QKeyEvent copyLocalEvent(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(localTable->viewport(), &copyLocalEvent);
    const QMimeData *localClipboard = QApplication::clipboard()->mimeData();
    if (!copyLocalEvent.isAccepted()
        || localClipboard == nullptr
        || localClipboard->urls().size() != 1
        || localClipboard->urls().constFirst().toLocalFile() != clipboardUpload)
    {
        QTextStream(stderr) << "Local Ctrl+C did not create a standard file URL" << Qt::endl;
        return false;
    }
    remoteTable->clearSelection();
    remoteTable->setCurrentCell(-1, -1);
    QKeyEvent pasteRemoteEvent(QEvent::KeyPress, Qt::Key_V, Qt::ControlModifier);
    QApplication::sendEvent(remoteTable->viewport(), &pasteRemoteEvent);
    if (!pasteRemoteEvent.isAccepted()
        || !waitForTransferRow(transferTable, "clipboard-upload.txt", "上传", "已完成"))
    {
        QTextStream(stderr) << "Remote Ctrl+V did not upload the local clipboard item" << Qt::endl;
        return false;
    }
#ifdef _WIN32
    WindowsShellDragFile shellDirectory;
    shellDirectory.fileName = L"folder";
    shellDirectory.isDirectory = true;
    WindowsShellDragFile shellFile;
    shellFile.fileName = L"folder\\nested.txt";
    shellFile.size = 5;
    const QString shellMaterializedPath = QDir(downloadDirectory.path()).filePath("shell-materialized.txt");
    shellFile.materialize = [shellMaterializedPath](
                                std::wstring &temporaryPath,
                                const std::shared_ptr<std::atomic_bool> &canceled) {
        if (canceled->load())
        {
            return false;
        }
        QFile file(shellMaterializedPath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write("hello") != 5)
        {
            return false;
        }
        file.close();
        temporaryPath = QDir::toNativeSeparators(shellMaterializedPath).toStdWString();
        return true;
    };
    int lazyFileProviderCalls = 0;
    bool shellTreeValid = setWindowsShellClipboard(
        [&lazyFileProviderCalls, shellDirectory, shellFile](std::vector<WindowsShellDragFile> &files) {
            ++lazyFileProviderCalls;
            files = {shellDirectory, shellFile};
            return true;
        },
        QByteArray("shell-smoke"));
    const HRESULT treeOleInit = OleInitialize(nullptr);
    IDataObject *treeClipboard = nullptr;
    shellTreeValid = shellTreeValid
        && SUCCEEDED(treeOleInit)
        && SUCCEEDED(OleGetClipboard(&treeClipboard))
        && treeClipboard != nullptr;
    IDataObjectAsyncCapability *asyncCapability = nullptr;
    WINBOOL asyncMode = FALSE;
    WINBOOL asyncOperationRunning = FALSE;
    shellTreeValid = shellTreeValid
        && SUCCEEDED(treeClipboard->QueryInterface(
            IID_IDataObjectAsyncCapability,
            reinterpret_cast<void **>(&asyncCapability)))
        && asyncCapability != nullptr
        && SUCCEEDED(asyncCapability->GetAsyncMode(&asyncMode))
        && asyncMode == TRUE
        && SUCCEEDED(asyncCapability->StartOperation(nullptr))
        && SUCCEEDED(asyncCapability->InOperation(&asyncOperationRunning))
        && asyncOperationRunning == TRUE
        && SUCCEEDED(asyncCapability->EndOperation(S_OK, nullptr, DROPEFFECT_COPY));
    asyncOperationRunning = TRUE;
    shellTreeValid = shellTreeValid
        && SUCCEEDED(asyncCapability->InOperation(&asyncOperationRunning))
        && asyncOperationRunning == FALSE;
    if (asyncCapability != nullptr)
    {
        asyncCapability->Release();
    }
    FORMATETC applicationMetadataFormat{};
    applicationMetadataFormat.cfFormat = static_cast<CLIPFORMAT>(
        RegisterClipboardFormatW(L"application/x-dirbridge-remote-paths"));
    applicationMetadataFormat.dwAspect = DVASPECT_CONTENT;
    applicationMetadataFormat.lindex = -1;
    applicationMetadataFormat.tymed = TYMED_HGLOBAL;
    STGMEDIUM applicationMetadataMedium{};
    shellTreeValid = shellTreeValid
        && SUCCEEDED(treeClipboard->GetData(&applicationMetadataFormat, &applicationMetadataMedium))
        && lazyFileProviderCalls == 0;
    if (applicationMetadataMedium.hGlobal != nullptr)
    {
        ReleaseStgMedium(&applicationMetadataMedium);
    }
    FORMATETC treeDescriptorFormat{};
    treeDescriptorFormat.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
    treeDescriptorFormat.dwAspect = DVASPECT_CONTENT;
    treeDescriptorFormat.lindex = -1;
    treeDescriptorFormat.tymed = TYMED_HGLOBAL;
    STGMEDIUM treeDescriptorMedium{};
    shellTreeValid = shellTreeValid && SUCCEEDED(treeClipboard->GetData(&treeDescriptorFormat, &treeDescriptorMedium));
    shellTreeValid = shellTreeValid && lazyFileProviderCalls == 1;
    if (shellTreeValid)
    {
        auto *descriptor = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(treeDescriptorMedium.hGlobal));
        shellTreeValid = descriptor != nullptr
            && descriptor->cItems == 2
            && (descriptor->fgd[0].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
            && QString::fromWCharArray(descriptor->fgd[1].cFileName) == "folder\\nested.txt";
        if (descriptor != nullptr)
        {
            GlobalUnlock(treeDescriptorMedium.hGlobal);
        }
        ReleaseStgMedium(&treeDescriptorMedium);
    }
    FORMATETC treeContentsFormat{};
    treeContentsFormat.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
    treeContentsFormat.dwAspect = DVASPECT_CONTENT;
    treeContentsFormat.lindex = 1;
    treeContentsFormat.tymed = TYMED_ISTREAM;
    STGMEDIUM treeContentsMedium{};
    shellTreeValid = shellTreeValid && SUCCEEDED(treeClipboard->GetData(&treeContentsFormat, &treeContentsMedium));
    if (shellTreeValid)
    {
        char content[8]{};
        ULONG bytesRead = 0;
        shellTreeValid = treeContentsMedium.pstm != nullptr
            && SUCCEEDED(treeContentsMedium.pstm->Read(content, sizeof(content), &bytesRead))
            && QByteArray(content, static_cast<int>(bytesRead)) == "hello";
        ReleaseStgMedium(&treeContentsMedium);
    }
    if (treeClipboard != nullptr)
    {
        treeClipboard->Release();
    }
    if (SUCCEEDED(treeOleInit))
    {
        OleUninitialize();
    }
    clearWindowsShellClipboard();
    if (!shellTreeValid)
    {
        QTextStream(stderr) << "Windows Shell directory descriptor or delayed file stream is invalid" << Qt::endl;
        return false;
    }
#endif
    QApplication::clipboard()->clear();
    return true;
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

QTreeWidgetItem *findLatestTopLevelTransferRowWithChildren(
    QTreeWidget *table,
    const QString &name,
    const QString &direction,
    const QString &status,
    int minimumChildCount)
{
    if (table == nullptr)
    {
        return nullptr;
    }

    const QString arrow = direction == "上传" ? "->" : "<-";
    for (int index = table->topLevelItemCount() - 1; index >= 0; --index)
    {
        QTreeWidgetItem *item = table->topLevelItem(index);
        if (item != nullptr
            && item->text(0) == name
            && item->text(1) == status
            && item->text(5) == arrow
            && item->childCount() >= minimumChildCount)
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
    if (remoteTable->columnCount() != 5 || readmeRow < 0 || remoteTable->item(readmeRow, 1) == nullptr
        || remoteTable->horizontalHeaderItem(4) == nullptr
        || remoteTable->horizontalHeaderItem(4)->text() != "权限")
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

    const QString editedRemotePath = joinRemotePathForCheck(remotePath, "readme.txt");
    const int rowAfterFirstUpload = findTableRowByName(remoteTable, "readme.txt");
    if (rowAfterFirstUpload < 0 || remoteTable->item(rowAfterFirstUpload, 1) == nullptr)
    {
        QTextStream(stderr) << "External edit smoke lost the synchronized remote row" << Qt::endl;
        return false;
    }
    const QString sizeAfterFirstUpload = remoteTable->item(rowAfterFirstUpload, 1)->text();
    const QString firstCachePath = openedCachePath;
    window.closeRemoteEditForTesting(editedRemotePath);
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    if (QFileInfo::exists(firstCachePath))
    {
        QTextStream(stderr) << "External edit close did not remove the synchronized cache" << Qt::endl;
        return false;
    }

    openedCachePath.clear();
    window.editRemoteFileForTesting(editedRemotePath);
    const auto reopenDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (openedCachePath.isEmpty() && std::chrono::steady_clock::now() < reopenDeadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (openedCachePath.isEmpty())
    {
        QTextStream(stderr) << "External edit smoke could not reopen a closed edit document" << Qt::endl;
        return false;
    }

    const QByteArray reopenedContent("external edit smoke second synchronized version\n");
    QSaveFile reopenedFile(openedCachePath);
    if (!reopenedFile.open(QIODevice::WriteOnly)
        || reopenedFile.write(reopenedContent) != reopenedContent.size()
        || !reopenedFile.commit())
    {
        QTextStream(stderr) << "External edit smoke could not save the reopened cache file" << Qt::endl;
        return false;
    }

    const auto secondUploadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool secondUploadCompleted = false;
    while (std::chrono::steady_clock::now() < secondUploadDeadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        const int refreshedRow = findTableRowByName(remoteTable, "readme.txt");
        if (refreshedRow >= 0
            && remoteTable->item(refreshedRow, 1) != nullptr
            && remoteTable->item(refreshedRow, 1)->text() != sizeAfterFirstUpload)
        {
            secondUploadCompleted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (!secondUploadCompleted)
    {
        QTextStream(stderr) << "External edit smoke did not synchronize the reopened file" << Qt::endl;
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
        window.setRemotePermissionsForTesting(joinRemotePathForCheck(remotePath, "readme.txt"), 0600);
        if (!waitForTablePermission(remoteTable, "readme.txt", "-rw-------"))
        {
            QTextStream(stderr) << "Remote permission update did not refresh the file table" << Qt::endl;
            ok = false;
        }
    }

    if (useFakeBackend)
    {
        ok = checkExternalEditWorkflow(window, remotePath, remoteTable) && ok;
        ok = checkRemoteSingleItemDrops(window, remotePath) && ok;
    }

    const QString directoryName = expectedNames.isEmpty() ? QString() : expectedNames.first();
    const QString expectedChildPath = joinRemotePathForCheck(remotePath, directoryName);
    remotePathEdit->setText(expectedChildPath);
    QMetaObject::invokeMethod(remotePathEdit, "returnPressed", Qt::DirectConnection);
    if (!waitForRemoteConnected(remotePanel, expectedChildPath))
    {
        QTextStream(stderr) << "Remote address bar jump is unexpected: " << remotePathEdit->text() << Qt::endl;
        ok = false;
    }

    remotePathEdit->setText(joinRemotePathForCheck(remotePath, "dirbridge_missing_path_for_smoke"));
    QMetaObject::invokeMethod(remotePathEdit, "returnPressed", Qt::DirectConnection);
    if (!waitForRemoteLoadFailure(remotePanel, expectedChildPath))
    {
        QTextStream(stderr) << "Missing remote address did not restore the current path: " << remotePathEdit->text() << Qt::endl;
        ok = false;
    }

    remoteUpButton->click();
    if (!waitForRemoteConnected(remotePanel, remotePath))
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
    if (!waitForRemoteConnected(remotePanel, expectedChildPath))
    {
        QTextStream(stderr) << "Remote path after double click is unexpected: " << remotePathEdit->text() << Qt::endl;
        ok = false;
    }

    remoteUpButton->click();
    if (!waitForRemoteConnected(remotePanel, remotePath))
    {
        QTextStream(stderr) << "Remote path after up navigation is unexpected: " << remotePathEdit->text() << Qt::endl;
        ok = false;
    }

    disconnectAction->trigger();
    if (!waitForRemoteDisconnected(remotePanel) || remotePathEdit->text() != "/")
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
bool connectFakeRemoteSession(
    MainWindow &window,
    const QString &remotePath,
    FakeRemoteFileSystem **connectedFileSystem = nullptr)
{
    auto fileSystem = std::make_unique<FakeRemoteFileSystem>();
    FakeRemoteFileSystem *fileSystemPointer = fileSystem.get();
    window.setRemoteFileSystemForTesting(std::move(fileSystem));

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
    if (connectedFileSystem != nullptr)
    {
        *connectedFileSystem = fileSystemPointer;
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
 * @brief 验证慢速远程目录导航不会同步阻塞 UI 调用线程。
 * @return 导航请求快速返回且最终加载目标目录时返回 true。
 */
bool checkRemoteNavigationResponsiveness()
{
    MainWindow window(DependencyCheckResult{});
    window.setDialogsSuppressedForTesting(true);
    auto fileSystem = std::make_unique<SlowFakeRemoteFileSystem>();
    SlowFakeRemoteFileSystem *fileSystemObserver = fileSystem.get();
    window.setRemoteFileSystemForTesting(std::move(fileSystem));

    QComboBox *protocolCombo = window.findChild<QComboBox *>("quickProtocolCombo");
    QLineEdit *hostEdit = window.findChild<QLineEdit *>("quickHostEdit");
    QLineEdit *portEdit = window.findChild<QLineEdit *>("quickPortEdit");
    QLineEdit *userEdit = window.findChild<QLineEdit *>("quickUserEdit");
    QLineEdit *quickRemotePathEdit = window.findChild<QLineEdit *>("quickRemotePathEdit");
    QPushButton *connectButton = window.findChild<QPushButton *>("quickConnectButton");
    QTabWidget *remoteTabs = window.findChild<QTabWidget *>("remoteTabs");
    if (protocolCombo == nullptr || hostEdit == nullptr || portEdit == nullptr || userEdit == nullptr
        || quickRemotePathEdit == nullptr || connectButton == nullptr || remoteTabs == nullptr)
    {
        QTextStream(stderr) << "Remote navigation responsiveness prerequisites are incomplete" << Qt::endl;
        return false;
    }

    const QString rootPath = "/home/testuser/remote_test";
    const QString childPath = rootPath + "/download";
    protocolCombo->setCurrentText("SFTP");
    hostEdit->setText("slow-navigation-host");
    portEdit->setText("22");
    userEdit->setText("testuser");
    quickRemotePathEdit->setText(rootPath);
    connectButton->click();
    if (!waitForRemoteConnected(remoteTabs->currentWidget(), rootPath))
    {
        QTextStream(stderr) << "Slow remote navigation fixture did not connect" << Qt::endl;
        return false;
    }
    const int callsAfterConnect = fileSystemObserver->listDirectoryCallCount();
    if (callsAfterConnect < 1)
    {
        QTextStream(stderr) << "Initial remote directory load did not issue a listing request" << Qt::endl;
        return false;
    }

    QWidget *remotePanel = remoteTabs->currentWidget();
    QLineEdit *remotePathEdit = remotePanel == nullptr ? nullptr : remotePanel->findChild<QLineEdit *>("remotePathEdit");
    if (remotePathEdit == nullptr)
    {
        return false;
    }

    const auto startedAt = std::chrono::steady_clock::now();
    remotePathEdit->setText(childPath);
    QMetaObject::invokeMethod(remotePathEdit, "returnPressed", Qt::DirectConnection);
    const auto requestElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);
    if (requestElapsed >= std::chrono::milliseconds(100))
    {
        QTextStream(stderr) << "Remote navigation blocked the UI thread for " << requestElapsed.count() << " ms" << Qt::endl;
        return false;
    }
    if (!waitForRemoteConnected(remotePanel, childPath))
    {
        QTextStream(stderr) << "Slow remote navigation did not reach the requested path" << Qt::endl;
        return false;
    }
    const int navigationCalls = fileSystemObserver->listDirectoryCallCount() - callsAfterConnect;
    if (navigationCalls != 1)
    {
        QTextStream(stderr) << "Remote navigation should issue one listing request, got "
                            << navigationCalls << Qt::endl;
        return false;
    }
    return true;
}

/**
 * @brief 验证重新启动窗口后会从站点配置恢复文件树状态。
 */
bool checkPersistedFileTreeVisibilityAfterRestart()
{
    QTemporaryDir temporaryDirectory;
    if (!temporaryDirectory.isValid())
    {
        QTextStream(stderr) << "Could not create persisted file-tree test directory" << Qt::endl;
        return false;
    }

    const std::filesystem::path siteConfigPath = std::filesystem::path(temporaryDirectory.path().toStdWString()) / "sites.json";
    SiteProfile profile;
    profile.id = "persisted-file-tree-site";
    profile.name = "Persisted File Tree Site";
    profile.protocol = RemoteProtocol::Sftp;
    profile.host = "fake-host";
    profile.port = 22;
    profile.username = "testuser";
    profile.defaultRemotePath = "/home/testuser/remote_test";
    profile.fileTreeVisible = false;
    SiteStore(siteConfigPath).save({profile});

    DependencyCheckResult dependencyCheck;
    dependencyCheck.siteConfigPath = siteConfigPath.string();
    MainWindow window(dependencyCheck);
    window.setDialogsSuppressedForTesting(true);
    window.setRemoteFileSystemForTesting(std::make_unique<FakeRemoteFileSystem>());

    QTreeWidget *sessionTree = window.findChild<QTreeWidget *>("sessionManagerTree");
    QTabWidget *remoteTabs = window.findChild<QTabWidget *>("remoteTabs");
    QTreeWidgetItem *siteItem = nullptr;
    if (sessionTree != nullptr)
    {
        QTreeWidgetItemIterator iterator(sessionTree);
        while (*iterator != nullptr)
        {
            if ((*iterator)->text(0).contains("Persisted File Tree Site"))
            {
                siteItem = *iterator;
                break;
            }
            ++iterator;
        }
    }
    if (sessionTree == nullptr || remoteTabs == nullptr || siteItem == nullptr)
    {
        QTextStream(stderr) << "Persisted file-tree site was not loaded after restart" << Qt::endl;
        return false;
    }

    QMetaObject::invokeMethod(
        sessionTree,
        "itemDoubleClicked",
        Qt::DirectConnection,
        Q_ARG(QTreeWidgetItem *, siteItem),
        Q_ARG(int, 0));
    if (!waitForRemoteConnected(remoteTabs->currentWidget(), QString::fromStdString(profile.defaultRemotePath)))
    {
        QTextStream(stderr) << "Persisted file-tree site did not reconnect" << Qt::endl;
        return false;
    }

    QAction *fileTreeAction = findActionByText(window, "文件树");
    QTreeWidget *remoteTree = remoteTabs->currentWidget()->findChild<QTreeWidget *>("remoteFileTree");
    if (fileTreeAction == nullptr || fileTreeAction->isChecked() || remoteTree == nullptr || !remoteTree->isHidden())
    {
        QTextStream(stderr) << "Restarted site did not restore hidden file-tree state" << Qt::endl;
        return false;
    }

    fileTreeAction->setChecked(true);
    QApplication::processEvents();
    const std::vector<SiteProfile> reloadedSites = SiteStore(siteConfigPath).load();
    if (reloadedSites.size() != 1 || !reloadedSites.front().fileTreeVisible)
    {
        QTextStream(stderr) << "File-tree toggle was not persisted back to the site config" << Qt::endl;
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
 * @brief 验证本地标签名称和本地文件树状态能够同步并持久化。
 * @return 有效目录导航会更新标签、无效导航不会污染标签且重启能恢复本地树状态时返回 true。
 */
bool checkLocalPanelStateWorkflow()
{
    QTemporaryDir tempRoot;
    if (!tempRoot.isValid())
    {
        return false;
    }

    const std::filesystem::path configRoot = tempRoot.path().toStdString();
    const std::filesystem::path childDirectory = configRoot / "local-state-dir";
    std::filesystem::create_directories(childDirectory);

    DependencyCheckResult dependencyCheck;
    dependencyCheck.siteConfigPath = (configRoot / "sites.json").string();
    {
        MainWindow window(dependencyCheck);
        window.setDialogsSuppressedForTesting(true);
        QTabWidget *localTabs = window.findChild<QTabWidget *>("localTabs");
        QTreeWidget *localTree = window.findChild<QTreeWidget *>("localFileTree");
        if (localTabs == nullptr || localTree == nullptr || localTabs->count() != 1)
        {
            return false;
        }

        window.setLocalPathForTesting(QString::fromStdString(childDirectory.string()));
        QApplication::processEvents();
        if (localTabs->tabText(0) != "本地：local-state-dir")
        {
            return false;
        }

        const QString titleBeforeInvalidNavigation = localTabs->tabText(0);
        window.setLocalPathForTesting(QString::fromStdString((configRoot / "不存在的目录").string()));
        QApplication::processEvents();
        if (localTabs->tabText(0) != titleBeforeInvalidNavigation)
        {
            return false;
        }

        QAction *fileTreeAction = findActionByText(window, "文件树");
        if (fileTreeAction == nullptr)
        {
            return false;
        }
        fileTreeAction->setChecked(false);
        QApplication::processEvents();
        if (!localTree->isHidden())
        {
            return false;
        }
    }

    MainWindow restoredWindow(dependencyCheck);
    restoredWindow.setDialogsSuppressedForTesting(true);
    QTreeWidget *restoredTree = restoredWindow.findChild<QTreeWidget *>("localFileTree");
    return restoredTree != nullptr && restoredTree->isHidden();
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
    grouped.fileTreeVisible = false;
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
    QAction *fileTreeAction = findActionByText(window, "文件树");
    QTreeWidget *remoteTree = remoteTabs->currentWidget()->findChild<QTreeWidget *>("remoteFileTree");
    if (fileTreeAction == nullptr || fileTreeAction->isChecked() || remoteTree == nullptr || !remoteTree->isHidden())
    {
        QTextStream(stderr) << "Saved site did not restore its hidden file-tree state" << Qt::endl;
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
    QAction *fileTreeAction = findActionByText(window, "文件树");
    if (remoteTabs == nullptr || disconnectAction == nullptr || fileTreeAction == nullptr)
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
    window.setCurrentRemoteFileTreeVisibleForTesting(false);
    QApplication::processEvents();
    QTreeWidget *firstRemoteTree = remoteTabs->currentWidget()->findChild<QTreeWidget *>("remoteFileTree");
    if (firstRemoteTree == nullptr || !firstRemoteTree->isHidden())
    {
        QTextStream(stderr) << "First session did not retain its hidden file-tree state" << Qt::endl;
        return false;
    }
    if (!connectFakeRemoteSession(window, secondPath))
    {
        return false;
    }
    const int secondIndex = remoteTabs->currentIndex();
    QTreeWidget *secondRemoteTree = remoteTabs->currentWidget()->findChild<QTreeWidget *>("remoteFileTree");
    if (fileTreeAction->isChecked() || secondRemoteTree == nullptr || secondRemoteTree->isHidden()
        || firstRemoteTree == nullptr || !firstRemoteTree->isHidden())
    {
        QTextStream(stderr) << "Session-local file-tree toggle leaked into another session" << Qt::endl;
        return false;
    }
    if (remoteTabs->count() != initialCount + 2 || firstIndex == secondIndex)
    {
        QTextStream(stderr) << "Remote tabs were not added independently" << Qt::endl;
        return false;
    }

    fileTreeAction->setChecked(true);
    QApplication::processEvents();
    if (!fileTreeAction->isChecked() || firstRemoteTree->isHidden() || secondRemoteTree->isHidden())
    {
        QTextStream(stderr) << "Global file-tree action did not show every session tree" << Qt::endl;
        return false;
    }
    fileTreeAction->setChecked(false);
    QApplication::processEvents();
    if (fileTreeAction->isChecked() || !firstRemoteTree->isHidden() || !secondRemoteTree->isHidden())
    {
        QTextStream(stderr) << "Global file-tree action did not hide every session tree" << Qt::endl;
        return false;
    }
    fileTreeAction->setChecked(true);
    QApplication::processEvents();

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
    FakeRemoteFileSystem *fileSystem = nullptr;
    if (!connectFakeRemoteSession(window, remotePath, &fileSystem) || fileSystem == nullptr)
    {
        return false;
    }
    QTreeWidget *transferTable = window.findChild<QTreeWidget *>("transferTable");
    QTabWidget *remoteTabs = window.findChild<QTabWidget *>("remoteTabs");
    if (transferTable == nullptr || remoteTabs == nullptr || remoteTabs->currentWidget() == nullptr)
    {
        QTextStream(stderr) << "Directory operation UI objects are missing" << Qt::endl;
        return false;
    }

    const QString uploadDirectory = remotePath + "/upload";
    const QString sourceFile = remotePath + "/readme.txt";
    const QString conflictingFile = uploadDirectory + "/readme.txt";
    RemoteOperationResult result = fileSystem->createFile(conflictingFile.toStdString());
    if (!result.success)
    {
        QTextStream(stderr) << "Unable to prepare remote file conflict smoke test" << Qt::endl;
        return false;
    }
    window.moveRemotePathsForTesting({sourceFile}, uploadDirectory);
    const QString renamedFile = uploadDirectory + "/readme (1).txt";
    const std::vector<FileItem> rootItemsAfterFileRename = fileSystem->listDirectory(remotePath.toStdString());
    const std::vector<FileItem> uploadItemsAfterFileRename = fileSystem->listDirectory(uploadDirectory.toStdString());
    if (containsRemotePath(rootItemsAfterFileRename, sourceFile.toStdString(), FileItemType::File)
        || !containsRemotePath(uploadItemsAfterFileRename, conflictingFile.toStdString(), FileItemType::File)
        || !containsRemotePath(uploadItemsAfterFileRename, renamedFile.toStdString(), FileItemType::File))
    {
        QTextStream(stderr) << "Remote file conflict did not preserve the target and rename the moved file" << Qt::endl;
        return false;
    }
    result = fileSystem->removeFile(conflictingFile.toStdString());
    result = result.success ? fileSystem->removeFile(renamedFile.toStdString()) : result;
    if (!result.success)
    {
        QTextStream(stderr) << "Unable to clean remote file conflict smoke test" << Qt::endl;
        return false;
    }

    const QString sourceConflictDirectory = remotePath + "/conflict-folder";
    const QString targetConflictDirectory = uploadDirectory + "/conflict-folder";
    const QString renamedConflictDirectory = uploadDirectory + "/conflict-folder (1)";
    const QString sourceConflictFile = sourceConflictDirectory + "/source-only.txt";
    const QString targetConflictFile = targetConflictDirectory + "/target-only.txt";
    result = fileSystem->createDirectory(sourceConflictDirectory.toStdString());
    result = result.success ? fileSystem->createDirectory(targetConflictDirectory.toStdString()) : result;
    result = result.success ? fileSystem->createFile(sourceConflictFile.toStdString()) : result;
    result = result.success ? fileSystem->createFile(targetConflictFile.toStdString()) : result;
    if (!result.success)
    {
        QTextStream(stderr) << "Unable to prepare remote directory conflict smoke test" << Qt::endl;
        return false;
    }
    window.moveRemotePathsForTesting({sourceConflictDirectory}, uploadDirectory);
    if (!waitForRemoteConnected(remoteTabs->currentWidget(), remotePath))
    {
        QTextStream(stderr) << "Remote directory conflict move did not finish refreshing the remote panel" << Qt::endl;
        return false;
    }
    const std::vector<FileItem> originalTargetItems = fileSystem->listDirectory(targetConflictDirectory.toStdString());
    const std::vector<FileItem> renamedTargetItems = fileSystem->listDirectory(renamedConflictDirectory.toStdString());
    const std::vector<FileItem> rootItemsAfterRename = fileSystem->listDirectory(remotePath.toStdString());
    if (!containsRemotePath(originalTargetItems, targetConflictFile.toStdString(), FileItemType::File)
        || !containsRemotePath(renamedTargetItems, (renamedConflictDirectory + "/source-only.txt").toStdString(), FileItemType::File)
        || containsRemotePath(rootItemsAfterRename, sourceConflictDirectory.toStdString(), FileItemType::Directory))
    {
        QTextStream(stderr) << "Remote directory conflict did not preserve the target and rename the moved directory" << Qt::endl;
        return false;
    }
    result = fileSystem->removeFile(targetConflictFile.toStdString());
    result = result.success ? fileSystem->removeDirectory(targetConflictDirectory.toStdString()) : result;
    result = result.success ? fileSystem->removeFile((renamedConflictDirectory + "/source-only.txt").toStdString()) : result;
    result = result.success ? fileSystem->removeDirectory(renamedConflictDirectory.toStdString()) : result;
    if (!result.success)
    {
        QTextStream(stderr) << "Unable to clean remote directory conflict smoke test" << Qt::endl;
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

    const std::filesystem::path localConflictUpload = uploadRoot / "file-conflict.txt";
    {
        std::ofstream output(localConflictUpload, std::ios::binary | std::ios::trunc);
        output << "DirBridge file conflict upload\n";
    }
    const QString remoteConflictFile = remotePath + "/file-conflict.txt";
    const QString renamedRemoteConflictFile = remotePath + "/file-conflict (1).txt";
    result = fileSystem->createFile(remoteConflictFile.toStdString());
    if (!result.success)
    {
        QTextStream(stderr) << "Unable to prepare file upload conflict test" << Qt::endl;
        return false;
    }
    window.uploadLocalFileForTesting(QString::fromStdString(localConflictUpload.u8string()));
    if (!waitForRemotePath(fileSystem, remotePath, renamedRemoteConflictFile, FileItemType::File))
    {
        QTextStream(stderr) << "File upload conflict did not use a renamed remote target" << Qt::endl;
        return false;
    }

    const std::filesystem::path existingLocalDownload = downloadRoot / "file-conflict.txt";
    {
        std::ofstream output(existingLocalDownload, std::ios::binary | std::ios::trunc);
        output << "preserve existing local file\n";
    }
    window.downloadRemoteFileForTesting(remoteConflictFile);
    const std::filesystem::path renamedLocalDownload = downloadRoot / "file-conflict (1).txt";
    const auto renamedFileDownloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!std::filesystem::is_regular_file(renamedLocalDownload)
        && std::chrono::steady_clock::now() < renamedFileDownloadDeadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!std::filesystem::is_regular_file(existingLocalDownload)
        || !std::filesystem::is_regular_file(renamedLocalDownload))
    {
        QTextStream(stderr) << "File download conflict did not preserve the target and use a renamed local file" << Qt::endl;
        return false;
    }

    const auto armOverwriteDialog = [](QTimer &timer, bool &observed) {
        QObject::connect(&timer, &QTimer::timeout, &timer, [&timer, &observed]() {
            QWidget *modalWidget = QApplication::activeModalWidget();
            if (modalWidget == nullptr || modalWidget->objectName() != "transferConflictDialog")
            {
                return;
            }
            auto *actionCombo = modalWidget->findChild<QComboBox *>("conflictActionCombo");
            auto *confirmButton = modalWidget->findChild<QPushButton *>("conflictConfirmButton");
            auto *targetDetails = modalWidget->findChild<QLabel *>("conflictTargetDetails");
            auto *sourceDetails = modalWidget->findChild<QLabel *>("conflictSourceDetails");
            if (actionCombo == nullptr || confirmButton == nullptr
                || targetDetails == nullptr || sourceDetails == nullptr
                || targetDetails->text().contains("修改时间未知")
                || sourceDetails->text().contains("修改时间未知"))
            {
                return;
            }
            const int overwriteIndex = actionCombo->findText("覆盖");
            if (overwriteIndex < 0)
            {
                return;
            }
            actionCombo->setCurrentIndex(overwriteIndex);
            observed = true;
            timer.stop();
            confirmButton->click();
        });
        timer.start(10);
    };

    bool uploadConflictDialogObserved = false;
    QTimer uploadConflictDialogTimer;
    window.setDialogsSuppressedForTesting(false);
    armOverwriteDialog(uploadConflictDialogTimer, uploadConflictDialogObserved);
    window.uploadLocalFileForTesting(QString::fromStdString(localConflictUpload.u8string()));
    const bool uploadOverwriteCompleted = waitForRemoteFileSize(
            fileSystem,
            remotePath,
            remoteConflictFile,
            static_cast<std::int64_t>(std::filesystem::file_size(localConflictUpload)));
    uploadConflictDialogTimer.stop();
    window.setDialogsSuppressedForTesting(true);
    if (!uploadConflictDialogObserved || !uploadOverwriteCompleted)
    {
        QTextStream(stderr) << "File upload conflict did not show the new dialog and safely replace the remote target" << Qt::endl;
        return false;
    }

    {
        std::ofstream output(existingLocalDownload, std::ios::binary | std::ios::trunc);
        output << "replace this local target\n";
    }
    bool downloadConflictDialogObserved = false;
    QTimer downloadConflictDialogTimer;
    window.setDialogsSuppressedForTesting(false);
    armOverwriteDialog(downloadConflictDialogTimer, downloadConflictDialogObserved);
    window.downloadRemoteFileForTesting(remoteConflictFile);
    const auto overwrittenDownloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    const QString existingLocalDownloadText = QDir::fromNativeSeparators(
        QString::fromStdString(existingLocalDownload.u8string()));
    bool downloadOverwriteCompleted = false;
    while (std::chrono::steady_clock::now() < overwrittenDownloadDeadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        for (int index = 0; index < transferTable->topLevelItemCount(); ++index)
        {
            QTreeWidgetItem *item = transferTable->topLevelItem(index);
            if (item != nullptr
                && item->text(0) == "file-conflict.txt"
                && QDir::fromNativeSeparators(item->text(4)) == existingLocalDownloadText
                && item->text(5) == "<-"
                && item->text(1) == "已完成")
            {
                downloadOverwriteCompleted = true;
                break;
            }
        }
        if (downloadOverwriteCompleted)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::ifstream overwrittenDownload(existingLocalDownload, std::ios::binary);
    const std::string overwrittenDownloadText{
        std::istreambuf_iterator<char>(overwrittenDownload),
        std::istreambuf_iterator<char>()};
    downloadConflictDialogTimer.stop();
    window.setDialogsSuppressedForTesting(true);
    if (!downloadConflictDialogObserved || !downloadOverwriteCompleted
        || overwrittenDownloadText.rfind("fake remote file:", 0) != 0)
    {
        QTextStream(stderr)
            << "File download conflict did not show the new dialog and safely replace the local target"
            << " (dialog=" << downloadConflictDialogObserved
            << ", completed=" << downloadOverwriteCompleted
            << ", bytes=" << overwrittenDownloadText.size()
            << ", content=" << QString::fromStdString(overwrittenDownloadText.substr(0, 64)) << ')'
            << Qt::endl;
        dumpTransferRows(transferTable);
        return false;
    }

    const QString uploadedDirectory = remotePath + "/dirbridge-folder";
    int directoryUploadLoadingCount = 0;
    QLabel *remoteStateLabel = remoteTabs->currentWidget() == nullptr
        ? nullptr
        : remoteTabs->currentWidget()->findChild<QLabel *>("remoteStateLabel");
    bool directoryUploadWasLoading = false;
    QTimer directoryUploadLoadingMonitor;
    if (remoteStateLabel != nullptr)
    {
        QObject::connect(&directoryUploadLoadingMonitor, &QTimer::timeout, &directoryUploadLoadingMonitor, [
            remoteStateLabel,
            &directoryUploadLoadingCount,
            &directoryUploadWasLoading]() {
            const bool loading = remoteStateLabel->text().contains("正在加载");
            if (loading && !directoryUploadWasLoading)
            {
                ++directoryUploadLoadingCount;
            }
            directoryUploadWasLoading = loading;
        });
        directoryUploadLoadingMonitor.start(5);
    }
    window.uploadLocalPathForTesting(QString::fromStdString(localDirectory.u8string()));
    if (!waitForTransferRow(transferTable, "dirbridge-folder", "上传", "已完成"))
    {
        QTextStream(stderr) << "Directory upload did not complete before conflict retry" << Qt::endl;
        dumpTransferRows(transferTable);
        return false;
    }
    if (!waitForRemoteConnected(remoteTabs->currentWidget(), remotePath)
        || directoryUploadLoadingCount > 1)
    {
        directoryUploadLoadingMonitor.stop();
        QTextStream(stderr) << "Directory upload should refresh the remote panel only once after all files finish" << Qt::endl;
        return false;
    }
    directoryUploadLoadingMonitor.stop();

    const QString directoryTargetOnlyFile = uploadedDirectory + "/target-only-before-replacement.txt";
    result = fileSystem->createFile(directoryTargetOnlyFile.toStdString());
    if (!result.success)
    {
        QTextStream(stderr) << "Unable to prepare directory replacement target-only file" << Qt::endl;
        return false;
    }
    bool directoryConflictDialogObserved = false;
    QTimer directoryConflictDialogTimer;
    window.setDialogsSuppressedForTesting(false);
    armOverwriteDialog(directoryConflictDialogTimer, directoryConflictDialogObserved);
    window.uploadLocalPathForTesting(QString::fromStdString(localDirectory.u8string()));
    const bool directoryOverwriteCompleted = waitForRemoteDirectoryReplacement(
        fileSystem,
        remotePath,
        uploadedDirectory,
        uploadedDirectory + "/nested",
        directoryTargetOnlyFile);
    directoryConflictDialogTimer.stop();
    window.setDialogsSuppressedForTesting(true);
    if (!directoryConflictDialogObserved || !directoryOverwriteCompleted)
    {
        QTextStream(stderr) << "Directory upload conflict did not show overwrite or safely replace the remote directory" << Qt::endl;
        return false;
    }
    QTreeWidgetItem *replacementUploadRow = findLatestTopLevelTransferRowWithChildren(
        transferTable,
        "dirbridge-folder",
        "上传",
        "已完成",
        2);
    if (replacementUploadRow == nullptr
        || replacementUploadRow->child(0)->text(1) != "已完成"
        || replacementUploadRow->child(1)->text(1) != "已完成")
    {
        QTextStream(stderr) << "Directory replacement upload should expose completed per-file child rows" << Qt::endl;
        dumpTransferRows(transferTable);
        return false;
    }

    const QString renamedUploadedDirectory = remotePath + "/dirbridge-folder (1)";
    window.uploadLocalPathForTesting(QString::fromStdString(localDirectory.u8string()));
    if (!waitForRemotePath(fileSystem, remotePath, renamedUploadedDirectory, FileItemType::Directory)
        || !waitForRemotePath(
            fileSystem,
            renamedUploadedDirectory + "/nested",
            renamedUploadedDirectory + "/nested/inside.txt",
            FileItemType::File))
    {
        QTextStream(stderr) << "Directory upload conflict did not use a renamed remote root" << Qt::endl;
        return false;
    }
    QTreeWidgetItem *selectedUploadParent = findTopLevelTransferRow(transferTable, "dirbridge-folder", "上传", "已完成");
    if (selectedUploadParent == nullptr)
    {
        QTextStream(stderr) << "Directory upload parent row is missing before refresh-state check" << Qt::endl;
        return false;
    }
    transferTable->setCurrentItem(selectedUploadParent);
    selectedUploadParent->setSelected(true);
    const QString selectedUploadId = selectedUploadParent->data(0, Qt::UserRole).toString();
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
    if (transferTable->currentItem() == nullptr
        || transferTable->currentItem()->data(0, Qt::UserRole).toString() != selectedUploadId)
    {
        QTextStream(stderr) << "Transfer table refresh did not preserve the selected job" << Qt::endl;
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

    const std::filesystem::path localDirectoryTargetOnlyFile = downloadRoot
        / "dirbridge-folder"
        / "target-only-before-replacement.txt";
    {
        std::ofstream output(localDirectoryTargetOnlyFile, std::ios::binary | std::ios::trunc);
        output << "remove after safe directory download replacement\n";
    }
    bool directoryDownloadConflictDialogObserved = false;
    QTimer directoryDownloadConflictDialogTimer;
    window.setDialogsSuppressedForTesting(false);
    armOverwriteDialog(directoryDownloadConflictDialogTimer, directoryDownloadConflictDialogObserved);
    window.downloadRemotePathForTesting(movedDirectory);
    const bool directoryDownloadOverwriteCompleted = waitForLocalDirectoryReplacement(
        downloadRoot,
        downloadRoot / "dirbridge-folder",
        downloadedDeepFile,
        localDirectoryTargetOnlyFile);
    directoryDownloadConflictDialogTimer.stop();
    window.setDialogsSuppressedForTesting(true);
    if (!directoryDownloadConflictDialogObserved || !directoryDownloadOverwriteCompleted)
    {
        QTextStream(stderr) << "Directory download conflict did not show overwrite or safely replace the local directory" << Qt::endl;
        return false;
    }

    window.downloadRemotePathForTesting(movedDirectory);
    const std::filesystem::path renamedDownloadedFile = downloadRoot / "dirbridge-folder (1)" / "nested" / "inside.txt";
    const auto renamedDownloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!std::filesystem::is_regular_file(renamedDownloadedFile)
        && std::chrono::steady_clock::now() < renamedDownloadDeadline)
    {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!std::filesystem::is_regular_file(renamedDownloadedFile))
    {
        QTextStream(stderr) << "Directory download conflict did not use a renamed local root" << Qt::endl;
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
    return checkFileCreateRenameWorkflow()
        && checkFileTreeDropWorkflow()
        && checkRemoteDirectoryOperationWorkflow(window)
        && checkQuickSaveCreatesSeparateSite(window)
        && checkRemoteConnectionControlWorkflow(window)
        && checkRemoteNavigationResponsiveness()
        && checkPersistedFileTreeVisibilityAfterRestart()
        && checkLocalPanelStateWorkflow()
        && checkSessionManagerWorkflow(window)
        && checkRemoteMultiSessionWorkflow(window)
        && baseWorkflowOk
        && checkWindowCloseDuringUploadPreparation();
}

/**
 * @brief 针对内存假后端运行剪贴板与资源管理器拖放工作流。
 * @param window 待测试的主窗口。
 * @return 双向复制粘贴及拖放检查通过时返回 true。
 */
bool checkClipboardDragWorkflow(MainWindow &window)
{
    return checkSingleDragStartPerMousePress()
        && checkIncrementalLocalDirectoryRefresh()
        && checkRemoteUiWorkflow(
        window,
        "SFTP",
        "fake-host",
        "22",
        "testuser",
        "",
        "/home/testuser/remote_test",
        {"download", "upload", "edit", "readme.txt"},
        true);
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
