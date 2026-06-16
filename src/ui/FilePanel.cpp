#include "ui/FilePanel.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDropEvent>
#include <QDrag>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMimeData>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace
{
const char *RemotePathMimeType = "application/x-dirbridge-remote-paths";

QString fileItemTypeText(FileItemType type)
{
    switch (type)
    {
    case FileItemType::File:
        return "文件";
    case FileItemType::Directory:
        return "文件夹";
    case FileItemType::Symlink:
        return "符号链接";
    case FileItemType::Other:
        return "未知";
    }

    return "未知";
}

QString formatFileSize(qint64 size)
{
    if (size < 0)
    {
        return "";
    }

    static const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(size);
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < units.size() - 1)
    {
        value /= 1024.0;
        ++unitIndex;
    }

    if (unitIndex == 0)
    {
        return QString("%1 %2").arg(size).arg(units.at(unitIndex));
    }

    return QString("%1 %2").arg(value, 0, 'f', 1).arg(units.at(unitIndex));
}

QString desktopPath()
{
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    return desktop.isEmpty() ? QDir::homePath() : desktop;
}

QIcon iconForFileItem(QWidget *widget, const QFileIconProvider &iconProvider, const QString &name, FileItemType type)
{
    switch (type)
    {
    case FileItemType::Directory:
        return widget->style()->standardIcon(QStyle::SP_DirIcon);
    case FileItemType::Symlink:
        return widget->style()->standardIcon(QStyle::SP_FileIcon);
    case FileItemType::File:
    {
        const QIcon icon = iconProvider.icon(QFileInfo(name));
        return icon.isNull() ? widget->style()->standardIcon(QStyle::SP_FileIcon) : icon;
    }
    case FileItemType::Other:
        return widget->style()->standardIcon(QStyle::SP_FileIcon);
    }

    return widget->style()->standardIcon(QStyle::SP_FileIcon);
}

bool isValidRemoteName(const QString &name)
{
    if (name.isEmpty() || name == "." || name == "..")
    {
        return false;
    }

    return !name.contains('/') && !name.contains('\\');
}

void showInvalidRemoteNameWarning(QWidget *parent)
{
    QMessageBox::warning(parent, "远程名称无效", "名称不能为空，不能是 . 或 ..，也不能包含 / 或 \\。");
}

void showFileOperationWarning(QWidget *parent, const QString &title, const QString &message)
{
    QMessageBox::warning(parent, title, message);
}

int compareText(const QString &left, const QString &right)
{
    return QString::localeAwareCompare(left.toCaseFolded(), right.toCaseFolded());
}

int compareRemoteItemColumn(const FileItem &left, const FileItem &right, int column)
{
    switch (column)
    {
    case 1:
        if (left.size == right.size)
        {
            return 0;
        }
        return left.size < right.size ? -1 : 1;
    case 2:
        return compareText(fileItemTypeText(left.type), fileItemTypeText(right.type));
    case 3:
        return compareText(QString::fromStdString(left.modifiedTime), QString::fromStdString(right.modifiedTime));
    case 0:
    default:
        return compareText(QString::fromStdString(left.name), QString::fromStdString(right.name));
    }
}

std::vector<FileItem> sortedRemoteItems(const std::vector<FileItem> &items, int column, Qt::SortOrder order)
{
    std::vector<FileItem> sorted = items;
    std::sort(sorted.begin(), sorted.end(), [column, order](const FileItem &left, const FileItem &right) {
        const bool leftDirectory = left.type == FileItemType::Directory;
        const bool rightDirectory = right.type == FileItemType::Directory;
        if (leftDirectory != rightDirectory)
        {
            return order == Qt::AscendingOrder ? leftDirectory : rightDirectory;
        }

        int comparison = compareRemoteItemColumn(left, right, column);
        if (comparison == 0)
        {
            comparison = compareText(QString::fromStdString(left.name), QString::fromStdString(right.name));
        }

        return order == Qt::AscendingOrder ? comparison < 0 : comparison > 0;
    });
    return sorted;
}
}

FilePanel::FilePanel(Mode mode, QWidget *parent)
    : QWidget(parent)
    , m_mode(mode)
{
    setupUi();
    connectSignals();
    initialize();
}

