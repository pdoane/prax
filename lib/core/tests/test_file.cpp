#include "pr/file.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/time.h"

#include "tools/harness/test.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace pr_impl {

namespace {

std::string pathOf(const char* name) {
    return std::string(PR_CORE_TEST_DIR) + "/" + name;
}

std::string contentsOf(const PrByteView& mapping) {
    return std::string(reinterpret_cast<const char*>(mapping.data), mapping.size);
}

PrByteView bytesOf(const char* text) {
    return PrByteView{reinterpret_cast<const uint8_t*>(text), std::char_traits<char>::length(text)};
}

// Returns the absolute form of `path` under `base`, which prFilePathAbsolute makes.
String absolute(PrStringView path, PrStringView base = "") {
    return String::adopt(prFilePathAbsolute(path, base));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a directory is made with the directories above it, and making it again succeeds") {
    std::string nested = pathOf("made/twice/deep");
    CHECK(prFileCreateDirectory(nested));
    CHECK(prFileCreateDirectory(nested));
    // A path that names a file is not a directory.
    REQUIRE(prFileWrite(pathOf("made/file"), bytesOf("x")));
    CHECK(!prFileCreateDirectory(pathOf("made/file")));
    CHECK(prFileRemove(pathOf("made/file")));
}

TEST_CASE("a file is written whole, replaced while a mapping of it stays valid, and removed") {
    REQUIRE(prFileCreateDirectory(PR_CORE_TEST_DIR));
    std::string path = pathOf("written.txt");
    REQUIRE(prFileWrite(path, bytesOf("first")));
    PrByteView first{};
    REQUIRE(prFileMap(path, &first));
    CHECK(contentsOf(first) == "first");

    REQUIRE(prFileWrite(path, bytesOf("second, and longer")));
    PrByteView second{};
    REQUIRE(prFileMap(path, &second));
    CHECK(contentsOf(second) == "second, and longer");
    CHECK(contentsOf(first) == "first");
    prFileUnmap(&first);
    prFileUnmap(&second);

    // An empty file maps to an empty mapping.
    REQUIRE(prFileWrite(path, PrByteView{nullptr, 0}));
    PrByteView empty{};
    CHECK(prFileMap(path, &empty));
    CHECK(empty.size == 0);
    prFileUnmap(&empty);

    CHECK(prFileRemove(path));
    CHECK(!prFileRemove(path));
    PrByteView gone{};
    CHECK(!prFileMap(path, &gone));
    // A file in a directory that does not exist cannot be written.
    CHECK(!prFileWrite(pathOf("missing/written.txt"), bytesOf("x")));
}

TEST_CASE("a file's info is its kind, its size and when it changed") {
    REQUIRE(prFileCreateDirectory(pathOf("info")));
    std::string path = pathOf("info/sized.txt");
    double before = prTimeWallMilliseconds();
    REQUIRE(prFileWrite(path, bytesOf("12345")));
    PrFileInfo info{};
    REQUIRE(prFileInfo(path, &info));
    CHECK(info.kind == PrFileKind_Regular);
    CHECK(info.size == 5);
    // File systems record times more coarsely than the wall clock reads them.
    CHECK(info.modified > before - 2000.0);
    CHECK(info.modified < prTimeWallMilliseconds() + 2000.0);
    REQUIRE(prFileInfo(pathOf("info"), &info));
    CHECK(info.kind == PrFileKind_Directory);
    CHECK(info.size == 0);
    CHECK(!prFileInfo(pathOf("info/missing"), &info));
    CHECK(info.kind == PrFileKind_None);
    CHECK(prFileRemove(path));
}

TEST_CASE("two spellings of a path name the same file, and two files are not the same") {
    REQUIRE(prFileCreateDirectory(pathOf("same/inner")));
    std::string path = pathOf("same/inner/file.txt");
    REQUIRE(prFileWrite(path, bytesOf("x")));
    REQUIRE(prFileWrite(pathOf("same/other.txt"), bytesOf("x")));
    CHECK(prFileSame(path, path));
    CHECK(prFileSame(path, pathOf("same/inner/../inner/./file.txt")));
#if defined(_WIN32)
    std::string backslashed = path;
    for (char& c : backslashed)
        c = c == '/' ? '\\' : c;
    CHECK(prFileSame(path, backslashed));
#endif
    CHECK(prFileSame(pathOf("same/inner"), pathOf("same/inner/")));
    CHECK(!prFileSame(path, pathOf("same/other.txt")));
    CHECK(!prFileSame(path, pathOf("same/missing.txt")));
    CHECK(!prFileSame(pathOf("same/missing.txt"), pathOf("same/missing.txt")));
    CHECK(prFileRemove(path));
    CHECK(prFileRemove(pathOf("same/other.txt")));
}

TEST_CASE("a directory's entries are its files and directories, without its links to itself and its parent") {
    REQUIRE(prFileCreateDirectory(pathOf("listed/inner")));
    REQUIRE(prFileWrite(pathOf("listed/a.txt"), bytesOf("a")));
    REQUIRE(prFileWrite(pathOf("listed/b.txt"), bytesOf("bb")));
    std::vector<std::pair<std::string, PrFileInfo>> entries;
    auto collect = [](void* context, const PrFileEntry* entry) {
        static_cast<std::vector<std::pair<std::string, PrFileInfo>>*>(context)->emplace_back(
            std::string(entry->name.data, entry->name.size), entry->info);
    };
    REQUIRE(prFileEachEntry(pathOf("listed"), collect, &entries));
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].first == "a.txt");
    CHECK(entries[0].second.kind == PrFileKind_Regular);
    CHECK(entries[1].first == "b.txt");
    CHECK(entries[1].second.size == 2);
    CHECK(entries[2].first == "inner");
    CHECK(entries[2].second.kind == PrFileKind_Directory);
    CHECK(!prFileEachEntry(pathOf("listed/a.txt"), collect, &entries));
    CHECK(!prFileEachEntry(pathOf("listed/missing"), collect, &entries));
    CHECK(prFileRemove(pathOf("listed/a.txt")));
    // A directory that holds files stays, and an empty one is removed.
    CHECK(!prFileRemove(pathOf("listed")));
    CHECK(prFileRemove(pathOf("listed/b.txt")));
    CHECK(prFileRemove(pathOf("listed/inner")));
    CHECK(prFileRemove(pathOf("listed")));
    PrFileInfo removed{};
    CHECK(!prFileInfo(pathOf("listed"), &removed));
}

