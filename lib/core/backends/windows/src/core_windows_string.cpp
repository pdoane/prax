// The conversions between UTF-8 and the UTF-16 strings that Windows' wide-character functions take.

#include "pr/core_windows_string.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

uint32_t prCoreWindowsWiden(PrStringView text, wchar_t* PR_NULLABLE out, uint32_t capacity, PrCoreWindowsString string) {
    bool fail = string == PrCoreWindowsString_Exact;
    if (text.size == 0)
        return 0;
    if (text.size > size_t(INT32_MAX))
        return fail ? uint32_t(kPrCoreWindowsStringInvalid) : 0;
    DWORD flags = fail ? MB_ERR_INVALID_CHARS : 0;
    int count = MultiByteToWideChar(CP_UTF8, flags, text.data, int(text.size), nullptr, 0);
    if (count == 0)
        return fail ? uint32_t(kPrCoreWindowsStringInvalid) : 0;
    if (out != nullptr && capacity >= uint32_t(count))
        MultiByteToWideChar(CP_UTF8, flags, text.data, int(text.size), out, count);
    return uint32_t(count);
}

uint32_t prCoreWindowsNarrow(const wchar_t* PR_NULLABLE units, uint32_t count, char* PR_NULLABLE out, uint32_t capacity,
        PrCoreWindowsString string) {
    bool fail = string == PrCoreWindowsString_Exact;
    if (count == 0 || units == nullptr)
        return 0;
    if (count > uint32_t(INT32_MAX))
        return fail ? uint32_t(kPrCoreWindowsStringInvalid) : 0;
    DWORD flags = fail ? WC_ERR_INVALID_CHARS : 0;
    int size = WideCharToMultiByte(CP_UTF8, flags, units, int(count), nullptr, 0, nullptr, nullptr);
    if (size == 0)
        return fail ? uint32_t(kPrCoreWindowsStringInvalid) : 0;
    if (out != nullptr && capacity >= uint32_t(size))
        WideCharToMultiByte(CP_UTF8, flags, units, int(count), out, size, nullptr, nullptr);
    return uint32_t(size);
}