void FilePanel::setFileTreeVisible(bool visible)
{
    if (m_localTree != nullptr)
    {
        m_localTree->setVisible(visible);
    }

    if (m_remoteTree != nullptr)
    {
        m_remoteTree->setVisible(visible);
    }
}

bool FilePanel::isFileTreeVisible() const
{
    if (m_localTree != nullptr)
    {
        return m_localTree->isVisible();
    }

    if (m_remoteTree != nullptr)
    {
        return m_remoteTree->isVisible();
    }

    return false;
}

void FilePanel::setRemotePathRequestedHandler(std::function<void(const QString &, bool)> handler)
{
    m_remotePathRequested = std::move(handler);
}

void FilePanel::setRemoteRefreshRequestedHandler(std::function<void()> handler)
{
    m_remoteRefreshRequested = std::move(handler);
}

void FilePanel::setRemoteCreateDirectoryRequestedHandler(std::function<void(const QString &)> handler)
{
    m_remoteCreateDirectoryRequested = std::move(handler);
}

void FilePanel::setRemoteCreateFileRequestedHandler(std::function<void(const QString &)> handler)
{
    m_remoteCreateFileRequested = std::move(handler);
}

void FilePanel::setRemoteRemoveRequestedHandler(std::function<void(const QString &)> handler)
{
    m_remoteRemoveRequested = std::move(handler);
}

void FilePanel::setRemoteRenameRequestedHandler(std::function<void(const QString &, const QString &)> handler)
{
    m_remoteRenameRequested = std::move(handler);
}

void FilePanel::setLocalUploadRequestedHandler(std::function<void(const QString &)> handler)
{
    m_localUploadRequested = std::move(handler);
}

void FilePanel::setRemoteDownloadRequestedHandler(std::function<void(const QString &)> handler)
{
    m_remoteDownloadRequested = std::move(handler);
}

void FilePanel::setLocalFilesDroppedOnRemoteHandler(std::function<void(const QStringList &)> handler)
{
    m_localFilesDroppedOnRemote = std::move(handler);
}

void FilePanel::setRemoteFilesDroppedOnLocalHandler(std::function<void(const QStringList &)> handler)
{
    m_remoteFilesDroppedOnLocal = std::move(handler);
}

void FilePanel::setRemoteFilesDroppedOnRemoteHandler(std::function<void(const QStringList &, const QString &)> handler)
{
    m_remoteFilesDroppedOnRemote = std::move(handler);
}

QString FilePanel::currentPath() const
{
    return m_currentPath;
}

void FilePanel::setRemoteSummary(const QString &curlVersion, bool hasFtp, bool hasSftp)
{
    if (m_mode != Mode::RemotePlaceholder)
    {
        return;
    }

    m_stateLabel->setText(QString("远程未连接。libcurl %1，FTP=%2，SFTP=%3")
        .arg(curlVersion)
        .arg(hasFtp ? "可用" : "不可用")
        .arg(hasSftp ? "可用" : "不可用"));
}

void FilePanel::setRemoteKnownDirectories(const QStringList &directories)
{
    m_remoteKnownDirectories = directories;
    m_remoteKnownDirectories.removeDuplicates();
}

void FilePanel::setRemoteItems(const QString &path, const std::vector<FileItem> &items, const QString &status, bool addToHistory)
{
    if (m_mode != Mode::RemotePlaceholder)
    {
        return;
    }

    populateRemoteItems(path, items, status);

    if (addToHistory)
    {
        while (m_history.size() > m_historyIndex + 1)
        {
            m_history.removeLast();
        }
        m_history.append(m_currentPath);
        m_historyIndex = m_history.size() - 1;
    }
    else if (m_historyIndex >= 0 && m_historyIndex < m_history.size())
    {
        m_history[m_historyIndex] = m_currentPath;
    }

    updateRemoteNavigationButtons();
}

void FilePanel::setRemoteDisconnected(const QString &status)
{
    if (m_mode != Mode::RemotePlaceholder)
    {
        return;
    }

    m_history.clear();
    m_historyIndex = -1;
    populateRemotePlaceholder();
    m_stateLabel->setText(status);
}

