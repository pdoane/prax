// The Windows backend of resources.

#include "pr/core_windows_resource.h"

#include "pr/memory.h"
#include "pr/raw_string.h"
#include "pr/windows_string.h"

#include "core/backends/shared/src/core_shared_resource.h"

#include <windows.h>

namespace pr_impl {

namespace {

// Returns the path of the process's executable, or the empty string if the system does not name it. A path longer than
// MAX_PATH takes a larger buffer, up to the longest path that Windows has.
PrRawString executablePath() {
    wchar_t buffer[MAX_PATH];
    DWORD size = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (size != 0 && size < MAX_PATH)
        return prWindowsNarrowToRawString(buffer, size);
    constexpr DWORD kLongest = 32768;
    wchar_t* large = static_cast<wchar_t*>(prMemoryAlloc(kLongest * sizeof(wchar_t)));
    size = GetModuleFileNameW(nullptr, large, kLongest);
    PrRawString path = size != 0 && size < kLongest ? prWindowsNarrowToRawString(large, size) : prRawStringEmpty();
    prMemoryFree(large);
    return path;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrBlob* PR_NULLABLE prCoreWindowsResourceOpen(PrStringView name) {
    PrRawString executable = executablePath();
    PrRawString directory = resourceDirectoryBeside(prRawStringView(&executable));
    PrBlob* blob = resourceFileOpen(prRawStringView(&directory), name);
    prRawStringReset(&directory);
    prRawStringReset(&executable);
    return blob;
}
