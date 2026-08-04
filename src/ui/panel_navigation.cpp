#include "ui/FilePanel.h"
#include "ui/panel_shared.h"

#include <QAbstractItemView>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSplitter>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QUrl>
#include <QKeySequence>
#include <QVBoxLayout>

#include <utility>

using namespace panel_shared;
/**
 * @brief 切换左侧目录树的可见状态。
 * @param visible 是否显示目录树。
 */
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

/**
 * @brief 查询当前面板目录树是否可见。
 * @return 目录树可见时返回 true。
 */
bool FilePanel::isFileTreeVisible() const
{
    if (m_localTree != nullptr)
    {
        return !m_localTree->isHidden();
    }

    if (m_remoteTree != nullptr)
    {
        return !m_remoteTree->isHidden();
    }

    return false;
}

/**
 * @brief 设置文件树显示状态变更请求的统一回调。
 * @param handler 接收用户选择的新显示状态。
 */
void FilePanel::setFileTreeVisibilityRequestedHandler(std::function<void(bool)> handler)
{
    m_fileTreeVisibilityRequested = std::move(handler);
}

/**
 * @brief 设置远程路径跳转请求回调。
 * @param handler 接收目标路径与是否写入历史记录的回调。
 */
void FilePanel::setRemotePathRequestedHandler(std::function<void(const QString &, bool)> handler)
{
    m_remotePathRequested = std::move(handler);
}

/**
 * @brief 设置远程刷新请求回调。
 * @param handler 触发远程目录重新加载的回调。
 */
void FilePanel::setRemoteRefreshRequestedHandler(std::function<void()> handler)
{
    m_remoteRefreshRequested = std::move(handler);
}

/**
 * @brief 设置远程新建文件夹请求回调。
 * @param handler 接收远程文件夹绝对路径的回调。
 */
void FilePanel::setRemoteCreateDirectoryRequestedHandler(std::function<void(const QString &)> handler)
{
    m_remoteCreateDirectoryRequested = std::move(handler);
}

/**
 * @brief 设置远程新建文件请求回调。
 * @param handler 接收远程文件绝对路径的回调。
 */
void FilePanel::setRemoteCreateFileRequestedHandler(std::function<void(const QString &)> handler)
{
    m_remoteCreateFileRequested = std::move(handler);
}

/**
 * @brief 设置远程删除请求回调。
 * @param handler 接收远程项目路径的回调。
 */
void FilePanel::setRemoteRemoveRequestedHandler(std::function<void(const QString &, bool)> handler)
{
    m_remoteRemoveRequested = std::move(handler);
}

/**
 * @brief 设置远程重命名请求回调。
 * @param handler 接收源路径和目标路径的回调。
 */
void FilePanel::setRemoteRenameRequestedHandler(std::function<void(const QString &, const QString &)> handler)
{
    m_remoteRenameRequested = std::move(handler);
}

/**
 * @brief 设置远程权限修改请求回调。
 * @param handler 接收路径、权限模式和递归标志的回调。
 */
void FilePanel::setRemotePermissionsRequestedHandler(std::function<void(const QString &, int, bool)> handler)
{
    m_remotePermissionsRequested = std::move(handler);
}

/**
 * @brief 设置本地上传请求回调。
 * @param handler 接收本地路径的回调。
 */
void FilePanel::setLocalUploadRequestedHandler(std::function<void(const QString &)> handler)
{
    m_localUploadRequested = std::move(handler);
}

/**
 * @brief 设置远程下载请求回调。
 * @param handler 接收远程路径和是否目录的回调。
 */
void FilePanel::setRemoteDownloadRequestedHandler(std::function<void(const QString &, bool)> handler)
{
    m_remoteDownloadRequested = std::move(handler);
}

/**
 * @brief 设置远程文件外部编辑请求回调。
 * @param handler 接收远程文件绝对路径的回调。
 */
void FilePanel::setRemoteEditRequestedHandler(std::function<void(const QString &)> handler)
{
    m_remoteEditRequested = std::move(handler);
}

void FilePanel::setRemoteEditActiveQuery(std::function<bool(const QString &)> query)
{
    m_remoteEditActiveQuery = std::move(query);
}

void FilePanel::setRemoteEditCloseRequestedHandler(std::function<void(const QString &)> handler)
{
    m_remoteEditCloseRequested = std::move(handler);
}

/**
 * @brief 设置本地文件拖放到远程面板时的上传回调。
 * @param handler 接收本地路径列表的回调。
 */
void FilePanel::setLocalFilesDroppedOnRemoteHandler(std::function<void(const QStringList &)> handler)
{
    m_localFilesDroppedOnRemote = std::move(handler);
}

/**
 * @brief 设置远程文件拖放到本地面板时的下载回调。
 * @param handler 接收带文件类型的远程项目列表。
 */
void FilePanel::setRemoteFilesDroppedOnLocalHandler(std::function<void(const QList<RemoteTransferItem> &)> handler)
{
    m_remoteFilesDroppedOnLocal = std::move(handler);
}

/**
 * @brief 设置远程文件拖放到远程目录时的移动/复制回调。
 * @param handler 接收远程源路径列表和目标目录的回调。
 */