void FilePanel::setRemoteError(const QString &status)
{
    if (m_mode != Mode::RemotePlaceholder)
    {
        return;
    }

    m_stateLabel->setText(status);
    m_pathEdit->setText(m_currentPath.isEmpty() ? "/" : m_currentPath);
    updateRemoteNavigationButtons();
}

void FilePanel::setLocalPathForTesting(const QString &path)
{
    if (m_mode != Mode::Local)
    {
        return;
    }

    navigateTo(path);
}

void FilePanel::setupUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(4);

    auto *toolbar = new QWidget(this);
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(4);

    m_backButton = new QPushButton("<", toolbar);
    m_forwardButton = new QPushButton(">", toolbar);
    m_upButton = new QPushButton("..", toolbar);
    m_refreshButton = new QPushButton("刷新", toolbar);
    m_pathEdit = new QLineEdit(toolbar);

    const QString objectPrefix = m_mode == Mode::Local ? "local" : "remote";
    m_backButton->setObjectName(objectPrefix + "BackButton");
    m_forwardButton->setObjectName(objectPrefix + "ForwardButton");
    m_upButton->setObjectName(objectPrefix + "UpButton");
    m_refreshButton->setObjectName(objectPrefix + "RefreshButton");
    m_pathEdit->setObjectName(objectPrefix + "PathEdit");

    m_backButton->setToolTip("后退");
    m_forwardButton->setToolTip("前进");
    m_upButton->setToolTip("上一级");
    m_refreshButton->setToolTip("刷新当前目录");

    toolbarLayout->addWidget(m_backButton);
    toolbarLayout->addWidget(m_forwardButton);
    toolbarLayout->addWidget(m_upButton);
    toolbarLayout->addWidget(m_pathEdit, 1);
    toolbarLayout->addWidget(m_refreshButton);

    m_contentSplitter = new QSplitter(Qt::Horizontal, this);

    if (m_mode == Mode::Local)
    {
        m_localTree = new QTreeWidget(m_contentSplitter);
        m_localTree->setObjectName("localFileTree");
        m_localTree->setHeaderHidden(true);
        m_localTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_localTree->setMinimumWidth(180);
        m_contentSplitter->addWidget(m_localTree);
    }
    else
    {
        m_remoteTree = new QTreeWidget(m_contentSplitter);
        m_remoteTree->setObjectName("remoteFileTree");
        m_remoteTree->setHeaderHidden(true);
        m_remoteTree->setMinimumWidth(160);
        m_contentSplitter->addWidget(m_remoteTree);
    }

    m_table = new QTableWidget(m_contentSplitter);
    m_table->setObjectName(objectPrefix + "FileTable");
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({"名称", "大小", "类型", "修改时间", "权限", "所有者"});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setAlternatingRowColors(true);
    m_table->setAcceptDrops(true);
    m_table->setDragEnabled(true);
    m_table->setDragDropMode(QAbstractItemView::DragDrop);
    m_table->setDefaultDropAction(Qt::CopyAction);
    m_table->setDropIndicatorShown(true);
    m_table->setSortingEnabled(false);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSortIndicatorShown(m_mode == Mode::RemotePlaceholder);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < m_table->columnCount(); ++column)
    {
        m_table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }

    m_contentSplitter->addWidget(m_table);
    m_table->viewport()->installEventFilter(this);
    m_contentSplitter->setStretchFactor(0, 0);
    m_contentSplitter->setStretchFactor(1, 1);
    m_contentSplitter->setSizes({220, 620});

    m_stateLabel = new QLabel(this);
    m_stateLabel->setObjectName(objectPrefix + "StateLabel");

    rootLayout->addWidget(toolbar);
    rootLayout->addWidget(m_contentSplitter, 1);
    rootLayout->addWidget(m_stateLabel);
}

