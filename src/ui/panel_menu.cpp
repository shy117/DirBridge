#include "ui/FilePanel.h"
#include "ui/panel_shared.h"

#include <QAbstractItemDelegate>
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIODevice>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <array>

using namespace panel_shared;

namespace
{
int permissionModeFromText(const QString &text, int fallbackMode)
{
    const QString bits = text.right(9);
    if (bits.size() != 9)
    {
        return fallbackMode;
    }

    int mode = 0;
    constexpr int masks[] = {0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001};
    constexpr QChar symbols[] = {'r', 'w', 'x', 'r', 'w', 'x', 'r', 'w', 'x'};
    for (int index = 0; index < 9; ++index)
    {
        if (bits.at(index) == symbols[index])
        {
            mode |= masks[index];
        }
        else if (bits.at(index) != '-')
        {
            return fallbackMode;
        }
    }
    return mode;
}

QString octalPermissionText(int mode)
{
    return QString("%1").arg(mode, 3, 8, QChar('0'));
}
} // namespace

/**
 * @brief 显示本地面板右键菜单。
 * @param position 表格视口中的菜单触发位置。
 */
void FilePanel::showLocalContextMenu(const QPoint &position)
{
    showUnifiedContextMenu(position);
}

/**
 * @brief 显示远程面板右键菜单。
 * @param position 表格视口中的菜单触发位置。
 */
void FilePanel::showRemoteContextMenu(const QPoint &position)
{
    showUnifiedContextMenu(position);
}

/**
 * @brief 根据当前模式构建并处理统一文件操作菜单。
 * @param position 表格视口中的菜单触发位置。
 */
