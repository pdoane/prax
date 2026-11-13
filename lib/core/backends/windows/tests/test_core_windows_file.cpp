// Tests of the Windows backend of files against the sharing that Windows gives other readers of a file.

#include "pr/file.h"
#include "pr/raw_array.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/windows_string.h"

#include "tools/harness/test.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using namespace pr_impl;

TEST_CASE("a file that another reader holds open, sharing its removal, is still replaced") {
    REQUIRE(prFileCreateDirectory(PR_CORE_TEST_DIR));
    String path = format("{}/held.txt", PR_CORE_TEST_DIR);
    REQUIRE(prFileWrite(path, prStringBytes("first")));
    // As a virus scanner opens a file that was just written.
    RawArray<wchar_t> wide;
    windowsWiden(path, wide);
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE held = CreateFileW(wide.data, GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    wide.reset();
    REQUIRE(held != INVALID_HANDLE_VALUE);
    CHECK(prFileWrite(path, prStringBytes("second")));
    CloseHandle(held);
    PrByteView mapping{};
    REQUIRE(prFileMap(path, &mapping));
    CHECK(prStringFromBytes(mapping) == "second");
    prFileUnmap(&mapping);
    CHECK(prFileRemove(path));
}