void FilePanel::connectSignals()
{
    connect(m_refreshButton, &QPushButton::clicked, this, &FilePanel::refresh);
    connect(m_upButton, &QPushButton::clicked, this, &FilePanel::navigateUp);
    connect(m_backButton, &QPushButton::clicked, this, &FilePanel::navigateBack);
    connect(m_forwardButton, &QPushButton::clicked, this, &FilePanel::navigateForward);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this]() {
        navigateTo(m_pathEdit->text());
    });
    connect(m_table, &QTableWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        showUnifiedContextMenu(position);
    });
    connect(m_table->horizontalHeader(), &QHeaderView::sectionClicked, this, [this](int column) {
        if (m_mode == Mode::RemotePlaceholder && column >= 0 && column <= 3)
        {
            sortRemoteItemsByColumn(column);
        }
    });
    if (m_localTree != nullptr)
    {
        connect(m_localTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
            if (item == nullptr)
            {
                return;
            }

            const QString path = item->data(0, Qt::UserRole).toString();
            if (!path.isEmpty())
            {
                navigateTo(path);
            }
        });
    }
    if (m_remoteTree != nullptr)
    {
        connect(m_remoteTree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int) {
            if (item == nullptr)
            {
                return;
            }

            const QString path = item->data(0, Qt::UserRole).toString();
            if (!path.isEmpty())
            {
                requestRemotePath(path, true);
            }
        });
    }
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        QTableWidgetItem *nameItem = m_table->item(row, 0);
        if (nameItem == nullptr)
        {
            return;
        }

        const QString path = nameItem->data(Qt::UserRole).toString();
        if (m_mode == Mode::RemotePlaceholder)
        {
            const QString typeText = m_table->item(row, 2) == nullptr ? QString() : m_table->item(row, 2)->text();
            if (nameItem->data(Qt::UserRole + 1).toBool() || typeText == "未知")
            {
                requestRemotePath(path, true);
            }
            return;
        }

        const QFileInfo info(path);
        if (info.isDir())
        {
            navigateTo(path);
        }
        else
        {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        }
    });
}

void FilePanel::initialize()
{
    if (m_mode == Mode::Local)
    {
        navigateTo(desktopPath());
        return;
    }

    populateRemotePlaceholder();
}

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
            handleTransferDrop(dropEvent->mimeData(), dropEvent->position().toPoint());
            dropEvent->acceptProposedAction();
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    return QWidget::eventFilter(watched, event);
}

void FilePanel::navigateTo(const QString &path, bool addToHistory)
{
    if (m_mode == Mode::RemotePlaceholder)
    {
        requestRemotePath(path, addToHistory);
        return;
    }

    if (m_mode != Mode::Local)
    {
        return;
    }

    const QFileInfo info(path);
    if (!info.exists() || !info.isDir())
    {
        m_stateLabel->setText(QString("无法打开目录：%1").arg(path));
        m_pathEdit->setText(m_currentPath);
        return;
    }

    m_currentPath = QDir::cleanPath(info.absoluteFilePath());
    m_pathEdit->setText(m_currentPath);

    if (addToHistory)
    {
        while (m_history.size() > m_historyIndex + 1)
        {
            m_history.removeLast();
        }
        m_history.append(m_currentPath);
        m_historyIndex = m_history.size() - 1;
    }

    populateLocalDirectory(m_currentPath);
    updateLocalTreeSelection(m_currentPath);
    updateNavigationButtons();
}

void FilePanel::refresh()
{
    if (m_mode == Mode::Local)
    {
        populateLocalDirectory(m_currentPath);
        return;
    }

    if (m_remoteRefreshRequested)
    {
        m_remoteRefreshRequested();
    }
}

void FilePanel::navigateUp()
{
    if (m_mode == Mode::RemotePlaceholder)
    {
        QString path = m_currentPath.isEmpty() ? "/" : m_currentPath;
        if (path == "/")
        {
            return;
        }

        if (path.endsWith('/'))
        {
            path.chop(1);
        }

        const int slashIndex = path.lastIndexOf('/');
        requestRemotePath(slashIndex <= 0 ? "/" : path.left(slashIndex), true);
        return;
    }

    if (m_mode != Mode::Local)
    {
        return;
    }

    QDir dir(m_currentPath);
    if (dir.cdUp())
    {
        navigateTo(dir.absolutePath());
    }
}

void FilePanel::navigateBack()
{
    if (m_historyIndex <= 0)
    {
        return;
    }

    --m_historyIndex;
    if (m_mode == Mode::RemotePlaceholder)
    {
        requestRemotePath(m_history.at(m_historyIndex), false);
        return;
    }

    navigateTo(m_history.at(m_historyIndex), false);
}

