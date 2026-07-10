#ifndef DIRBRIDGE_CONFIG_PASSWORDCRYPTO_H
#define DIRBRIDGE_CONFIG_PASSWORDCRYPTO_H

#include <string>

/**
 * @brief 返回当前构建使用的持久化密码存储方案名称。
 * @return 写入 `sites.json` 的稳定存储方案标识。
 */
std::string passwordStorageScheme();

/**
 * @brief 对站点密码进行加密，以便本地持久化保存。
 * @param password 应用运行期间仅保存在内存中的明文密码。
 * @return 适合写入 JSON 的已编码加密密码。
 */
std::string protectPassword(const std::string &password);

/**
 * @brief 解密从本地持久化存储中读取的站点密码。
 * @param protectedPassword 从 JSON 存储中读取的已编码加密密码。
 * @return 供运行时连接使用的明文密码。
 */
std::string unprotectPassword(const std::string &protectedPassword);

#endif // DIRBRIDGE_CONFIG_PASSWORDCRYPTO_H
