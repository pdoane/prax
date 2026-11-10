// The Windows backend of the environment.

#include "pr/core_windows_environment.h"

#include "pr/core_windows_string.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

namespace pr_impl {

namespace {

// Returns `count` UTF-16 units as UTF-8, or the empty string if they are not valid UTF-16.
PrRawString narrow(const wchar_t* wide, uint32_t count) {
    uint32_t size = prCoreWindowsNarrow(wide, count, nullptr, 0, PrCoreWindowsString_Exact);
    char* utf8 = size != 0 && size != kPrCoreWindowsStringInvalid ? static_cast<char*>(malloc(size)) : nullptr;
    PrRawString value = prRawStringEmpty();
    if (utf8 != nullptr && prCoreWindowsNarrow(wide, count, utf8, size, PrCoreWindowsString_Exact) == size)
        value = prRawStringCopy(PrStringView{utf8, size});
    free(utf8);
    return value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrRawString prCoreWindowsEnvironmentVariable(PrStringView name) {
    uint32_t nameCount = prCoreWindowsWiden(name, nullptr, 0, PrCoreWindowsString_Exact);
    if (nameCount == 0 || nameCount == kPrCoreWindowsStringInvalid)
        return prRawStringEmpty();
    wchar_t* wideName = static_cast<wchar_t*>(malloc((size_t(nameCount) + 1) * sizeof(wchar_t)));
    if (wideName == nullptr)
        return prRawStringEmpty();
    prCoreWindowsWiden(name, wideName, nameCount, PrCoreWindowsString_Exact);
    wideName[nameCount] = 0;

    // The first call measures the value with its terminator, and the second reads it. The value can change between
    // them, and a longer one is not read.
    PrRawString value = prRawStringEmpty();
    DWORD count = GetEnvironmentVariableW(wideName, nullptr, 0);
    wchar_t* wide = count > 1 ? static_cast<wchar_t*>(malloc(size_t(count) * sizeof(wchar_t))) : nullptr;
    DWORD read = wide != nullptr ? GetEnvironmentVariableW(wideName, wide, count) : 0;
    if (read != 0 && read < count)
        value = narrow(wide, uint32_t(read));
    free(wide);
    free(wideName);
    return value;
}

PrRawString prCoreWindowsEnvironmentHostName(void) {
    // The first call measures the name with its terminator, and the second reads it.
    DWORD count = 0;
    GetComputerNameExW(ComputerNameDnsHostname, nullptr, &count);
    if (count == 0)
        return prRawStringEmpty();
    wchar_t* wide = static_cast<wchar_t*>(malloc(size_t(count) * sizeof(wchar_t)));
    PrRawString value = prRawStringEmpty();
    if (wide != nullptr && GetComputerNameExW(ComputerNameDnsHostname, wide, &count) && count != 0)
        value = narrow(wide, uint32_t(count));
    free(wide);
    return value;
}