void FilePanel::navigateForward()
{
    if (m_historyIndex + 1 >= m_history.size())
    {
        return;
    }

    ++m_historyIndex;
    if (m_mode == Mode::RemotePlaceholder)
    {
        requestRemotePath(m_history.at(m_historyIndex), false);
        return;
    }

    navigateTo(m_history.at(m_historyIndex), false);
}

void FilePanel::requestRemotePath(const QString &path, bool addToHistory)
{
    if (m_mode != Mode::RemotePlaceholder || !m_remotePathRequested)
    {
        return;
    }

    QString requestedPath = path.trimmed();
    if (requestedPath.isEmpty())
    {
        requestedPath = "/";
    }
    if (!requestedPath.startsWith('/'))
    {
        requestedPath.prepend('/');
    }

    m_remotePathRequested(requestedPath, addToHistory);
}

void FilePanel::showLocalContextMenu(const QPoint &position)
{
    showUnifiedContextMenu(position);
}

void FilePanel::showRemoteContextMenu(const QPoint &position)
{
    showUnifiedContextMenu(position);
}

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

    QMenu *viewMenu = menu.addMenu("查看");
    fileTreeAction = viewMenu->addAction("文件树");
    fileTreeAction->setCheckable(true);
    fileTreeAction->setChecked(isFileTreeVisible());

    if (hasSelection)
    {
        if (isLocal)
        {
            openAction = menu.addAction("打开");
        }
        else if (selectedIsDirectory)
        {
            enterAction = menu.addAction("进入");
        }
        if (isLocal)
        {
            uploadAction = menu.addAction("上传");
            uploadAction->setEnabled(!selectedIsDirectory && m_localUploadRequested != nullptr);
        }
        else if (!selectedIsDirectory)
        {
            downloadAction = menu.addAction("下载");
            downloadAction->setEnabled(m_remoteDownloadRequested != nullptr);
        }
        renameAction = menu.addAction("重命名");
        removeAction = menu.addAction("删除");
        menu.addSeparator();
        copyPathAction = menu.addAction("复制路径");
        copyFolderPathAction = menu.addAction("复制文件夹路径");
    }
    else
    {
        copyFolderPathAction = menu.addAction("复制文件夹路径");
    }

    menu.addSeparator();
    QMenu *newMenu = menu.addMenu("新建");
    newDirectoryAction = newMenu->addAction("新建文件夹");
    newFileAction = newMenu->addAction("新建文件");

    propertiesAction = menu.addAction("属性");

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
        m_remoteDownloadRequested(selectedPath);
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

    QMessageBox::information(const_cast<FilePanel *>(this), "远程属性", lines.join('\n'));
}

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

void FilePanel::removeLocalPath(const QString &path)
{
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

    bool removed = false;
    if (info.isDir())
    {
        removed = QDir(path).removeRecursively();
    }
    else
    {
        removed = QFile::remove(path);
    }

    if (!removed)
    {
        showFileOperationWarning(this, "删除本地项目失败", QString("无法删除：%1").arg(path));
        return;
    }
    refresh();
}

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
    QMessageBox::information(const_cast<FilePanel *>(this), "本地属性", lines.join('\n'));
}

void FilePanel::startDragFromSelection()
{
    const QStringList paths = selectedFileTransferPaths();
    if (paths.isEmpty())
    {
        return;
    }

    auto *mimeData = new QMimeData();
    if (m_mode == Mode::Local)
    {
        QList<QUrl> urls;
        for (const QString &path : paths)
        {
            urls.append(QUrl::fromLocalFile(path));
        }
        mimeData->setUrls(urls);
    }
    else
    {
        mimeData->setData(RemotePathMimeType, paths.join('\n').toUtf8());
    }

    auto *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    drag->exec(Qt::CopyAction);
}

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
            QStringList remotePaths = QString::fromUtf8(mimeData->data(RemotePathMimeType)).split('\n', Qt::SkipEmptyParts);
            remotePaths.removeDuplicates();
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

    QStringList remotePaths = QString::fromUtf8(mimeData->data(RemotePathMimeType)).split('\n', Qt::SkipEmptyParts);
    if (!remotePaths.isEmpty())
    {
        m_remoteFilesDroppedOnLocal(remotePaths);
    }
}

