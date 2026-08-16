#ifndef DIRBRIDGE_PLATFORM_WINDOWS_SHELLDATAOBJECT_H
#define DIRBRIDGE_PLATFORM_WINDOWS_SHELLDATAOBJECT_H

#include <functional>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <QByteArray>

struct WindowsShellDragFile
{
    std::wstring fileName;
    unsigned long long size = 0;
    bool isDirectory = false;
    // 回调负责生成临时路径并将远程内容写入该路径。
    std::function<bool(
        std::wstring &temporaryPath,
        const std::shared_ptr<std::atomic_bool> &canceled)> materialize;
};

using WindowsShellDragFileProvider = std::function<bool(std::vector<WindowsShellDragFile> &files)>;

/**
 * @brief 将远程文件作为 Windows Shell 虚拟文件拖出。
 * @param files Shell 文件描述列表。
 * @param applicationMetadata 可选的 DirBridge 自定义 MIME 数据，用于应用内拖放目标。
 * @param onDataObjectReleased 数据对象完全释放时调用的生命周期回调。
 * @return OLE 拖拽正常结束或由用户取消时返回 true。
 */
bool executeWindowsShellDrag(
    const std::vector<WindowsShellDragFile> &files,
    const QByteArray &applicationMetadata = {},
    std::function<void()> onDataObjectReleased = {});

/**
 * @brief 以延迟文件列表启动 Windows Shell 拖拽。
 * @param fileProvider 仅在 Shell 请求文件描述或内容时枚举远程目录。
 * @param applicationMetadata DirBridge 应用内拖放直接使用的自定义 MIME 数据。
 * @param onDataObjectReleased 数据对象完全释放时调用的生命周期回调。
 * @return OLE 拖拽正常结束或由用户取消时返回 true。
 */
bool executeWindowsShellDrag(
    WindowsShellDragFileProvider fileProvider,
    const QByteArray &applicationMetadata,
    std::function<void()> onDataObjectReleased = {});

/**
 * @brief 将虚拟文件列表放入 Windows 系统剪贴板。
 * @param files 供 Explorer 读取的文件和目录描述。
 * @param applicationMetadata DirBridge 自定义 MIME 的 JSON 数据。
 * @param onDataObjectReleased 数据对象完全释放时调用的生命周期回调。
 * @return 系统剪贴板接收数据对象时返回 true。
 */
bool setWindowsShellClipboard(
    const std::vector<WindowsShellDragFile> &files,
    const QByteArray &applicationMetadata,
    std::function<void()> onDataObjectReleased = {});

/**
 * @brief 将带延迟文件枚举的数据对象放入 Windows 系统剪贴板，供自动化验证和延迟消费使用。
 */
bool setWindowsShellClipboard(
    WindowsShellDragFileProvider fileProvider,
    const QByteArray &applicationMetadata,
    std::function<void()> onDataObjectReleased = {});

/**
 * @brief 清除由 DirBridge 设置的系统剪贴板对象。
 */
void clearWindowsShellClipboard();

#endif // DIRBRIDGE_PLATFORM_WINDOWS_SHELLDATAOBJECT_H