void FilePanel::setRemoteFilesDroppedOnRemoteHandler(std::function<void(const QStringList &, const QString &)> handler)
{
    m_remoteFilesDroppedOnRemote = std::move(handler);
}

/**
 * @brief 返回当前面板路径。
 * @return 本地目录或远程目录路径。
 */
QString FilePanel::currentPath() const
{
    return m_currentPath;
}

/**
 * @brief 显示远程依赖能力摘要。
 * @param curlVersion libcurl 版本。
 * @param hasFtp FTP 是否可用。
 * @param hasSftp SFTP 是否可用。
 */
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

/**
 * @brief 更新远程目录树已知目录集合。
 * @param directories 已知远程目录路径列表。
 */
void FilePanel::setRemoteKnownDirectories(const QStringList &directories)
{
    m_remoteKnownDirectories = directories;
    m_remoteKnownDirectories.removeDuplicates();
}

/**
 * @brief 将远程目录加载结果应用到表格与导航历史。
 * @param path 当前远程路径。
 * @param items 当前目录项目列表。
 * @param status 用户可见状态文本。
 * @param addToHistory 是否追加到导航历史。
 */
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

/**
 * @brief 将远程面板恢复到未连接状态。
 * @param status 用户可见状态文本。
 */
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

/**
 * @brief 展示远程目录加载错误状态。
 * @param status 用户可见错误文本。
 */
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

/**
 * @brief 在自动化测试中切换本地面板路径。
 * @param path 目标本地目录。
 */
void FilePanel::setLocalPathForTesting(const QString &path)
{
    if (m_mode != Mode::Local)
    {
        return;
    }

    navigateTo(path);
}

/**
 * @brief 构建文件面板工具栏、目录树、文件表格和状态栏。
 */
void FilePanel::setupUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(4);

    auto *toolbar = new QWidget(this);
    auto *toolbarLayout = new QHBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(0, 0, 0, 0);
    toolbarLayout->setSpacing(4);

    m_backButton = new QPushButton(toolbar);
    m_forwardButton = new QPushButton(toolbar);
    m_upButton = new QPushButton(toolbar);
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
    m_backButton->setIcon(fluentIcon("arrow_left"));
    m_forwardButton->setIcon(fluentIcon("arrow_right"));
    m_upButton->setIcon(fluentIcon("arrow_up"));
    m_refreshButton->setIcon(fluentIcon("arrow_sync"));

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
    m_table->setTextElideMode(Qt::ElideMiddle);
    m_table->horizontalHeader()->setSortIndicatorShown(m_mode == Mode::RemotePlaceholder);
    m_table->horizontalHeader()->setStretchLastSection(false);
    const QList<int> fileColumnWidths{175, 75, 80, 150, 80, 80};
    for (int column = 0; column < fileColumnWidths.size(); ++column)
    {
        m_table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Interactive);
        m_table->setColumnWidth(column, fileColumnWidths.at(column));
    }

    m_contentSplitter->addWidget(m_table);
    m_table->viewport()->installEventFilter(this);
    auto *renameShortcut = new QShortcut(QKeySequence(Qt::Key_F2), m_table);
    renameShortcut->setObjectName(objectPrefix + "RenameShortcut");
    renameShortcut->setContext(Qt::WidgetShortcut);
    connect(renameShortcut, &QShortcut::activated, this, [this]() {
        renameSelectedEntry();
    });
    m_contentSplitter->setStretchFactor(0, 0);
    m_contentSplitter->setStretchFactor(1, 1);
    m_contentSplitter->setSizes({220, 620});

    m_stateLabel = new QLabel(this);
    m_stateLabel->setObjectName(objectPrefix + "StateLabel");

    rootLayout->addWidget(toolbar);
    rootLayout->addWidget(m_contentSplitter, 1);
    rootLayout->addWidget(m_stateLabel);
}

/**
 * @brief 连接文件面板按钮、路径框、表格和目录树信号。
 */
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

/**
 * @brief 根据面板模式初始化本地目录或远程占位状态。
 */
void FilePanel::initialize()
{
    if (m_mode == Mode::Local)
    {
        navigateTo(desktopPath());
        return;
    }

    populateRemotePlaceholder();
}

/**
 * @brief 导航到指定本地或远程路径。
 * @param path 目标路径。
 * @param addToHistory 是否写入导航历史。
 */
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

/**
 * @brief 刷新当前本地目录或请求刷新远程目录。
 */
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

/**
 * @brief 导航到当前路径的上一级目录。
 */
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

/**
 * @brief 回退到上一条导航历史。
 */
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

/**
 * @brief 前进到下一条导航历史。
 */
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

/**
 * @brief 规范化远程路径并转发给外部远程加载回调。
 * @param path 用户输入或树节点提供的路径。
 * @param addToHistory 是否写入导航历史。
 */
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

/**
 * @brief 更新本地面板导航按钮可用状态。
 */
void FilePanel::updateNavigationButtons()
{
    m_backButton->setEnabled(m_historyIndex > 0);
    m_forwardButton->setEnabled(m_historyIndex + 1 < m_history.size());
    m_upButton->setEnabled(QDir(m_currentPath).cdUp());
}

/**
 * @brief 更新远程面板导航按钮和路径输入框可用状态。
 */
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
