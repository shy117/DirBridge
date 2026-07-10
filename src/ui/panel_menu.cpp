#include "ui/FilePanel.h"
#include "ui/panel_shared.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QIODevice>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QUrl>

using namespace panel_shared;
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
    QAction *renameAction = nullptr;
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
        }
        renameAction = menu.addAction(fluentIcon("edit"), "重命名");
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
        setFileTreeVisible(fileTreeAction->isChecked());
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

        bool ok = false;
        const QString name = QInputDialog::getText(this, "新建远程文件夹", "文件夹名称：", QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok)
        {
            return;
        }
        if (!isValidRemoteName(name))
        {
            showInvalidRemoteNameWarning(this);
            return;
        }
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

        bool ok = false;
        const QString name = QInputDialog::getText(this, "新建远程文件", "文件名称：", QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok)
        {
            return;
        }
        if (!isValidRemoteName(name))
        {
            showInvalidRemoteNameWarning(this);
            return;
        }
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
            m_remoteRemoveRequested(selectedPath);
        }
        return;
    }

    if (selectedAction == renameAction)
    {
        if (isLocal)
        {
            renameLocalPath(selectedPath);
            return;
        }

        if (!m_remoteRenameRequested)
        {
            return;
        }
        const QFileInfo info(selectedPath);
        bool ok = false;
        const QString newName = QInputDialog::getText(this, "重命名远程项目", "新名称：", QLineEdit::Normal, info.fileName(), &ok).trimmed();
        if (!ok)
        {
            return;
        }
        if (!isValidRemoteName(newName))
        {
            showInvalidRemoteNameWarning(this);
            return;
        }
        if (newName != info.fileName())
        {
            m_remoteRenameRequested(selectedPath, remoteSiblingPath(selectedPath, newName));
        }
    }
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
 * @brief 在当前本地目录中新建文件夹。
 */
void FilePanel::createLocalDirectory()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, "新建本地文件夹", "文件夹名称：", QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok)
    {
        return;
    }
    if (!isValidRemoteName(name))
    {
        showInvalidRemoteNameWarning(this);
        return;
    }

    const QString path = QDir(m_currentPath).filePath(name);
    if (!QDir().mkdir(path))
    {
        showFileOperationWarning(this, "新建本地文件夹失败", QString("无法创建文件夹：%1").arg(path));
        return;
    }
    refresh();
}

/**
 * @brief 在当前本地目录中新建空文件。
 */
void FilePanel::createLocalFile()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, "新建本地文件", "文件名称：", QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok)
    {
        return;
    }
    if (!isValidRemoteName(name))
    {
        showInvalidRemoteNameWarning(this);
        return;
    }

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
}

/**
 * @brief 异步删除指定本地项目。
 * @param path 要删除的本地文件或目录。
 */
void FilePanel::removeLocalPath(const QString &path)
{
    if (m_pendingLocalDeletes.contains(path))
    {
        showFileOperationWarning(this, "删除本地项目", "该项目正在删除，请等待完成。");
        return;
    }

    const QFileInfo info(path);
    if (!info.exists())
    {
        showFileOperationWarning(this, "删除本地项目失败", QString("项目不存在：%1").arg(path));
        return;
    }

    const QString text = QString("确定删除本地项目？\n%1").arg(path);
    if (QMessageBox::question(this, "删除本地项目", text) != QMessageBox::Yes)
    {
        return;
    }

    m_pendingLocalDeletes.insert(path);
    QPointer<FilePanel> panel(this);
    QThread *thread = QThread::create([panel, path]() {
        const QFileInfo info(path);
        const bool removed = info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
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
 * @brief 处理本地异步删除完成后的 UI 更新。
 * @param path 已尝试删除的路径。
 * @param removed 是否删除成功。
 */
void FilePanel::finishLocalRemove(const QString &path, bool removed)
{
    m_pendingLocalDeletes.remove(path);
    if (!removed)
    {
        showFileOperationWarning(this, "删除本地项目失败", QString("无法删除：%1").arg(path));
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

    bool ok = false;
    const QString newName = QInputDialog::getText(this, "重命名本地项目", "新名称：", QLineEdit::Normal, info.fileName(), &ok).trimmed();
    if (!ok)
    {
        return;
    }
    if (!isValidRemoteName(newName))
    {
        showInvalidRemoteNameWarning(this);
        return;
    }
    if (newName == info.fileName())
    {
        return;
    }

    const QString targetPath = QDir(info.absolutePath()).filePath(newName);
    if (QFileInfo::exists(targetPath))
    {
        showFileOperationWarning(this, "重命名本地项目失败", QString("目标已存在：%1").arg(targetPath));
        return;
    }
    if (!QFile::rename(path, targetPath))
    {
        showFileOperationWarning(this, "重命名本地项目失败", QString("无法重命名：%1").arg(path));
        return;
    }
    refresh();
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

