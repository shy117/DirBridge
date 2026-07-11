#ifndef DIRBRIDGE_APP_UISMOKETESTS_H
#define DIRBRIDGE_APP_UISMOKETESTS_H

#include <QString>

class MainWindow;

/**
 * @brief 验证远程 UI 对象连线和默认状态是否正确。
 * @param window 待测试的主窗口。
 * @return 界面对象存在且初始化正确时返回 true。
 */
bool checkRemoteUiObjects(MainWindow &window);

/**
 * @brief 运行基于假后端的远程 UI 工作流冒烟测试。
 * @param window 待测试的主窗口。
 * @return 假后端工作流通过时返回 true。
 */
bool checkRemoteUiWorkflow(MainWindow &window);

/**
 * @brief 运行真实远程连接与导航冒烟测试。
 * @param window 待测试的主窗口。
 * @return 真实工作流通过时返回 true。
 */
bool checkLiveRemoteUiWorkflow(MainWindow &window);

/**
 * @brief 使用当前配置中已保存的站点运行真实连接冒烟测试。
 * @param window 待测试的主窗口。
 * @param siteName 已保存站点的显示名称。
 * @return 站点连接并成功加载远程目录时返回 true。
 */
bool checkSavedSiteRemoteUiWorkflow(MainWindow &window, const QString &siteName);

/**
 * @brief 运行真实远程刷新冒烟测试。
 * @param window 待测试的主窗口。
 * @return 刷新能够检测到服务端外部变化时返回 true。
 */
bool checkLiveRemoteRefreshWorkflow(MainWindow &window);

/**
 * @brief 运行真实上传/下载传输 UI 冒烟测试。
 * @param window 待测试的主窗口。
 * @return 传输 UI 工作流通过时返回 true。
 */
bool checkLiveRemoteTransferWorkflow(MainWindow &window);

#endif // DIRBRIDGE_APP_UISMOKETESTS_H
