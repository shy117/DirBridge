#include "config/PasswordCrypto.h"

#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>
#endif

namespace
{
#ifdef _WIN32
DATA_BLOB makeBlob(const std::string &value)
{
    DATA_BLOB blob{};
    blob.pbData = reinterpret_cast<BYTE *>(const_cast<char *>(value.data()));
    blob.cbData = static_cast<DWORD>(value.size());
    return blob;
}

std::string base64Encode(const BYTE *data, DWORD size)
{
    if (size == 0)
    {
        return {};
    }

    DWORD outputSize = 0;
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &outputSize))
    {
        throw std::runtime_error("Failed to size encrypted password encoding");
    }

    std::string output(outputSize, '\0');
    if (!CryptBinaryToStringA(data, size, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, output.data(), &outputSize))
    {
        throw std::runtime_error("Failed to encode encrypted password");
    }
    while (!output.empty() && output.back() == '\0')
    {
        output.pop_back();
    }
    return output;
}

std::vector<BYTE> base64Decode(const std::string &value)
{
    if (value.empty())
    {
        return {};
    }

    DWORD outputSize = 0;
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, nullptr, &outputSize, nullptr, nullptr))
    {
        throw std::runtime_error("Failed to size encrypted password decoding");
    }

    std::vector<BYTE> output(outputSize);
    if (!CryptStringToBinaryA(value.c_str(), static_cast<DWORD>(value.size()), CRYPT_STRING_BASE64, output.data(), &outputSize, nullptr, nullptr))
    {
        throw std::runtime_error("Failed to decode encrypted password");
    }
    output.resize(outputSize);
    return output;
}
#endif
}

std::string passwordStorageScheme()
{
#ifdef _WIN32
    return "windows-dpapi-current-user";
#else
    return "plain-text";
#endif
}

std::string protectPassword(const std::string &password)
{
    if (password.empty())
    {
        return {};
    }

#ifdef _WIN32
    DATA_BLOB input = makeBlob(password);
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"DirBridge site password", nullptr, nullptr, nullptr, 0, &output))
    {
        throw std::runtime_error("Failed to protect site password");
    }

    try
    {
        const std::string encoded = base64Encode(output.pbData, output.cbData);
        LocalFree(output.pbData);
        return encoded;
    }
    catch (...)
    {
        LocalFree(output.pbData);
        throw;
    }
#else
    return password;
#endif
}

std::string unprotectPassword(const std::string &protectedPassword)
{
    if (protectedPassword.empty())
    {
        return {};
    }

#ifdef _WIN32
    std::vector<BYTE> encryptedBytes = base64Decode(protectedPassword);
    DATA_BLOB input{};
    input.pbData = encryptedBytes.data();
    input.cbData = static_cast<DWORD>(encryptedBytes.size());

    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output))
    {
        throw std::runtime_error("Failed to unprotect site password");
    }

    std::string password(reinterpret_cast<char *>(output.pbData), reinterpret_cast<char *>(output.pbData) + output.cbData);
    LocalFree(output.pbData);
    return password;
#else
    return protectedPassword;
#endif
}
