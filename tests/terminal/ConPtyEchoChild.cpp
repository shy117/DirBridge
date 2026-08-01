#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string_view>

int main()
{
    HANDLE output = CreateFileW(
        L"CONOUT$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (output == INVALID_HANDLE_VALUE)
    {
        return 2;
    }
    constexpr std::string_view Marker = "DIRBRIDGE_CONPTY_PRODUCTION_OK\r\n";
    DWORD written = 0;
    const bool ok = WriteFile(
        output,
        Marker.data(),
        static_cast<DWORD>(Marker.size()),
        &written,
        nullptr) != FALSE
        && written == Marker.size();
    CloseHandle(output);
    return ok ? 0 : 3;
}
