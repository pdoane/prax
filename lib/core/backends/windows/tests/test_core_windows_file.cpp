// Tests of the Windows backend of files against the sharing that Windows gives other readers of a file.

#include "pr/array.h"
#include "pr/file.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace pr_impl {

namespace {

// Returns `path` as a NUL-terminated UTF-16 string.
Array<wchar_t> widened(PrStringView path) {
    int count = MultiByteToWideChar(CP_UTF8, 0, path.data, int(path.size), nullptr, 0);
    Array<wchar_t> wide;
    wide.resize(uint32_t(count) + 1);
    MultiByteToWideChar(CP_UTF8, 0, path.data, int(path.size), wide.data(), count);
    wide[uint32_t(count)] = 0;
    return wide;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a file that another reader holds open, sharing its removal, is still replaced") {
    REQUIRE(prFileCreateDirectory(PR_CORE_TEST_DIR));
    String path = format("{}/held.txt", PR_CORE_TEST_DIR);
    REQUIRE(prFileWrite(path, prStringBytes("first")));
    // As a virus scanner opens a file that was just written.
    Array<wchar_t> wide = widened(path);
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE held = CreateFileW(wide.data(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    CHECK(prFileWrite(path, prStringBytes("second")));
    CloseHandle(held);
    PrByteView mapping{};
    REQUIRE(prFileMap(path, &mapping));
    CHECK(prStringFromBytes(mapping) == "second");
    prFileUnmap(&mapping);
    CHECK(prFileRemove(path));
}