void FilePanel::showUnifiedContextMenu(const QPoint &position)
{
    if (m_currentPath.isEmpty())
    {
        return;
    }

    if (QTableWidgetItem *item = m_table->itemAt(position); item != nullptr)
    {
        m_table->selectRow(item->row());
    }
    else
    {
        m_table->clearSelection();
        m_table->setCurrentItem(nullptr);
    }

    const QString selectedPath = selectedEntryPath();
    const bool hasSelection = !selectedPath.isEmpty();
    const bool selectedIsDirectory = hasSelection
        && m_table->currentItem() != nullptr
        && m_table->item(m_table->currentRow(), 0) != nullptr
        && m_table->item(m_table->currentRow(), 0)->data(Qt::UserRole + 1).toBool();
    const bool isLocal = m_mode == Mode::Local;

    QMenu menu(this);
    QAction *openAction = nullptr;
    QAction *enterAction = nullptr;
    QAction *uploadAction = nullptr;
    QAction *downloadAction = nullptr;
    QAction *editAction = nullptr;
    QAction *closeEditAction = nullptr;
    QAction *renameAction = nullptr;
    QAction *permissionsAction = nullptr;
    QAction *removeAction = nullptr;
    QAction *copyPathAction = nullptr;
    QAction *propertiesAction = nullptr;
    QAction *copyFolderPathAction = nullptr;
    QAction *newDirectoryAction = nullptr;
    QAction *newFileAction = nullptr;
    QAction *fileTreeAction = nullptr;

    QMenu *viewMenu = menu.addMenu(fluentIcon("more_horizontal"), "查看");
    fileTreeAction = viewMenu->addAction(fluentIcon("folder_add"), "文件树");
    fileTreeAction->setCheckable(true);
    fileTreeAction->setChecked(isFileTreeVisible());

    if (hasSelection)
    {
        if (isLocal)
        {
            openAction = menu.addAction(fluentIcon("chevron_right"), "打开");
        }
        else if (selectedIsDirectory)
        {
            enterAction = menu.addAction(fluentIcon("chevron_right"), "进入");
        }
        if (isLocal)
        {
            uploadAction = menu.addAction(fluentIcon("arrow_right"), "上传");
            uploadAction->setEnabled(m_localUploadRequested != nullptr);
        }
        else
        {
            downloadAction = menu.addAction(fluentIcon("arrow_left"), "下载");
            downloadAction->setEnabled(m_remoteDownloadRequested != nullptr);
            if (!selectedIsDirectory)
            {
                editAction = menu.addAction(fluentIcon("edit"), "编辑");
                editAction->setEnabled(m_remoteEditRequested != nullptr);
                if (m_remoteEditActiveQuery && m_remoteEditActiveQuery(selectedPath))
                {
                    closeEditAction = menu.addAction(fluentIcon("dismiss_circle"), "关闭编辑");
                    closeEditAction->setEnabled(m_remoteEditCloseRequested != nullptr);
                }
            }
        }
        renameAction = menu.addAction(fluentIcon("edit"), "重命名");
        if (!isLocal)
        {
            permissionsAction = menu.addAction(fluentIcon("lock_closed"), "更改权限...");
            permissionsAction->setEnabled(m_remotePermissionsRequested != nullptr);
        }
        removeAction = menu.addAction(fluentIcon("delete"), "删除");
        menu.addSeparator();
        copyPathAction = menu.addAction(fluentIcon("copy"), "复制路径");
        copyFolderPathAction = menu.addAction(fluentIcon("copy"), "复制文件夹路径");
    }
    else
    {
        copyFolderPathAction = menu.addAction(fluentIcon("copy"), "复制文件夹路径");
    }

    menu.addSeparator();
    QMenu *newMenu = menu.addMenu(fluentIcon("add"), "新建");
    newDirectoryAction = newMenu->addAction(fluentIcon("folder_add"), "新建文件夹");
    newFileAction = newMenu->addAction(fluentIcon("add"), "新建文件");

    propertiesAction = menu.addAction(fluentIcon("info"), "属性");

    QAction *selectedAction = menu.exec(m_table->viewport()->mapToGlobal(position));
    if (selectedAction == nullptr)
    {
        return;
    }

    if (selectedAction == fileTreeAction)
    {
        const bool visible = fileTreeAction->isChecked();
        if (m_fileTreeVisibilityRequested)
        {
            m_fileTreeVisibilityRequested(visible);
        }
        else
        {
            setFileTreeVisible(visible);
        }
        return;
    }

    if (selectedAction == enterAction)
    {
        requestRemotePath(selectedPath, true);
        return;
    }

    if (selectedAction == openAction)
    {
        const QFileInfo info(selectedPath);
        if (info.isDir())
        {
            navigateTo(selectedPath);
        }
        else
        {
            QDesktopServices::openUrl(QUrl::fromLocalFile(selectedPath));
        }
        return;
    }

    if (selectedAction == copyFolderPathAction)
    {
        QApplication::clipboard()->setText(isLocal ? QDir::toNativeSeparators(m_currentPath) : m_currentPath);
        return;
    }

    if (selectedAction == copyPathAction)
    {
        QApplication::clipboard()->setText(isLocal ? QDir::toNativeSeparators(selectedPath) : selectedPath);
        return;
    }

    if (selectedAction == propertiesAction)
    {
        if (isLocal)
        {
            showLocalProperties(hasSelection ? selectedPath : m_currentPath);
        }
        else
        {
            showRemoteProperties(hasSelection ? selectedPath : m_currentPath);
        }
        return;
    }

    if (selectedAction == newDirectoryAction)
    {
        if (isLocal)
        {
            createLocalDirectory();
            return;
        }

        const QString name = nextAvailableName("新建文件夹");
        if (m_remoteCreateDirectoryRequested)
        {
            m_remoteCreateDirectoryRequested(remoteChildPath(name));
        }
        return;
    }

    if (selectedAction == newFileAction)
    {
        if (isLocal)
        {
            createLocalFile();
            return;
        }

        const QString name = nextAvailableName("新建文件");
        if (m_remoteCreateFileRequested)
        {
            m_remoteCreateFileRequested(remoteChildPath(name));
        }
        return;
    }

    if (selectedAction == uploadAction && m_localUploadRequested)
    {
        m_localUploadRequested(selectedPath);
        return;
    }

    if (selectedAction == downloadAction && m_remoteDownloadRequested)
    {
        m_remoteDownloadRequested(selectedPath, selectedIsDirectory);
        return;
    }

    if (selectedAction == editAction && m_remoteEditRequested)
    {
        m_remoteEditRequested(selectedPath);
        return;
    }

    if (selectedAction == closeEditAction && m_remoteEditCloseRequested)
    {
        m_remoteEditCloseRequested(selectedPath);
        return;
    }

    if (selectedAction == removeAction)
    {
        if (isLocal)
        {
            removeLocalPath(selectedPath);
            return;
        }

        if (!m_remoteRemoveRequested)
        {
            return;
        }
        const QString text = QString("确定删除远程项目？\n%1").arg(selectedPath);
        if (QMessageBox::question(this, "删除远程项目", text) == QMessageBox::Yes)
        {
            m_remoteRemoveRequested(selectedPath, selectedIsDirectory);
        }
        return;
    }

    if (selectedAction == permissionsAction)
    {
        showRemotePermissionsDialog(selectedPath, selectedIsDirectory);
        return;
    }

    if (selectedAction == renameAction)
    {
        renameSelectedEntry();
    }
}

