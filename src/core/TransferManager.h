#ifndef DIRBRIDGE_CORE_TRANSFERMANAGER_H
#define DIRBRIDGE_CORE_TRANSFERMANAGER_H

#include "core/RemoteFileSystem.h"
#include "core/TransferQueue.h"

#include <cstddef>
#include <cstdint>
#include <functional>

class TransferManager
{
public:
    using QueueChangedCallback = std::function<void()>;
    using ProgressCallback = std::function<bool(const TransferJob &, std::int64_t, std::int64_t)>;
    using RemoteResolver = std::function<RemoteFileSystem *(const TransferJob &)>;

    /**
     * @brief 创建一个从传输队列调度任务的管理器。
     * @param remoteFileSystem 用于执行传输的已连接远程文件系统。
     * @param queue 包含等待任务和历史任务的队列。
     */
    TransferManager(RemoteFileSystem &remoteFileSystem, TransferQueue &queue);

    /**
     * @brief 创建一个可按任务解析远程文件系统的管理器。
     * @param remoteResolver 为每个传输任务返回后端实例的解析器。
     * @param queue 包含等待任务和历史任务的队列。
     */
    TransferManager(RemoteResolver remoteResolver, TransferQueue &queue);

    /**
     * @brief 设置任务状态变化时触发的回调。
     * @param callback 供 UI 适配层刷新展示状态使用的回调。
     */
    void setQueueChangedCallback(QueueChangedCallback callback);

    /**
     * @brief 设置活动文件传输上报字节进度时触发的回调。
     * @param callback 返回 false 时表示请求取消当前传输。
     */
    void setProgressCallback(ProgressCallback callback);

    /**
     * @brief 设置允许同时运行的最大任务数。
     * @param limit 正的并发上限；小于 1 的值会按 1 处理。
     */
    void setConcurrencyLimit(std::size_t limit);

    /**
     * @brief 在遵守并发上限的前提下执行等待中的任务。
     */
    void processPending();

private:
    RemoteOperationResult runJob(const TransferJob &job);
    void notifyQueueChanged() const;

private:
    RemoteResolver m_remoteResolver;
    TransferQueue &m_queue;
    QueueChangedCallback m_queueChangedCallback;
    ProgressCallback m_progressCallback;
    std::size_t m_concurrencyLimit = 1;
};

#endif // DIRBRIDGE_CORE_TRANSFERMANAGER_H
