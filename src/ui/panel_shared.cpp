#include "ui/panel_shared.h"
#include "ui/FilePanel.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QStandardPaths>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>
#include <QSet>

#include <algorithm>

namespace panel_shared
{
namespace
{
constexpr int maxRemoteTransferPayloadBytes = 1024 * 1024;
constexpr int maxRemoteTransferItems = 4096;
constexpr int maxRemoteTransferStringLength = 32768;

bool hasControlCharacter(const QString &value)
{
    return std::any_of(value.cbegin(), value.cend(), [](QChar character) {
        return character.unicode() < 0x20;
    });
}

bool isValidTransferMetadataString(const QString &value, bool allowEmpty)
{
    return (allowEmpty || !value.isEmpty())
        && value.size() <= maxRemoteTransferStringLength
        && !hasControlCharacter(value);
}
}

QByteArray encodeRemoteTransferItems(const QList<RemoteTransferItem> &items)
{
    QJsonObject payload;
    payload.insert("version", 1);
    payload.insert("sessionId", items.isEmpty() ? QString() : items.constFirst().sessionId);
    QJsonArray array;
    for (const RemoteTransferItem &item : items)
    {
        QJsonObject object;
        object.insert("path", item.path);
        object.insert("sessionId", item.sessionId);
        object.insert("name", item.name);
        if (item.size >= 0)
        {
            object.insert("size", item.size);
        }
        object.insert("isDirectory", item.isDirectory);
        array.append(object);
    }
    payload.insert("items", array);
    return QJsonDocument(payload).toJson(QJsonDocument::Compact);
}

QList<RemoteTransferItem> decodeRemoteTransferItems(const QByteArray &payload)
{
    QList<RemoteTransferItem> items;
    if (payload.isEmpty() || payload.size() > maxRemoteTransferPayloadBytes)
    {
        return items;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || (!document.isArray() && !document.isObject()))
    {
        return items;
    }

    QJsonArray array;
    QString payloadSessionId;
    if (document.isArray())
    {
        array = document.array();
    }
    else
    {
        const QJsonObject object = document.object();
        if (object.value("version").toInt() != 1 || !object.value("items").isArray())
        {
            return items;
        }
        payloadSessionId = object.value("sessionId").toString();
        if (!isValidTransferMetadataString(payloadSessionId, false))
        {
            return items;
        }
        array = object.value("items").toArray();
    }

    if (array.isEmpty() || array.size() > maxRemoteTransferItems)
    {
        return items;
    }

    QSet<QString> seenPaths;
    for (const QJsonValue &value : array)
    {
        if (!value.isObject())
        {
            continue;
        }
        const QJsonObject object = value.toObject();
        RemoteTransferItem item;
        item.path = object.value("path").toString();
        item.sessionId = object.value("sessionId").toString(payloadSessionId);
        item.name = object.value("name").toString();
        item.size = object.value("size").toInteger(-1);
        item.isDirectory = object.value("isDirectory").toBool();
        const bool versionedPayload = document.isObject();
        if ((versionedPayload && item.sessionId != payloadSessionId)
            || !isValidTransferMetadataString(item.path, false)
            || !isValidTransferMetadataString(item.sessionId, !versionedPayload)
            || !isValidTransferMetadataString(item.name, !versionedPayload)
            || item.size < -1
            || seenPaths.contains(item.path))
        {
            if (versionedPayload)
            {
                return {};
            }
            continue;
        }
        seenPaths.insert(item.path);
        items.append(item);
    }
    return items;
}

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

QString promptConflictRename(
    QWidget *parent,
    const QString &title,
    const QString &targetPath,
    const QString &originalName,
    bool isDirectory)
{
    QDialog dialog(parent);
    dialog.setObjectName("conflictRenameDialog");
    dialog.setWindowTitle(title);
    dialog.setMinimumWidth(520);
    dialog.resize(560, 260);

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(14);

    auto *summaryLayout = new QHBoxLayout();
    summaryLayout->setSpacing(12);
    auto *iconLabel = new QLabel(&dialog);
    iconLabel->setPixmap(dialog.style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(32, 32));
    iconLabel->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    summaryLayout->addWidget(iconLabel, 0, Qt::AlignTop);

    auto *summaryTextLayout = new QVBoxLayout();
    summaryTextLayout->setSpacing(4);
    auto *headingLabel = new QLabel(
        QString("目标位置已存在同名%1").arg(isDirectory ? "文件夹" : "文件"),
        &dialog);
    QFont headingFont = headingLabel->font();
    headingFont.setBold(true);
    headingFont.setPointSizeF(headingFont.pointSizeF() + 1.5);
    headingLabel->setFont(headingFont);
    summaryTextLayout->addWidget(headingLabel);

    auto *descriptionLabel = new QLabel("请输入新名称后继续，现有项目不会被覆盖或合并。", &dialog);
    descriptionLabel->setWordWrap(true);
    summaryTextLayout->addWidget(descriptionLabel);
    summaryLayout->addLayout(summaryTextLayout, 1);
    layout->addLayout(summaryLayout);

    auto *pathLayout = new QVBoxLayout();
    pathLayout->setSpacing(6);
    auto *pathCaption = new QLabel("目标位置", &dialog);
    pathLayout->addWidget(pathCaption);

    auto *pathLabel = new QLabel(targetPath, &dialog);
    pathLabel->setObjectName("conflictTargetPath");
    pathLabel->setTextFormat(Qt::PlainText);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathLabel->setWordWrap(true);
    pathLabel->setFrameShape(QFrame::StyledPanel);
    pathLabel->setMargin(9);
    pathLayout->addWidget(pathLabel);
    layout->addLayout(pathLayout);

    auto *nameLayout = new QVBoxLayout();
    nameLayout->setSpacing(6);
    auto *nameCaption = new QLabel("新名称", &dialog);
    nameLayout->addWidget(nameCaption);

    auto *nameEdit = new QLineEdit(originalName, &dialog);
    nameEdit->setObjectName("conflictRenameEdit");
    nameEdit->setMinimumHeight(30);
    nameEdit->selectAll();
    nameLayout->addWidget(nameEdit);
    layout->addLayout(nameLayout);

    auto *buttons = new QDialogButtonBox(&dialog);
    auto *confirmButton = buttons->addButton("重命名后继续", QDialogButtonBox::AcceptRole);
    buttons->addButton("取消", QDialogButtonBox::RejectRole);
    confirmButton->setMinimumWidth(128);
    confirmButton->setDefault(true);
    layout->addWidget(buttons);

    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    nameEdit->setFocus();
    return dialog.exec() == QDialog::Accepted ? nameEdit->text().trimmed() : QString();
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

QDateTime parseRemoteModifiedTime(const QString &value)
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
    return parsed;
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
