#ifndef DIRBRIDGE_CONFIG_PASSWORDCRYPTO_H
#define DIRBRIDGE_CONFIG_PASSWORDCRYPTO_H

#include <string>

/**
 * @brief Returns the persisted password storage scheme used by this build.
 * @return Stable storage scheme name written to sites.json.
 */
std::string passwordStorageScheme();

/**
 * @brief Encrypts a site password for local persistence.
 * @param password Plain password kept only in memory while the app is running.
 * @return Encoded encrypted password suitable for JSON storage.
 */
std::string protectPassword(const std::string &password);

/**
 * @brief Decrypts a site password loaded from local persistence.
 * @param protectedPassword Encoded encrypted password from JSON storage.
 * @return Plain password for runtime connection use.
 */
std::string unprotectPassword(const std::string &protectedPassword);

#endif // DIRBRIDGE_CONFIG_PASSWORDCRYPTO_H