TEST_CASE("the platform names each of the user's directories, and they differ") {
    PrRawString directories[PrFileUserDirectory_Count];
    for (uint32_t i = 0; i < PrFileUserDirectory_Count; ++i)
        directories[i] = prFileUserDirectory(PrFileUserDirectory(i));
#if defined(__EMSCRIPTEN__)
    // The web keeps no directories for the user, unless the host that runs the program names its home.
    if (prRawStringIsEmpty(&directories[PrFileUserDirectory_Cache]) &&
            prRawStringIsEmpty(&directories[PrFileUserDirectory_Data]))
        return;
#endif
    for (PrRawString& directory : directories) {
        PrStringView view = prRawStringView(&directory);
        REQUIRE(view.size != 0);
#if defined(_WIN32)
        CHECK(view.size > 2);
        CHECK(view.data[1] == ':');
#else
        CHECK(view.data[0] == '/');
#endif
    }
    for (uint32_t i = 0; i < PrFileUserDirectory_Count; ++i) {
        for (uint32_t j = i + 1; j < PrFileUserDirectory_Count; ++j)
            CHECK(prRawStringView(&directories[i]) != prRawStringView(&directories[j]));
    }
    for (PrRawString& directory : directories)
        prRawStringReset(&directory);
}

TEST_CASE("a path's parts are read from its text") {
    CHECK(prFilePathName("dir/sub/notes.txt") == "notes.txt");
    CHECK(prFilePathName("notes.txt") == "notes.txt");
    CHECK(prFilePathName("dir/") == "");
    CHECK(prFilePathParent("dir/sub/notes.txt") == "dir/sub");
    CHECK(prFilePathParent("dir//notes.txt") == "dir");
    CHECK(prFilePathParent("/notes.txt") == "/");
    CHECK(prFilePathParent("notes.txt") == "");
    CHECK(prFilePathExtension("dir.d/notes.txt") == ".txt");
    CHECK(prFilePathExtension("archive.tar.gz") == ".gz");
    CHECK(prFilePathExtension("dir.d/README") == "");
    CHECK(prFilePathExtension(".profile") == "");
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    CHECK(prFilePathName("C:\\dir\\notes.txt") == "notes.txt");
    CHECK(prFilePathParent("C:\\dir\\notes.txt") == "C:\\dir");
#endif
}

TEST_CASE("a part appends to a path after one separator") {
    PrTextBuffer path = {};
    prFilePathAppend(&path, "dir");
    prFilePathAppend(&path, "sub");
    CHECK(prTextBufferView(&path) == "dir/sub");
    prTextBufferAppend(&path, "/");
    prFilePathAppend(&path, "notes.txt");
    CHECK(prTextBufferView(&path) == "dir/sub/notes.txt");
    prTextBufferReset(&path);
}

TEST_CASE("an absolute path is normalized from its text, and a relative one is under the working directory") {
    // The root is "/", or on Windows the root of the working directory's drive.
    String root = absolute("/");
    CHECK(prStringEndsWith(root, "/"));
    CHECK(absolute("/a/b/../c/./d//e") == format("{}a/c/d/e", root));
    CHECK(absolute("/a/b/..") == format("{}a", root));
    CHECK(absolute("/../x") == format("{}x", root));
    PrRawString empty = prFilePathAbsolute("", "");
    CHECK(prRawStringIsNull(&empty));

    PrRawString working = prFileWorkingDirectory();
    REQUIRE(!prRawStringIsEmpty(&working));
    String base = absolute(prRawStringView(&working));
    prRawStringReset(&working);
    String expected = base;
    if (prStringEndsWith(expected, "/"))
        expected.append("notes.txt");
    else
        expected.append("/notes.txt");
    CHECK(absolute("dir/../notes.txt") == expected);
    CHECK(absolute("./notes.txt") == expected);

#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    CHECK(absolute("C:\\a\\..\\b") == "C:/b");
    CHECK(absolute("C:") == "C:/");
    CHECK(absolute("\\\\server\\share\\x") == "//server/share/x");
#endif
    // A relative path is resolved against the base directory. A relative base is itself resolved against the working
    // directory.
    CHECK(absolute("c/../d.txt", "/a/b") == format("{}a/b/d.txt", root));
    CHECK(absolute("/x/y", "/a/b") == format("{}x/y", root));
    CHECK(absolute("notes.txt", "dir/..") == expected);

#if defined(_WIN32)
    // A lone separator is the root of the working directory's drive.
    String drive = absolute("\\x");
    CHECK(prStringSlice(drive, 1, drive.count() - 1) == ":/x");
#endif
}
