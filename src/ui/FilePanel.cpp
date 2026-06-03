#include "ui/FilePanel.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace
{
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
    if (!desktop.isEmpty())
    {
        return desktop;
    }

    return QDir::homePath();
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

    m_backButton->setToolTip("后退");
    m_forwardButton->setToolTip("前进");
    m_upButton->setToolTip("上一级");
    m_refreshButton->setToolTip("刷新当前目录");

    toolbarLayout->addWidget(m_backButton);
    toolbarLayout->addWidget(m_forwardButton);
    toolbarLayout->addWidget(m_upButton);
    toolbarLayout->addWidget(m_pathEdit, 1);
    toolbarLayout->addWidget(m_refreshButton);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({"名称", "大小", "类型", "修改时间", "权限", "所有者"});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    m_table->setSortingEnabled(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < m_table->columnCount(); ++column)
    {
        m_table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }

    m_stateLabel = new QLabel(this);

    rootLayout->addWidget(toolbar);
    rootLayout->addWidget(m_table, 1);
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
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (m_mode != Mode::Local)
        {
            return;
        }

        QTableWidgetItem *nameItem = m_table->item(row, 0);
        if (nameItem == nullptr)
        {
            return;
        }

        const QString path = nameItem->data(Qt::UserRole).toString();
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

void FilePanel::navigateTo(const QString &path, bool addToHistory)
{
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
    updateNavigationButtons();
}

void FilePanel::refresh()
{
    if (m_mode == Mode::Local)
    {
        populateLocalDirectory(m_currentPath);
        return;
    }

    populateRemotePlaceholder();
}

void FilePanel::navigateUp()
{
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
    navigateTo(m_history.at(m_historyIndex), false);
}

void FilePanel::navigateForward()
{
    if (m_historyIndex + 1 >= m_history.size())
    {
        return;
    }

    ++m_historyIndex;
    navigateTo(m_history.at(m_historyIndex), false);
}

void FilePanel::updateNavigationButtons()
{
    m_backButton->setEnabled(m_historyIndex > 0);
    m_forwardButton->setEnabled(m_historyIndex + 1 < m_history.size());
    m_upButton->setEnabled(QDir(m_currentPath).cdUp());
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
    m_table->setSortingEnabled(true);
    m_stateLabel->setText("远程面板占位，后续接入 FTP/SFTP 会话。");
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
