// The Windows backend of the environment.

#include "pr/core_windows_environment.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

PrRawString prCoreWindowsEnvironmentVariable(PrStringView name) {
    int nameCount = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data, int(name.size), nullptr, 0);
    if (nameCount == 0)
        return prRawStringEmpty();
    wchar_t* wideName = static_cast<wchar_t*>(malloc((size_t(nameCount) + 1) * sizeof(wchar_t)));
    if (wideName == nullptr)
        return prRawStringEmpty();
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data, int(name.size), wideName, nameCount);
    wideName[nameCount] = 0;

    // The first call measures the value with its terminator, and the second reads it. The value can change between
    // them, and a longer one is not read.
    PrRawString value = prRawStringEmpty();
    DWORD count = GetEnvironmentVariableW(wideName, nullptr, 0);
    wchar_t* wide = count > 1 ? static_cast<wchar_t*>(malloc(size_t(count) * sizeof(wchar_t))) : nullptr;
    DWORD read = wide != nullptr ? GetEnvironmentVariableW(wideName, wide, count) : 0;
    if (read != 0 && read < count) {
        int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, int(read), nullptr, 0, nullptr, nullptr);
        char* utf8 = size > 0 ? static_cast<char*>(malloc(size_t(size))) : nullptr;
        if (utf8 != nullptr &&
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, int(read), utf8, size, nullptr, nullptr) == size)
            value = prRawStringCopy(PrStringView{utf8, size_t(size)});
        free(utf8);
    }
    free(wide);
    free(wideName);
    return value;
}
