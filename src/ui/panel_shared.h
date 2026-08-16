#ifndef DIRBRIDGE_UI_PANEL_SHARED_H
#define DIRBRIDGE_UI_PANEL_SHARED_H

#include <QDateTime>
#include <QByteArray>
#include <QIcon>
#include <QList>
#include <QString>

#include <vector>

#include "core/FileItem.h"

class QFileIconProvider;
class QWidget;
struct RemoteTransferItem;

namespace panel_shared
{
inline constexpr const char *RemotePathMimeType = "application/x-dirbridge-remote-paths";
inline constexpr const char *LocalPathMimeType = "application/x-dirbridge-local-paths";
inline constexpr int FileOwnerRole = Qt::UserRole + 2;

QByteArray encodeRemoteTransferItems(const QList<RemoteTransferItem> &items);
QList<RemoteTransferItem> decodeRemoteTransferItems(const QByteArray &payload);

/**
 * @brief 从 Qt 资源中加载 Fluent UI SVG 图标。
 * @param name 不包含 _24_regular 后缀的图标基础名称。
 * @return 对应资源图标。
 */
QIcon fluentIcon(const QString &name);

/**
 * @brief 将远程文件项目类型转换为用户可见中文文本。
 * @param type 文件项目类型。
 * @return 类型中文文本。
 */
QString fileItemTypeText(FileItemType type);

/**
 * @brief 显示支持复制文本的信息对话框。
 * @param parent 父窗口。
 * @param title 对话框标题。
 * @param message 对话框正文。
 */
void showInformationDialog(QWidget *parent, const QString &title, const QString &message);

/**
 * @brief 显示统一的重名处理对话框。
 * @param parent 父窗口。
 * @param title 对话框标题。
 * @param targetPath 已存在同名项目的完整目标路径。
 * @param originalName 原始名称。
 * @param isDirectory 冲突项目是否为文件夹。
 * @return 用户确认的新名称；取消时返回空字符串。
 */
QString promptConflictRename(
    QWidget *parent,
    const QString &title,
    const QString &targetPath,
    const QString &originalName,
    bool isDirectory);

/**
 * @brief 将字节数格式化为适合文件列表展示的大小文本。
 * @param size 字节数；负数表示未知大小。
 * @return 格式化后的大小文本。
 */
QString formatFileSize(qint64 size);

/**
 * @brief 解析远程目录列表中的修改时间。
 * @param value ISO、本地标准格式或 Unix LIST 格式时间文本。
 * @return 可解析的时间；格式未知时返回无效时间。
 */
QDateTime parseRemoteModifiedTime(const QString &value);

/**
 * @brief 取得默认本地起始目录。
 * @return 桌面目录；不可用时返回用户主目录。
 */
QString desktopPath();

/**
 * @brief 根据远程项目类型选择表格图标。
 * @param widget 用于读取平台标准图标的控件。
 * @param iconProvider 本地文件图标提供器。
 * @param name 文件名。
 * @param type 文件项目类型。
 * @return 适合展示的图标。
 */
QIcon iconForFileItem(QWidget *widget, const QFileIconProvider &iconProvider, const QString &name, FileItemType type);

/**
 * @brief 校验远程或本地新建名称是否可作为单级文件名。
 * @param name 待校验名称。
 * @return 名称有效时返回 true。
 */
bool isValidRemoteName(const QString &name);

/**
 * @brief 显示名称无效警告。
 * @param parent 父窗口。
 */
void showInvalidRemoteNameWarning(QWidget *parent);

/**
 * @brief 显示文件操作失败警告。
 * @param parent 父窗口。
 * @param title 警告标题。
 * @param message 警告正文。
 */
void showFileOperationWarning(QWidget *parent, const QString &title, const QString &message);

/**
 * @brief 按远程文件表格列规则排序项目列表。
 * @param items 原始远程项目列表。
 * @param column 排序列索引。
 * @param order 排序方向。
 * @return 排序后的远程项目列表。
 */
std::vector<FileItem> sortedRemoteItems(const std::vector<FileItem> &items, int column, Qt::SortOrder order);
}

#endif // DIRBRIDGE_UI_PANEL_SHARED_H
