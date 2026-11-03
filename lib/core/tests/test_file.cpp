#include "pr/file.h"
#include "pr/time.h"

#include "doctest/doctest.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace pr_impl {

namespace {

std::string pathOf(const char* name) {
    return std::string(PR_CORE_TEST_DIR) + "/" + name;
}

std::string contentsOf(const PrFileMapping& mapping) {
    return std::string(reinterpret_cast<const char*>(mapping.data), mapping.size);
}

PrByteView bytesOf(const char* text) {
    return PrByteView{reinterpret_cast<const uint8_t*>(text), std::char_traits<char>::length(text)};
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
    PrFileMapping first{};
    REQUIRE(prFileMap(path, &first));
    CHECK(contentsOf(first) == "first");

    REQUIRE(prFileWrite(path, bytesOf("second, and longer")));
    PrFileMapping second{};
    REQUIRE(prFileMap(path, &second));
    CHECK(contentsOf(second) == "second, and longer");
    CHECK(contentsOf(first) == "first");
    prFileUnmap(&first);
    prFileUnmap(&second);

    // An empty file maps to an empty mapping.
    REQUIRE(prFileWrite(path, PrByteView{nullptr, 0}));
    PrFileMapping empty{};
    CHECK(prFileMap(path, &empty));
    CHECK(empty.size == 0);
    prFileUnmap(&empty);

    CHECK(prFileRemove(path));
    CHECK(!prFileRemove(path));
    PrFileMapping gone{};
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
    CHECK(prFileRemove(pathOf("listed/b.txt")));
}

TEST_CASE("the platform names directories for the user's caches and data") {
    PrRawString directories[] = {prFileCacheDirectory(), prFileDataDirectory()};
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
    CHECK(prRawStringView(&directories[0]) != prRawStringView(&directories[1]));
    for (PrRawString& directory : directories)
        prRawStringReset(&directory);
}
