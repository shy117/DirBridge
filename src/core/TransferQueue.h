#ifndef DIRBRIDGE_CORE_TRANSFERQUEUE_H
#define DIRBRIDGE_CORE_TRANSFERQUEUE_H

#include "core/TransferJob.h"

#include <cstddef>
#include <string>
#include <vector>

class TransferQueue
{
public:
    /**
     * @brief 将传输任务追加到队列末尾。
     * @param job 要入队的任务定义。
     * @return 入队后的任务快照引用。
     */
    const TransferJob &enqueue(TransferJob job);

    /**
     * @brief 用相同 id 的新快照替换已有任务。
     * @param job 更新后的任务快照。
     * @return 成功更新已有任务时返回 true。
     */
    bool update(const TransferJob &job);

    /**
     * @brief 将等待中或运行中的任务标记为已取消。
     * @param id 要取消的任务 id。
     * @param message 可选的取消原因。
     * @return 任务存在且允许取消时返回 true。
     */
    bool cancel(const std::string &id, const std::string &message = {});

    /**
     * @brief 为失败或已取消的任务创建重试副本并重新入队。
     * @param id 原始任务 id。
     * @param retryId 新重试任务的唯一 id。
     * @return 新等待任务的指针；不允许重试时返回 nullptr。
     */
    const TransferJob *retry(const std::string &id, const std::string &retryId);

    /**
     * @brief 从历史记录中移除已完成、失败和已取消的任务。
     * @return 被移除的任务数量。
     */
    std::size_t clearFinished();

    /**
     * @brief 按 id 查找可修改的任务。
     * @param id 要查找的任务 id。
     * @return 找到时返回任务指针；否则返回 nullptr。
     */
    TransferJob *find(const std::string &id);

    /**
     * @brief 按 id 查找只读任务。
     * @param id 要查找的任务 id。
     * @return 找到时返回任务指针；否则返回 nullptr。
     */
    const TransferJob *find(const std::string &id) const;

    /**
     * @brief 查找下一个等待执行的任务。
     * @return 下一个等待任务的指针；队列空闲时返回 nullptr。
     */
    TransferJob *nextPending();

    /**
     * @brief 统计当前处于运行状态的可执行文件任务数量。
     * @return 正在运行的文件任务数。
     */
    std::size_t runningCount() const;

    /**
     * @brief 按展示顺序返回全部任务。
     * @return 按插入顺序排列的任务快照集合。
     */
    const std::vector<TransferJob> &jobs() const;

    /**
     * @brief 清空队列中的全部任务。
     */
    void clear();

private:
    std::vector<TransferJob> m_jobs;
};

#endif // DIRBRIDGE_CORE_TRANSFERQUEUE_H