/**
 * @brief 对文件表格当前选中的本地或远程项目执行重命名。
 */
void FilePanel::renameSelectedEntry()
{
    const QString selectedPath = selectedEntryPath();
    if (selectedPath.isEmpty())
    {
        return;
    }

    if (m_mode == Mode::Local)
    {
        renameLocalPath(selectedPath);
        return;
    }

    if (!m_remoteRenameRequested)
    {
        return;
    }

    beginInlineRenameForPath(selectedPath);
}

/**
 * @brief 显示远程项目属性对话框。
 * @param path 远程项目路径。
 */
void FilePanel::showRemoteProperties(const QString &path) const
{
    QStringList lines;
    lines << QString("路径：%1").arg(path);

    for (int row = 0; row < m_table->rowCount(); ++row)
    {
        QTableWidgetItem *nameItem = m_table->item(row, 0);
        if (nameItem == nullptr || nameItem->data(Qt::UserRole).toString() != path)
        {
            continue;
        }

        lines << QString("名称：%1").arg(nameItem->text());
        lines << QString("大小：%1").arg(m_table->item(row, 1) == nullptr ? QString() : m_table->item(row, 1)->text());
        lines << QString("类型：%1").arg(m_table->item(row, 2) == nullptr ? QString() : m_table->item(row, 2)->text());
        lines << QString("修改时间：%1").arg(m_table->item(row, 3) == nullptr ? QString() : m_table->item(row, 3)->text());
        lines << QString("权限：%1").arg(m_table->item(row, 4) == nullptr ? QString() : m_table->item(row, 4)->text());
        lines << QString("所有者：%1").arg(m_table->item(row, 5) == nullptr ? QString() : m_table->item(row, 5)->text());
        break;
    }

    showInformationDialog(const_cast<FilePanel *>(this), "远程属性", lines.join('\n'));
}

/**
 * @brief 显示 UNIX 风格远程权限编辑器。
 * @param path 要修改的远程项目路径。
 * @param isDirectory 项目是否为目录。
 */
