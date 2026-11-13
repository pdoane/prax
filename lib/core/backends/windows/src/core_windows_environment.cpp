// The Windows backend of the environment.

#include "pr/core_windows_environment.h"

#include "pr/memory.h"
#include "pr/windows_string.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

namespace pr_impl {

namespace {

// Returns `count` UTF-16 units as UTF-8, or the empty string if they are not valid UTF-16.
PrRawString narrow(const wchar_t* wide, uint32_t count) {
    PrRawString value = prWindowsNarrowToRawString(wide, count);
    return prRawStringIsNull(&value) ? prRawStringEmpty() : value;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrRawString prCoreWindowsEnvironmentVariable(PrStringView name) {
    uint32_t nameCount = 0;
    wchar_t* wideName = prWindowsWidenCopy(name, &nameCount);
    if (wideName == nullptr || nameCount == 0) {
        prMemoryFree(wideName);
        return prRawStringEmpty();
    }

    // The first call measures the value with its terminator, and the second reads it. The value can change between
    // them, and a longer one is not read.
    PrRawString value = prRawStringEmpty();
    DWORD count = GetEnvironmentVariableW(wideName, nullptr, 0);
    wchar_t* wide = count > 1 ? static_cast<wchar_t*>(malloc(size_t(count) * sizeof(wchar_t))) : nullptr;
    DWORD read = wide != nullptr ? GetEnvironmentVariableW(wideName, wide, count) : 0;
    if (read != 0 && read < count)
        value = narrow(wide, uint32_t(read));
    free(wide);
    prMemoryFree(wideName);
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
