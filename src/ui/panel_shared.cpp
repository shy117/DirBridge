#include "ui/panel_shared.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

namespace panel_shared
{
QIcon fluentIcon(const QString &name)
{
    return QIcon(QString(":/icons/fluent/%1_24_regular.svg").arg(name));
}

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

void showInformationDialog(QWidget *parent, const QString &title, const QString &message)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);

    auto *layout = new QVBoxLayout(&dialog);
    auto *label = new QLabel(message, &dialog);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setWordWrap(true);
    layout->addWidget(label);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText("关闭");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
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
