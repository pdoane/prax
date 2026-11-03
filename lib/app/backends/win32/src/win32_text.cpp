// The conversions between UTF-8 and the UTF-16 of Windows that the Win32 backend makes.

#include "app/backends/win32/src/win32_text.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace pr_impl {

void win32Widen(PrStringView text, RawArray<wchar_t>& out) {
    int count;
    if (text.size == 0 || text.size > size_t(INT32_MAX))
        count = 0;
    else
        count = MultiByteToWideChar(CP_UTF8, 0, text.data, int(text.size), nullptr, 0);
    out.resize(uint32_t(count) + 1);
    if (count != 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data, int(text.size), out.data, count);
    out.data[count] = 0;
}

uint32_t win32Utf8Length(const wchar_t* units, int count) {
    return count <= 0 ? 0 : uint32_t(WideCharToMultiByte(CP_UTF8, 0, units, count, nullptr, 0, nullptr, nullptr));
}

PrStringView win32Narrow(const wchar_t* units, int count, RawArray<char>& out) {
    uint32_t size = win32Utf8Length(units, count);
    if (size == 0)
        return "";
    out.resize(size);
    WideCharToMultiByte(CP_UTF8, 0, units, count, out.data, int(size), nullptr, nullptr);
    return PrStringView{out.data, size};
}

} // namespace pr_impl