void FilePanel::showRemotePermissionsDialog(const QString &path, bool isDirectory)
{
    if (!m_remotePermissionsRequested)
    {
        return;
    }

    QString listedPermissions;
    for (int row = 0; row < m_table->rowCount(); ++row)
    {
        QTableWidgetItem *nameItem = m_table->item(row, 0);
        if (nameItem != nullptr && nameItem->data(Qt::UserRole).toString() == path)
        {
            QTableWidgetItem *permissionItem = m_table->item(row, 4);
            listedPermissions = permissionItem == nullptr ? QString() : permissionItem->text();
            break;
        }
    }

    QDialog dialog(this);
    dialog.setWindowTitle("更改权限");
    dialog.setModal(true);
    auto *layout = new QVBoxLayout(&dialog);

    auto *modeRow = new QHBoxLayout();
    modeRow->addWidget(new QLabel("文件/文件夹 权限(P):", &dialog));
    auto *modeEdit = new QLineEdit(&dialog);
    modeEdit->setObjectName("remotePermissionModeEdit");
    modeEdit->setFixedWidth(56);
    modeEdit->setMaxLength(3);
    modeEdit->setAlignment(Qt::AlignCenter);
    modeEdit->setValidator(new QRegularExpressionValidator(QRegularExpression("[0-7]{3}"), modeEdit));
    modeRow->addWidget(modeEdit);
    modeRow->addStretch(1);
    layout->addLayout(modeRow);

    auto *groupsRow = new QHBoxLayout();
    std::array<std::array<QCheckBox *, 3>, 3> checks{};
    const QStringList groupNames = {"所有者", "组", "其他"};
    const QStringList permissionNames = {"读取", "写入", "执行"};
    for (int groupIndex = 0; groupIndex < 3; ++groupIndex)
    {
        auto *groupBox = new QGroupBox(groupNames.at(groupIndex), &dialog);
        auto *groupLayout = new QVBoxLayout(groupBox);
        for (int permissionIndex = 0; permissionIndex < 3; ++permissionIndex)
        {
            checks[groupIndex][permissionIndex] = new QCheckBox(permissionNames.at(permissionIndex), groupBox);
            groupLayout->addWidget(checks[groupIndex][permissionIndex]);
        }
        groupsRow->addWidget(groupBox);
    }
    layout->addLayout(groupsRow);

    auto updateChecks = [&checks](int mode) {
        constexpr int masks[3][3] = {
            {0400, 0200, 0100},
            {0040, 0020, 0010},
            {0004, 0002, 0001}
        };
        for (int groupIndex = 0; groupIndex < 3; ++groupIndex)
        {
            for (int permissionIndex = 0; permissionIndex < 3; ++permissionIndex)
            {
                QSignalBlocker blocker(checks[groupIndex][permissionIndex]);
                checks[groupIndex][permissionIndex]->setChecked((mode & masks[groupIndex][permissionIndex]) != 0);
            }
        }
    };
    auto updateMode = [&checks, modeEdit]() {
        constexpr int masks[3][3] = {
            {0400, 0200, 0100},
            {0040, 0020, 0010},
            {0004, 0002, 0001}
        };
        int mode = 0;
        for (int groupIndex = 0; groupIndex < 3; ++groupIndex)
        {
            for (int permissionIndex = 0; permissionIndex < 3; ++permissionIndex)
            {
                if (checks[groupIndex][permissionIndex]->isChecked())
                {
                    mode |= masks[groupIndex][permissionIndex];
                }
            }
        }
        QSignalBlocker blocker(modeEdit);
        modeEdit->setText(octalPermissionText(mode));
    };

    const int initialMode = permissionModeFromText(listedPermissions, isDirectory ? 0755 : 0644);
    modeEdit->setText(octalPermissionText(initialMode));
    updateChecks(initialMode);
    connect(modeEdit, &QLineEdit::textChanged, &dialog, [modeEdit, updateChecks](const QString &text) {
        bool ok = false;
        const int mode = text.toInt(&ok, 8);
        if (ok && text.size() == 3)
        {
            updateChecks(mode);
        }
    });
    for (const auto &group : checks)
    {
        for (QCheckBox *check : group)
        {
            connect(check, &QCheckBox::toggled, &dialog, [updateMode](bool) {
                updateMode();
            });
        }
    }

    auto *recursiveCheck = new QCheckBox("包含子目录(&I)", &dialog);
    recursiveCheck->setObjectName("remotePermissionRecursiveCheck");
    recursiveCheck->setEnabled(isDirectory);
    layout->addWidget(recursiveCheck);
    auto *hintLabel = new QLabel("此命令仅在部分 UNIX 主机中适用。", &dialog);
    layout->addWidget(hintLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    bool ok = false;
    const int mode = modeEdit->text().toInt(&ok, 8);
    if (!ok || modeEdit->text().size() != 3 || mode < 0 || mode > 0777)
    {
        showFileOperationWarning(this, "更改权限", "请输入 000 到 777 之间的三位八进制权限值。");
        return;
    }
    m_remotePermissionsRequested(path, mode, isDirectory && recursiveCheck->isChecked());
}

/**
 * @brief 在当前本地目录中新建文件夹。
 */
void FilePanel::createLocalDirectory()
{
    const QString name = nextAvailableName("新建文件夹");
    const QString path = QDir(m_currentPath).filePath(name);
    if (!QDir().mkdir(path))
    {
        showFileOperationWarning(this, "新建本地文件夹失败", QString("无法创建文件夹：%1").arg(path));
        return;
    }
    refresh();
    for (int row = 0; row < m_table->rowCount(); ++row)
    {
        QTableWidgetItem *nameItem = m_table->item(row, 0);
        if (nameItem != nullptr && nameItem->data(Qt::UserRole).toString() == path)
        {
            m_table->selectRow(row);
            break;
        }
    }
    beginInlineRenameForPath(path);
}

/**
 * @brief 在当前本地目录中新建空文件。
 */
void FilePanel::createLocalFile()
{
    const QString name = nextAvailableName("新建文件");
    const QString path = QDir(m_currentPath).filePath(name);
    if (QFileInfo::exists(path))
    {
        showFileOperationWarning(this, "新建本地文件失败", QString("文件或文件夹已存在：%1").arg(path));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
    {
        showFileOperationWarning(this, "新建本地文件失败", QString("无法创建文件：%1").arg(path));
        return;
    }
    file.close();
    refresh();
    for (int row = 0; row < m_table->rowCount(); ++row)
    {
        QTableWidgetItem *nameItem = m_table->item(row, 0);
        if (nameItem != nullptr && nameItem->data(Qt::UserRole).toString() == path)
        {
            m_table->selectRow(row);
            break;
        }
    }
    beginInlineRenameForPath(path);
}

/**
 * @brief 异步将指定本地项目移入系统回收站。
 * @param path 要移入回收站的本地文件或目录。
 */
void FilePanel::removeLocalPath(const QString &path)
{
    if (m_pendingLocalDeletes.contains(path))
    {
        showFileOperationWarning(this, "移入回收站", "该项目正在处理，请等待完成。");
        return;
    }

    const QFileInfo info(path);
    if (!info.exists())
    {
        showFileOperationWarning(this, "移入回收站失败", QString("项目不存在：%1").arg(path));
        return;
    }

    const QString text = QString("确定将本地项目移入回收站？\n%1").arg(path);
    if (QMessageBox::question(this, "移入回收站", text) != QMessageBox::Yes)
    {
        return;
    }

    m_pendingLocalDeletes.insert(path);
    QPointer<FilePanel> panel(this);
    QThread *thread = QThread::create([panel, path]() {
        bool removed = false;
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        removed = QFile::moveToTrash(path);
#endif
        if (panel != nullptr)
        {
            QMetaObject::invokeMethod(panel.data(), [panel, path, removed]() {
                if (panel != nullptr)
                {
                    panel->finishLocalRemove(path, removed);
                }
            }, Qt::QueuedConnection);
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

/**
 * @brief 处理本地项目移入回收站后的 UI 更新。
 * @param path 已尝试移动的路径。
 * @param removed 是否成功移入回收站。
 */
void FilePanel::finishLocalRemove(const QString &path, bool removed)
{
    m_pendingLocalDeletes.remove(path);
    if (!removed)
    {
        showFileOperationWarning(this, "移入回收站失败", QString("无法将项目移入回收站：%1").arg(path));
        return;
    }
    refresh();
}

/**
 * @brief 重命名指定本地项目。
 * @param path 要重命名的本地文件或目录。
 */
void FilePanel::renameLocalPath(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
    {
        showFileOperationWarning(this, "重命名本地项目失败", QString("项目不存在：%1").arg(path));
        return;
    }
    beginInlineRenameForPath(path);
}

/**
 * @brief 为当前目录中的新建项目选择不冲突的默认名称。
 * @param baseName 默认名称。
 * @return 不与现有文件或目录冲突的名称。
 */
QString FilePanel::nextAvailableName(const QString &baseName) const
{
    auto nameExists = [this](const QString &name) {
        if (m_mode == Mode::Local)
        {
            return QFileInfo::exists(QDir(m_currentPath).filePath(name));
        }

        const QString foldedName = name.toCaseFolded();
        for (const FileItem &item : m_remoteItems)
        {
            if (QString::fromStdString(item.name).toCaseFolded() == foldedName)
            {
                return true;
            }
        }
        return false;
    };

    QString candidate = baseName;
    int suffix = 2;
    while (nameExists(candidate))
    {
        candidate = QString("%1 (%2)").arg(baseName).arg(suffix++);
    }
    return candidate;
}

/**
 * @brief 在表格名称列上创建内联重命名编辑器。
 * @param path 要重命名的项目路径。
 */
void FilePanel::beginInlineRenameForPath(const QString &path)
{
    if (path.isEmpty() || m_table == nullptr)
    {
        return;
    }

    if (m_inlineRenameEditor != nullptr)
    {
        cancelInlineRename();
    }

    int row = -1;
    QTableWidgetItem *nameItem = nullptr;
    for (int index = 0; index < m_table->rowCount(); ++index)
    {
        QTableWidgetItem *candidate = m_table->item(index, 0);
        if (candidate != nullptr && candidate->data(Qt::UserRole).toString() == path)
        {
            row = index;
            nameItem = candidate;
            break;
        }
    }
    if (row < 0 || nameItem == nullptr)
    {
        return;
    }

    const QString originalName = nameItem->text();
    if (originalName.isEmpty())
    {
        return;
    }

    m_table->scrollToItem(nameItem, QAbstractItemView::EnsureVisible);
    m_table->selectRow(row);
    const QRect itemRect = m_table->visualItemRect(nameItem);
    if (!itemRect.isValid())
    {
        return;
    }

    m_inlineRenameItem = nameItem;
    m_inlineRenamePath = path;
    m_inlineRenameOriginalName = originalName;
    m_inlineRenameCommitRequested = false;
    m_inlineRenameFinishing = false;
    m_table->editItem(nameItem);

    auto *editor = m_table->viewport()->findChild<QLineEdit *>(QString(), Qt::FindDirectChildrenOnly);
    if (editor == nullptr)
    {
        m_inlineRenameItem = nullptr;
        m_inlineRenamePath.clear();
        m_inlineRenameOriginalName.clear();
        return;
    }
    editor->setObjectName("inlineRenameEditor");
    const bool isDirectory = nameItem->data(Qt::UserRole + 1).toBool();
    if (isDirectory)
    {
        editor->selectAll();
    }
    else
    {
        const QString baseName = QFileInfo(originalName).completeBaseName();
        editor->setSelection(0, baseName.isEmpty() ? originalName.size() : baseName.size());
    }
    m_inlineRenameEditor = editor;
}

/**
 * @brief 提交或取消当前内联重命名。
 * @param accepted 是否提交编辑内容。
 */
void FilePanel::finishInlineRename(bool accepted)
{
    if (m_inlineRenameItem == nullptr || m_inlineRenameFinishing)
    {
        return;
    }

    m_inlineRenameFinishing = true;
    QTableWidgetItem *nameItem = m_inlineRenameItem;
    const QString sourcePath = m_inlineRenamePath;
    const QString originalName = m_inlineRenameOriginalName;
    const QString newName = accepted ? nameItem->text().trimmed() : originalName;
    {
        const QSignalBlocker blocker(m_table);
        nameItem->setText(originalName);
    }
    m_inlineRenameEditor = nullptr;
    m_inlineRenameItem = nullptr;
    m_inlineRenamePath.clear();
    m_inlineRenameOriginalName.clear();
    m_inlineRenameCommitRequested = false;
    m_inlineRenameFinishing = false;

    if (!accepted || newName.isEmpty() || newName == originalName)
    {
        if (accepted && newName.isEmpty())
        {
            showInvalidRemoteNameWarning(this);
        }
        return;
    }
    if (!isValidRemoteName(newName))
    {
        showInvalidRemoteNameWarning(this);
        return;
    }

    if (m_mode == Mode::Local)
    {
        const QString targetPath = QDir(QFileInfo(sourcePath).absolutePath()).filePath(newName);
        if (QFileInfo::exists(targetPath))
        {
            showFileOperationWarning(this, "重命名本地项目失败", QString("目标已存在：%1").arg(targetPath));
            return;
        }
        if (!QFile::rename(sourcePath, targetPath))
        {
            showFileOperationWarning(this, "重命名本地项目失败", QString("无法重命名：%1").arg(sourcePath));
            return;
        }
        refresh();
        for (int row = 0; row < m_table->rowCount(); ++row)
        {
            QTableWidgetItem *nameItem = m_table->item(row, 0);
            if (nameItem != nullptr && nameItem->data(Qt::UserRole).toString() == targetPath)
            {
                m_table->selectRow(row);
                break;
            }
        }
        return;
    }

    const QString sourceName = QFileInfo(sourcePath).fileName();
    const QString foldedName = newName.toCaseFolded();
    for (const FileItem &item : m_remoteItems)
    {
        const QString itemPath = QString::fromStdString(item.path);
        if (itemPath != sourcePath && QString::fromStdString(item.name).toCaseFolded() == foldedName)
        {
            showFileOperationWarning(this, "重命名远程项目失败", "目标项目已存在，请使用其他名称。");
            return;
        }
    }
    if (m_remoteRenameRequested && !sourceName.isEmpty())
    {
        m_remoteRenameRequested(sourcePath, remoteSiblingPath(sourcePath, newName));
    }
}

/**
 * @brief 取消当前内联重命名并保留原名称。
 */
void FilePanel::cancelInlineRename()
{
    if (m_inlineRenameEditor == nullptr || m_table == nullptr)
    {
        finishInlineRename(false);
        return;
    }

    QWidget *editor = m_inlineRenameEditor;
    m_table->itemDelegate()->closeEditor(editor, QAbstractItemDelegate::RevertModelCache);
    if (m_inlineRenameEditor == editor)
    {
        finishInlineRename(false);
    }
}

/**
 * @brief 显示本地项目属性对话框。
 * @param path 本地文件或目录路径。
 */
void FilePanel::showLocalProperties(const QString &path) const
{
    const QFileInfo info(path);
    QStringList lines;
    lines << QString("路径：%1").arg(path);
    lines << QString("名称：%1").arg(info.fileName().isEmpty() ? info.absoluteFilePath() : info.fileName());
    lines << QString("类型：%1").arg(info.isDir() ? "文件夹" : "文件");
    lines << QString("大小：%1").arg(info.isDir() ? QString() : formatFileSize(info.size()));
    lines << QString("修改时间：%1").arg(info.lastModified().toString("yyyy-MM-dd HH:mm:ss"));
    lines << QString("权限：%1").arg(info.permission(QFile::WriteUser) ? "可写" : "只读");
    lines << QString("所有者：%1").arg(info.owner());
    showInformationDialog(const_cast<FilePanel *>(this), "本地属性", lines.join('\n'));
}

/**
 * @brief 基于当前远程目录拼接子项目路径。
 * @param name 子项目名称。
 * @return 远程子项目完整路径。
 */
QString FilePanel::remoteChildPath(const QString &name) const
{
    QString path = m_currentPath.isEmpty() ? "/" : m_currentPath;
    if (!path.endsWith('/'))
    {
        path.append('/');
    }
    return path + name;
}

/**
 * @brief 基于源远程路径和新名称拼接同级目标路径。
 * @param sourcePath 源远程路径。
 * @param newName 新名称。
 * @return 重命名后的远程目标路径。
 */
QString FilePanel::remoteSiblingPath(const QString &sourcePath, const QString &newName) const
{
    QString path = sourcePath;
    if (path.endsWith('/'))
    {
        path.chop(1);
    }
    const int slashIndex = path.lastIndexOf('/');
    return slashIndex <= 0 ? "/" + newName : path.left(slashIndex + 1) + newName;
}

/**
 * @brief 读取当前表格选中行对应的项目路径。
 * @return 选中项目路径；没有选择时为空。
 */
QString FilePanel::selectedEntryPath() const
{
    const QList<QTableWidgetItem *> items = m_table->selectedItems();
    if (items.isEmpty())
    {
        return QString();
    }

    QTableWidgetItem *nameItem = m_table->item(items.first()->row(), 0);
    if (nameItem == nullptr)
    {
        return QString();
    }

    return nameItem->data(Qt::UserRole).toString();
}
