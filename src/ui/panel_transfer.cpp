#include "ui/FilePanel.h"
#include "ui/panel_shared.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDropEvent>
#include <QDrag>
#include <QDir>
#include <QDateTime>
#include <QEvent>
#include <QFileInfo>
#include <QHeaderView>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QLocale>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>

#include <algorithm>

using namespace panel_shared;

namespace
{
QString formattedModifiedTime(const QString &value)
{
    const QString normalized = value.simplified();
    if (normalized.isEmpty())
    {
        return {};
    }

    QDateTime parsed = QDateTime::fromString(normalized, "yyyy-MM-dd HH:mm:ss");
    if (!parsed.isValid())
    {
        parsed = QDateTime::fromString(normalized, Qt::ISODate);
    }
    if (!parsed.isValid())
    {
        const QStringList parts = normalized.split(' ');
        if (parts.size() == 3 && parts.at(2).contains(':'))
        {
            const QDateTime now = QDateTime::currentDateTime();
            parsed = QLocale::c().toDateTime(
                QString("%1 %2 %3 %4").arg(parts.at(0), parts.at(1), QString::number(now.date().year()), parts.at(2)),
                "MMM d yyyy HH:mm");
            if (parsed.isValid() && parsed > now.addDays(1))
            {
                parsed = parsed.addYears(-1);
            }
        }
        else if (parts.size() == 3)
        {
            parsed = QLocale::c().toDateTime(normalized, "MMM d yyyy");
        }
    }
    return parsed.isValid() ? parsed.toString("yyyy/MM/dd HH:mm:ss") : value;
}

QByteArray encodeRemoteTransferItems(const QList<RemoteTransferItem> &items)
{
    QJsonArray array;
    for (const RemoteTransferItem &item : items)
    {
        QJsonObject object;
        object.insert("path", item.path);
        object.insert("isDirectory", item.isDirectory);
        array.append(object);
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

QList<RemoteTransferItem> decodeRemoteTransferItems(const QByteArray &payload)
{
    QList<RemoteTransferItem> items;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray())
    {
        return items;
    }

    QSet<QString> seenPaths;
    for (const QJsonValue &value : document.array())
    {
        if (!value.isObject())
        {
            continue;
        }
        const QJsonObject object = value.toObject();
        RemoteTransferItem item;
        item.path = object.value("path").toString();
        item.isDirectory = object.value("isDirectory").toBool();
        if (item.path.isEmpty() || seenPaths.contains(item.path))
        {
            continue;
        }
        seenPaths.insert(item.path);
        items.append(item);
    }
    return items;
}
} // namespace

/**
 * @brief 处理文件表格视口上的拖拽和投放事件。
 * @param watched 事件接收对象。
 * @param event 待处理事件。
 * @return 事件已被面板消费时返回 true。
 */
bool FilePanel::eventFilter(QObject *watched, QEvent *event)
{
    if (m_table == nullptr || watched != m_table->viewport())
    {
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseButtonPress)
    {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton)
        {
            m_dragStartPosition = mouseEvent->pos();
        }
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::MouseMove)
    {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if ((mouseEvent->buttons() & Qt::LeftButton) != 0
            && (mouseEvent->pos() - m_dragStartPosition).manhattanLength() >= QApplication::startDragDistance())
        {
            startDragFromSelection();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove)
    {
        auto *dragEvent = static_cast<QDragMoveEvent *>(event);
        if (canAcceptTransferDrop(dragEvent->mimeData()))
        {
            dragEvent->acceptProposedAction();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    if (event->type() == QEvent::Drop)
    {
        auto *dropEvent = static_cast<QDropEvent *>(event);
        if (canAcceptTransferDrop(dropEvent->mimeData()))
        {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            const QPoint dropPosition = dropEvent->position().toPoint();
#else
            const QPoint dropPosition = dropEvent->pos();
#endif
            handleTransferDrop(dropEvent->mimeData(), dropPosition);
            dropEvent->acceptProposedAction();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    return QWidget::eventFilter(watched, event);
}

/**
 * @brief 按用户点击的表头列切换远程项目排序。
 * @param column 表头列索引。
 */
void FilePanel::sortRemoteItemsByColumn(int column)
{
    if (m_mode != Mode::RemotePlaceholder || m_remoteItems.empty())
    {
        return;
    }

    if (m_remoteSortColumn == column)
    {
        m_remoteSortOrder = m_remoteSortOrder == Qt::AscendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder;
    }
    else
    {
        m_remoteSortColumn = column;
        m_remoteSortOrder = Qt::AscendingOrder;
    }

    populateRemoteItems(m_currentPath, m_remoteItems, m_stateLabel->text());
}

/**
 * @brief 将当前选中文件项目封装为拖拽数据并启动拖拽。
 */
void FilePanel::startDragFromSelection()
{
    const QList<RemoteTransferItem> items = selectedFileTransferItems();
    if (items.isEmpty())
    {
        return;
    }

    auto *mimeData = new QMimeData();
    if (m_mode == Mode::Local)
    {
        QList<QUrl> urls;
        for (const RemoteTransferItem &item : items)
        {
            urls.append(QUrl::fromLocalFile(item.path));
        }
        mimeData->setUrls(urls);
    }
    else
    {
        mimeData->setData(RemotePathMimeType, encodeRemoteTransferItems(items));
    }

    auto *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    drag->exec(Qt::CopyAction);
}

/**
 * @brief 判断当前面板是否可以接收给定拖放数据。
 * @param mimeData 拖放数据。
 * @return 可以接收时返回 true。
 */
bool FilePanel::canAcceptTransferDrop(const QMimeData *mimeData) const
{
    if (mimeData == nullptr)
    {
        return false;
    }

    if (m_mode == Mode::RemotePlaceholder)
    {
        return (m_localFilesDroppedOnRemote != nullptr && mimeData->hasUrls())
            || (m_remoteFilesDroppedOnRemote != nullptr && mimeData->hasFormat(RemotePathMimeType));
    }

    if (m_mode == Mode::Local)
    {
        return m_remoteFilesDroppedOnLocal != nullptr && mimeData->hasFormat(RemotePathMimeType);
    }

    return false;
}

/**
 * @brief 根据面板模式处理本地和远程文件拖放。
 * @param mimeData 拖放数据。
 * @param position 投放位置。
 */
void FilePanel::handleTransferDrop(const QMimeData *mimeData, const QPoint &position)
{
    if (!canAcceptTransferDrop(mimeData))
    {
        return;
    }

    if (m_mode == Mode::RemotePlaceholder)
    {
        if (mimeData->hasFormat(RemotePathMimeType) && m_remoteFilesDroppedOnRemote)
        {
            QStringList remotePaths;
            for (const RemoteTransferItem &item : decodeRemoteTransferItems(mimeData->data(RemotePathMimeType)))
            {
                remotePaths.append(item.path);
            }
            if (!remotePaths.isEmpty())
            {
                m_remoteFilesDroppedOnRemote(remotePaths, remoteDropTargetDirectory(position));
            }
            return;
        }

        QStringList localPaths;
        for (const QUrl &url : mimeData->urls())
        {
            if (!url.isLocalFile())
            {
                continue;
            }
            const QString path = url.toLocalFile();
            if (QFileInfo::exists(path))
            {
                localPaths.append(path);
            }
        }
        if (!localPaths.isEmpty())
        {
            m_localFilesDroppedOnRemote(localPaths);
        }
        return;
    }

    const QList<RemoteTransferItem> remoteItems = decodeRemoteTransferItems(mimeData->data(RemotePathMimeType));
    if (!remoteItems.isEmpty())
    {
        m_remoteFilesDroppedOnLocal(remoteItems);
    }
}

/**
 * @brief 收集当前表格选中行对应的传输路径和类型。
 * @return 按行号排序的远程传输项目列表。
 */
QList<RemoteTransferItem> FilePanel::selectedFileTransferItems() const
{
    QList<RemoteTransferItem> items;
    QList<int> rows;
    for (QTableWidgetItem *item : m_table->selectedItems())
    {
        if (!rows.contains(item->row()))
        {
            rows.append(item->row());
        }
    }
    std::sort(rows.begin(), rows.end());

    for (int row : rows)
    {
        QTableWidgetItem *nameItem = m_table->item(row, 0);
        if (nameItem == nullptr)
        {
            continue;
        }

        const QString path = nameItem->data(Qt::UserRole).toString();
        if (path.isEmpty())
        {
            continue;
        }

        RemoteTransferItem item;
        item.path = path;
        item.isDirectory = nameItem->data(Qt::UserRole + 1).toBool();
        items.append(item);
    }

    return items;
}

/**
 * @brief 计算远程面板拖放目标目录。
 * @param position 投放位置。
 * @return 远程目标目录路径。
 */
QString FilePanel::remoteDropTargetDirectory(const QPoint &position) const
{
    if (m_mode != Mode::RemotePlaceholder)
    {
        return {};
    }

    QTableWidgetItem *item = m_table->itemAt(position);
    if (item == nullptr)
    {
        return m_currentPath.isEmpty() ? "/" : m_currentPath;
    }

    QTableWidgetItem *nameItem = m_table->item(item->row(), 0);
    if (nameItem == nullptr)
    {
        return m_currentPath.isEmpty() ? "/" : m_currentPath;
    }

    const bool isDirectory = nameItem->data(Qt::UserRole + 1).toBool();
    return isDirectory ? nameItem->data(Qt::UserRole).toString() : m_currentPath;
}

/**
 * @brief 将本地目录内容填充到文件表格。
 * @param path 本地目录路径。
 */
void FilePanel::populateLocalDirectory(const QString &path)
{
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    const QDir dir(path);
    const QFileInfoList entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);

    for (const QFileInfo &entry : entries)
    {
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        QTableWidgetItem *nameItem = createItem(entry.fileName(), m_iconProvider.icon(entry));
        nameItem->setData(Qt::UserRole, entry.absoluteFilePath());
        nameItem->setData(Qt::UserRole + 1, entry.isDir());
        nameItem->setToolTip(entry.absoluteFilePath());

        m_table->setItem(row, 0, nameItem);
        m_table->setItem(row, 1, createItem(entry.isDir() ? "" : formatFileSize(entry.size())));
        m_table->setItem(row, 2, createItem(entry.isDir() ? "文件夹" : entry.suffix().isEmpty() ? "文件" : entry.suffix()));
        m_table->setItem(row, 3, createItem(entry.lastModified().toString("yyyy/MM/dd HH:mm:ss")));
        m_table->setItem(row, 4, createItem(entry.permission(QFile::WriteUser) ? "可写" : "只读"));
        m_table->setItem(row, 5, createItem(entry.owner()));
    }

    m_table->setSortingEnabled(true);
    m_stateLabel->setText(QString("%1 个项目").arg(entries.size()));
}

/**
 * @brief 填充远程未连接时的占位表格和目录树。
 */
void FilePanel::populateRemotePlaceholder()
{
    m_pathEdit->setText("/");
    m_pathEdit->setEnabled(false);
    m_refreshButton->setEnabled(false);
    m_backButton->setEnabled(false);
    m_forwardButton->setEnabled(false);
    m_upButton->setEnabled(false);

    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, createItem("尚未连接远程会话"));
    m_table->setItem(row, 1, createItem(""));
    m_table->setItem(row, 2, createItem("占位"));
    m_table->setItem(row, 3, createItem(""));
    m_table->setItem(row, 4, createItem(""));
    m_table->setItem(row, 5, createItem(""));
    m_stateLabel->setText("远程面板占位，后续接入 FTP/SFTP 会话。");
    updateRemoteTree("/", {});
}

/**
 * @brief 填充远程连接中状态。
 * @param status 用户可见连接状态文本。
 */
void FilePanel::setRemoteConnecting(const QString &status)
{
    if (m_mode != Mode::RemotePlaceholder)
    {
        return;
    }

    m_currentPath.clear();
    m_remoteItems.clear();
    m_pathEdit->setText("/");
    m_pathEdit->setEnabled(false);
    m_refreshButton->setEnabled(false);
    m_backButton->setEnabled(false);
    m_forwardButton->setEnabled(false);
    m_upButton->setEnabled(false);

    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, createItem("正在连接远程会话"));
    m_table->setItem(row, 1, createItem(""));
    m_table->setItem(row, 2, createItem("连接中"));
    m_table->setItem(row, 3, createItem(""));
    m_table->setItem(row, 4, createItem(""));
    m_table->setItem(row, 5, createItem(""));
    m_stateLabel->setText(status.isEmpty() ? "正在连接远程会话..." : status);
    updateRemoteTree("/", {});
}

/**
 * @brief 显示远程目录后台加载状态，同时保留当前表格内容。
 * @param path 正在请求的远程目录。
 */
void FilePanel::setRemoteLoading(const QString &path)
{
    if (m_mode != Mode::RemotePlaceholder)
    {
        return;
    }

    m_pathEdit->setText(path);
    m_stateLabel->setText(QString("正在加载：%1").arg(path));
}

/**
 * @brief 将远程项目列表填充到文件表格。
 * @param path 当前远程路径。
 * @param items 远程项目列表。
 * @param status 用户可见状态文本。
 */
void FilePanel::populateRemoteItems(const QString &path, const std::vector<FileItem> &items, const QString &status)
{
    m_currentPath = path.isEmpty() ? "/" : path;
    m_remoteItems = items;
    m_pathEdit->setText(m_currentPath);
    m_pathEdit->setEnabled(true);
    m_refreshButton->setEnabled(true);

    m_table->setSortingEnabled(false);
    const std::vector<FileItem> displayItems = sortedRemoteItems(items, m_remoteSortColumn, m_remoteSortOrder);
    QHeaderView *header = m_table->horizontalHeader();
    for (int column = 1; column < m_table->columnCount(); ++column)
    {
        header->setSectionResizeMode(column, QHeaderView::Interactive);
    }

    const bool tableUpdatesEnabled = m_table->updatesEnabled();
    m_table->setUpdatesEnabled(false);
    const QSignalBlocker tableSignals(m_table);
    m_table->clearContents();
    m_table->setRowCount(static_cast<int>(displayItems.size()));
    QHash<QString, QIcon> fileIcons;

    for (int row = 0; row < static_cast<int>(displayItems.size()); ++row)
    {
        const FileItem &entry = displayItems.at(static_cast<std::size_t>(row));
        const bool isDirectory = entry.type == FileItemType::Directory;
        const QString name = QString::fromStdString(entry.name);
        QIcon icon;
        if (entry.type == FileItemType::File)
        {
            const QString iconKey = QFileInfo(name).suffix().toLower();
            const auto cachedIcon = fileIcons.constFind(iconKey);
            if (cachedIcon != fileIcons.constEnd())
            {
                icon = cachedIcon.value();
            }
            else
            {
                icon = iconForFileItem(this, m_iconProvider, name, entry.type);
                fileIcons.insert(iconKey, icon);
            }
        }
        else
        {
            icon = iconForFileItem(this, m_iconProvider, name, entry.type);
        }
        QTableWidgetItem *nameItem = createItem(name, icon);
        nameItem->setData(Qt::UserRole, QString::fromStdString(entry.path));
        nameItem->setData(Qt::UserRole + 1, isDirectory);
        nameItem->setToolTip(QString::fromStdString(entry.path));

        m_table->setItem(row, 0, nameItem);
        m_table->setItem(row, 1, createItem(isDirectory ? "" : formatFileSize(entry.size)));
        m_table->setItem(row, 2, createItem(fileItemTypeText(entry.type)));
        m_table->setItem(row, 3, createItem(formattedModifiedTime(QString::fromStdString(entry.modifiedTime))));
        m_table->setItem(row, 4, createItem(QString::fromStdString(entry.permissions)));
        m_table->setItem(row, 5, createItem(QString::fromStdString(entry.owner)));
    }

    m_table->setUpdatesEnabled(tableUpdatesEnabled);
    if (tableUpdatesEnabled)
    {
        m_table->viewport()->update();
    }

    m_table->horizontalHeader()->setSortIndicator(m_remoteSortColumn, m_remoteSortOrder);
    m_stateLabel->setText(status.isEmpty() ? QString("%1 个远程项目").arg(items.size()) : status);
    updateRemoteTree(m_currentPath, items);
}

/**
 * @brief 按当前路径重建本地目录树并选中当前目录。
 * @param path 当前本地路径。
 */
void FilePanel::updateLocalTreeSelection(const QString &path)
{
    if (m_localTree == nullptr)
    {
        return;
    }

    const QString currentPath = QDir::cleanPath(path);
    m_localTree->clear();

    QTreeWidgetItem *currentItem = nullptr;
    const QFileInfoList drives = QDir::drives();
    const QIcon driveIcon = style()->standardIcon(QStyle::SP_DriveHDIcon);
    const QIcon directoryIcon = style()->standardIcon(QStyle::SP_DirIcon);
    for (const QFileInfo &drive : drives)
    {
        const QString drivePath = QDir::cleanPath(drive.absoluteFilePath());
        auto *driveItem = new QTreeWidgetItem(m_localTree, {QDir::toNativeSeparators(drivePath)});
        driveItem->setIcon(0, driveIcon);
        driveItem->setData(0, Qt::UserRole, drivePath);

        if (!currentPath.startsWith(drivePath, Qt::CaseInsensitive))
        {
            continue;
        }

        currentItem = driveItem;
        const QStringList pathParts = QDir(drivePath).relativeFilePath(currentPath).split('/', Qt::SkipEmptyParts);
        QString parentPath = drivePath;
        for (const QString &part : pathParts)
        {
            const QFileInfoList siblingDirs = QDir(parentPath).entryInfoList(
                QDir::Dirs | QDir::NoDotAndDotDot,
                QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
            QTreeWidgetItem *pathItem = nullptr;
            const QString nextPath = QDir::cleanPath(QDir(parentPath).filePath(part));
            for (const QFileInfo &siblingDir : siblingDirs)
            {
                const QString siblingPath = QDir::cleanPath(siblingDir.absoluteFilePath());
                auto *siblingItem = new QTreeWidgetItem(currentItem, {siblingDir.fileName()});
                siblingItem->setIcon(0, directoryIcon);
                siblingItem->setData(0, Qt::UserRole, siblingPath);
                if (siblingPath.compare(nextPath, Qt::CaseInsensitive) == 0)
                {
                    pathItem = siblingItem;
                }
            }

            currentItem->setExpanded(true);
            if (pathItem == nullptr)
            {
                pathItem = new QTreeWidgetItem(currentItem, {part});
                pathItem->setIcon(0, directoryIcon);
                pathItem->setData(0, Qt::UserRole, nextPath);
            }
            currentItem = pathItem;
            parentPath = nextPath;
        }
    }

    if (currentItem == nullptr)
    {
        return;
    }

    const QDir currentDir(currentPath);
    const QFileInfoList childDirs = currentDir.entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &childDir : childDirs)
    {
        auto *childItem = new QTreeWidgetItem(currentItem, {childDir.fileName()});
        childItem->setIcon(0, directoryIcon);
        childItem->setData(0, Qt::UserRole, QDir::cleanPath(childDir.absoluteFilePath()));
    }

    currentItem->setExpanded(true);
    m_localTree->setCurrentItem(currentItem);
    m_localTree->scrollToItem(currentItem, QAbstractItemView::PositionAtCenter);
}

/**
 * @brief 按已知目录和当前目录项目重建远程目录树。
 * @param path 当前远程路径。
 * @param items 当前目录项目列表。
 */
void FilePanel::updateRemoteTree(const QString &path, const std::vector<FileItem> &items)
{
    if (m_remoteTree == nullptr)
    {
        return;
    }

    const QString currentPath = path.isEmpty() ? "/" : path;
    const bool treeUpdatesEnabled = m_remoteTree->updatesEnabled();
    m_remoteTree->setUpdatesEnabled(false);
    const QSignalBlocker treeSignals(m_remoteTree);
    m_remoteTree->clear();

    const QIcon rootIcon = style()->standardIcon(QStyle::SP_DriveNetIcon);
    const QIcon directoryIcon = style()->standardIcon(QStyle::SP_DirIcon);
    auto *root = new QTreeWidgetItem(m_remoteTree, {"/"});
    root->setIcon(0, rootIcon);
    root->setData(0, Qt::UserRole, "/");

    QMap<QString, QTreeWidgetItem *> treeItems;
    treeItems.insert("/", root);

    QStringList directories = m_remoteKnownDirectories;
    directories.append(currentPath);
    for (const FileItem &entry : items)
    {
        if (entry.type == FileItemType::Directory)
        {
            directories.append(QString::fromStdString(entry.path));
        }
    }
    directories.removeDuplicates();
    directories.sort(Qt::CaseInsensitive);

    QTreeWidgetItem *currentItem = root;
    for (QString directoryPath : directories)
    {
        if (directoryPath.isEmpty())
        {
            continue;
        }
        if (!directoryPath.startsWith('/'))
        {
            directoryPath.prepend('/');
        }
        while (directoryPath.size() > 1 && directoryPath.endsWith('/'))
        {
            directoryPath.chop(1);
        }
        if (directoryPath == "/" || treeItems.contains(directoryPath))
        {
            continue;
        }

        const int slashIndex = directoryPath.lastIndexOf('/');
        const QString parentPath = slashIndex <= 0 ? "/" : directoryPath.left(slashIndex);
        QTreeWidgetItem *parentItem = treeItems.value(parentPath, root);
        const QString name = directoryPath.mid(slashIndex + 1);

        auto *child = new QTreeWidgetItem(parentItem, {name});
        child->setIcon(0, directoryIcon);
        child->setData(0, Qt::UserRole, directoryPath);
        treeItems.insert(directoryPath, child);
        parentItem->setExpanded(true);
        if (directoryPath == currentPath)
        {
            currentItem = child;
        }
    }

    currentItem->setExpanded(true);
    m_remoteTree->setCurrentItem(currentItem);
    m_remoteTree->scrollToItem(currentItem, QAbstractItemView::PositionAtCenter);
    m_remoteTree->setUpdatesEnabled(treeUpdatesEnabled);
    if (treeUpdatesEnabled)
    {
        m_remoteTree->viewport()->update();
    }
}

/**
 * @brief 创建带提示文本的文件表格单元格。
 * @param text 单元格显示文本。
 * @param icon 单元格图标。
 * @return 新建的表格项。
 */
QTableWidgetItem *FilePanel::createItem(const QString &text, const QIcon &icon) const
{
    auto *item = new QTableWidgetItem(icon, text);
    item->setToolTip(text);
    return item;
}