QStringList FilePanel::selectedFileTransferPaths() const
{
    QStringList paths;
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

        paths.append(path);
    }

    return paths;
}

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

QString FilePanel::remoteChildPath(const QString &name) const
{
    QString path = m_currentPath.isEmpty() ? "/" : m_currentPath;
    if (!path.endsWith('/'))
    {
        path.append('/');
    }
    return path + name;
}

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

void FilePanel::updateNavigationButtons()
{
    m_backButton->setEnabled(m_historyIndex > 0);
    m_forwardButton->setEnabled(m_historyIndex + 1 < m_history.size());
    m_upButton->setEnabled(QDir(m_currentPath).cdUp());
}

void FilePanel::updateRemoteNavigationButtons()
{
    if (m_mode != Mode::RemotePlaceholder)
    {
        return;
    }

    const bool hasPath = !m_currentPath.isEmpty();
    m_backButton->setEnabled(m_historyIndex > 0);
    m_forwardButton->setEnabled(m_historyIndex + 1 < m_history.size());
    m_upButton->setEnabled(hasPath && m_currentPath != "/");
    m_pathEdit->setEnabled(hasPath);
    m_refreshButton->setEnabled(hasPath);
}

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

        m_table->setItem(row, 0, nameItem);
        m_table->setItem(row, 1, createItem(entry.isDir() ? "" : formatFileSize(entry.size())));
        m_table->setItem(row, 2, createItem(entry.isDir() ? "文件夹" : entry.suffix().isEmpty() ? "文件" : entry.suffix()));
        m_table->setItem(row, 3, createItem(entry.lastModified().toString("yyyy-MM-dd HH:mm:ss")));
        m_table->setItem(row, 4, createItem(entry.permission(QFile::WriteUser) ? "可写" : "只读"));
        m_table->setItem(row, 5, createItem(entry.owner()));
    }

    m_table->setSortingEnabled(true);
    m_stateLabel->setText(QString("%1 个项目").arg(entries.size()));
}

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

void FilePanel::populateRemoteItems(const QString &path, const std::vector<FileItem> &items, const QString &status)
{
    m_currentPath = path.isEmpty() ? "/" : path;
    m_remoteItems = items;
    m_pathEdit->setText(m_currentPath);
    m_pathEdit->setEnabled(true);
    m_refreshButton->setEnabled(true);

    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    const std::vector<FileItem> displayItems = sortedRemoteItems(items, m_remoteSortColumn, m_remoteSortOrder);

    for (const FileItem &entry : displayItems)
    {
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        const bool isDirectory = entry.type == FileItemType::Directory;
        const QString name = QString::fromStdString(entry.name);
        QTableWidgetItem *nameItem = createItem(
            name,
            iconForFileItem(this, m_iconProvider, name, entry.type));
        nameItem->setData(Qt::UserRole, QString::fromStdString(entry.path));
        nameItem->setData(Qt::UserRole + 1, isDirectory);

        m_table->setItem(row, 0, nameItem);
        m_table->setItem(row, 1, createItem(isDirectory ? "" : formatFileSize(entry.size)));
        m_table->setItem(row, 2, createItem(fileItemTypeText(entry.type)));
        m_table->setItem(row, 3, createItem(QString::fromStdString(entry.modifiedTime)));
        m_table->setItem(row, 4, createItem(QString::fromStdString(entry.permissions)));
        m_table->setItem(row, 5, createItem(QString::fromStdString(entry.owner)));
    }

    m_table->horizontalHeader()->setSortIndicator(m_remoteSortColumn, m_remoteSortOrder);
    m_stateLabel->setText(status.isEmpty() ? QString("%1 个远程项目").arg(items.size()) : status);
    updateRemoteTree(m_currentPath, items);
}

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

void FilePanel::updateRemoteTree(const QString &path, const std::vector<FileItem> &items)
{
    if (m_remoteTree == nullptr)
    {
        return;
    }

    const QString currentPath = path.isEmpty() ? "/" : path;
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
}

QTableWidgetItem *FilePanel::createItem(const QString &text, const QIcon &icon) const
{
    auto *item = new QTableWidgetItem(icon, text);
    item->setToolTip(text);
    return item;
}

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
