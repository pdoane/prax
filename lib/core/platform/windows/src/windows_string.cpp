// The conversions between UTF-8 and the UTF-16 of Windows' wide-character functions.

#include "pr/windows_string.h"

#include "pr/memory.h"
#include "pr/raw_array.h"

#include "core/src/raw_string.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace pr_impl;

uint32_t prWindowsWiden(PrStringView text, wchar_t* PR_NULLABLE out, uint32_t capacity) {
    if (text.size == 0)
        return 0;
    if (text.size > size_t(INT32_MAX))
        return kPrWindowsStringInvalid;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data, int(text.size), nullptr, 0);
    if (count == 0)
        return kPrWindowsStringInvalid;
    if (out != nullptr && capacity >= uint32_t(count))
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data, int(text.size), out, count);
    return uint32_t(count);
}

uint32_t prWindowsNarrow(const wchar_t* PR_NULLABLE units, uint32_t count, char* PR_NULLABLE out, uint32_t capacity) {
    if (count == 0 || units == nullptr)
        return 0;
    if (count > uint32_t(INT32_MAX))
        return kPrWindowsStringInvalid;
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, units, int(count), nullptr, 0, nullptr, nullptr);
    if (size == 0)
        return kPrWindowsStringInvalid;
    if (out != nullptr && capacity >= uint32_t(size))
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, units, int(count), out, size, nullptr, nullptr);
    return uint32_t(size);
}

wchar_t* PR_NULLABLE prWindowsWidenCopy(PrStringView text, uint32_t* PR_NULLABLE outCount) {
    uint32_t count = prWindowsWiden(text, nullptr, 0);
    if (count == kPrWindowsStringInvalid)
        return nullptr;
    wchar_t* wide = static_cast<wchar_t*>(prMemoryAlloc((size_t(count) + 1) * sizeof(wchar_t)));
    prWindowsWiden(text, wide, count);
    wide[count] = 0;
    if (outCount != nullptr)
        *outCount = count;
    return wide;
}

PrRawString prWindowsNarrowToRawString(const wchar_t* PR_NULLABLE units, uint32_t count) {
    uint32_t size = prWindowsNarrow(units, count, nullptr, 0);
    if (size == kPrWindowsStringInvalid)
        return prRawStringNull();
    if (size == 0)
        return prRawStringEmpty();
    char* data = static_cast<char*>(prMemoryAlloc(size));
    prWindowsNarrow(units, count, data, size);
    return rawStringAdopt(data, size, size);
}

namespace pr {

bool windowsWiden(PrStringView text, RawArray<wchar_t>& out) {
    uint32_t count = prWindowsWiden(text, nullptr, 0);
    bool valid = count != kPrWindowsStringInvalid;
    if (!valid)
        count = 0;
    out.resize(count + 1);
    if (count != 0)
        prWindowsWiden(text, out.data, count);
    out.data[count] = 0;
    return valid;
}

PrStringView windowsNarrow(const wchar_t* PR_NULLABLE units, uint32_t count, RawArray<char>& out) {
    uint32_t size = prWindowsNarrow(units, count, nullptr, 0);
    if (size == 0 || size == kPrWindowsStringInvalid)
        return "";
    out.resize(size);
    prWindowsNarrow(units, count, out.data, size);
    return PrStringView{out.data, size};
}

} // namespace pr
