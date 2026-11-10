// The conversions between UTF-8 and the UTF-16 of Windows that the Win32 backend makes, over core's.

#include "app/backends/win32/src/win32_text.h"

#include "pr/core_windows_string.h"

namespace pr_impl {

void win32Widen(PrStringView text, RawArray<wchar_t>& out) {
    uint32_t count = prCoreWindowsWiden(text, nullptr, 0, PrCoreWindowsString_Substitute);
    out.resize(count + 1);
    prCoreWindowsWiden(text, out.data, count, PrCoreWindowsString_Substitute);
    out.data[count] = 0;
}

uint32_t win32Utf8Length(const wchar_t* units, int count) {
    return count <= 0 ? 0 : prCoreWindowsNarrow(units, uint32_t(count), nullptr, 0, PrCoreWindowsString_Substitute);
}

PrStringView win32Narrow(const wchar_t* units, int count, RawArray<char>& out) {
    uint32_t size = win32Utf8Length(units, count);
    if (size == 0)
        return "";
    out.resize(size);
    prCoreWindowsNarrow(units, uint32_t(count), out.data, size, PrCoreWindowsString_Substitute);
    return PrStringView{out.data, size};
}

} // namespace pr_impl
