#include "pr/array.h"
#include "pr/file.h"
#include "pr/sort.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/thread.h"
#include "pr/time.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

String pathOf(PrStringView name) {
    return format("{}/{}", PR_CORE_TEST_DIR, name);
}

PrStringView contentsOf(const PrByteView& mapping) {
    return prStringFromBytes(mapping);
}

PrByteView bytesOf(PrStringView text) {
    return prStringBytes(text);
}

// A directory's entry, as prFileEachEntry reports it.
struct ListedEntry {
    String name;
    PrFileInfo info;
};

// Returns the absolute form of `path` under `base`, which prFilePathAbsolute makes.
String absolute(PrStringView path, PrStringView base = "") {
    return String::adopt(prFilePathAbsolute(path, base));
}

// What the callback of a watch counts, under a lock, and the condition that the callback signals.
struct Watched {
    PrMutex mutex;
    PrCondition changed;
    uint32_t calls;
};

void watchCalled(void* PR_NULLABLE context) {
    Watched& watched = *static_cast<Watched*>(context);
    prMutexLock(&watched.mutex);
    ++watched.calls;
    prConditionSignal(&watched.changed);
    prMutexUnlock(&watched.mutex);
}

// Waits up to five seconds for the watch to call back more than `seen` times, and returns the number of calls.
uint32_t waitForCall(Watched& watched, uint32_t seen) {
    prMutexLock(&watched.mutex);
    PrTimePerf start = prTimePerfNow();
    while (watched.calls <= seen && prTimePerfMilliseconds(start, prTimePerfNow()) < 5000)
        prConditionWaitFor(&watched.changed, &watched.mutex, 100);
    uint32_t calls = watched.calls;
    prMutexUnlock(&watched.mutex);
    return calls;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a directory is made with the directories above it, and making it again succeeds") {
    String nested = pathOf("made/twice/deep");
    CHECK(prFileCreateDirectory(nested));
    CHECK(prFileCreateDirectory(nested));
    // A path that names a file is not a directory.
    REQUIRE(prFileWrite(pathOf("made/file"), bytesOf("x")));
    CHECK(!prFileCreateDirectory(pathOf("made/file")));
    CHECK(prFileRemove(pathOf("made/file")));
}

TEST_CASE("a file is written whole, replaced while a mapping of it stays valid, and removed") {
    REQUIRE(prFileCreateDirectory(PR_CORE_TEST_DIR));
    String path = pathOf("written.txt");
    REQUIRE(prFileWrite(path, bytesOf("first")));
    PrBlob* first = prFileOpen(path);
    REQUIRE(first != nullptr);
    CHECK(contentsOf(prBlobBytes(first)) == "first");

    REQUIRE(prFileWrite(path, bytesOf("second, and longer")));
    PrBlob* second = prFileOpen(path);
    REQUIRE(second != nullptr);
    CHECK(contentsOf(prBlobBytes(second)) == "second, and longer");
    CHECK(contentsOf(prBlobBytes(first)) == "first");
    prBlobRelease(first);
    prBlobRelease(second);

    // An empty file opens as a blob of no bytes.
    REQUIRE(prFileWrite(path, PrByteView{nullptr, 0}));
    PrBlob* empty = prFileOpen(path);
    REQUIRE(empty != nullptr);
    CHECK(prBlobBytes(empty).size == 0);
    prBlobRelease(empty);

    CHECK(prFileRemove(path));
    CHECK(!prFileRemove(path));
    CHECK(prFileOpen(path) == nullptr);
    // A file in a directory that does not exist cannot be written.
    CHECK(!prFileWrite(pathOf("missing/written.txt"), bytesOf("x")));
}

TEST_CASE("a file's info is its kind, its size and when it changed") {
    REQUIRE(prFileCreateDirectory(pathOf("info")));
    String path = pathOf("info/sized.txt");
    PrTimeSystem before = prTimeSystemNow();
    REQUIRE(prFileWrite(path, bytesOf("12345")));
    PrFileInfo info{};
    REQUIRE(prFileInfo(path, &info));
    CHECK(info.kind == PrFileKind_Regular);
    CHECK(info.size == 5);
    // File systems record times more coarsely than the system time reads them.
    CHECK(prTimeSystemMilliseconds(before, info.modified) > -2000.0);
    CHECK(prTimeSystemMilliseconds(info.modified, prTimeSystemNow()) > -2000.0);
    REQUIRE(prFileInfo(pathOf("info"), &info));
    CHECK(info.kind == PrFileKind_Directory);
    CHECK(info.size == 0);
    CHECK(!prFileInfo(pathOf("info/missing"), &info));
    CHECK(info.kind == PrFileKind_None);
    CHECK(prFileRemove(path));
}

TEST_CASE("two spellings of a path name the same file, and two files are not the same") {
    REQUIRE(prFileCreateDirectory(pathOf("same/inner")));
    String path = pathOf("same/inner/file.txt");
    REQUIRE(prFileWrite(path, bytesOf("x")));
    REQUIRE(prFileWrite(pathOf("same/other.txt"), bytesOf("x")));
    CHECK(prFileSame(path, path));
    CHECK(prFileSame(path, pathOf("same/inner/../inner/./file.txt")));
#if defined(_WIN32)
    String backslashed;
    for (char c : path)
        backslashed.append(c == '/' ? '\\' : c);
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
    Array<ListedEntry> entries;
    auto collect = [](void* context, const PrFileEntry* entry) {
        static_cast<Array<ListedEntry>*>(context)->push(ListedEntry{.name = String(entry->name), .info = entry->info});
    };
    REQUIRE(prFileEachEntry(pathOf("listed"), collect, &entries));
    sortItems(entries.span(), [](const ListedEntry& a, const ListedEntry& b) { return prStringCompare(a.name, b.name) < 0; });
    REQUIRE(entries.count() == 3);
    CHECK(entries[0].name == "a.txt");
    CHECK(entries[0].info.kind == PrFileKind_Regular);
    CHECK(entries[1].name == "b.txt");
    CHECK(entries[1].info.size == 2);
    CHECK(entries[2].name == "inner");
    CHECK(entries[2].info.kind == PrFileKind_Directory);
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

TEST_CASE("a path's root is its leading separator, or on Windows a drive or the start of a UNC path") {
    CHECK(prFilePathRootSize("") == 0);
    CHECK(prFilePathRootSize("dir/notes.txt") == 0);
    CHECK(prFilePathRootSize("/notes.txt") == 1);
#if defined(_WIN32)
    CHECK(prFilePathRootSize("\\notes.txt") == 1);
    CHECK(prFilePathRootSize("C:\\dir") == 3);
    CHECK(prFilePathRootSize("c:notes.txt") == 2);
    CHECK(prFilePathRootSize("\\\\server\\share") == 2);
#endif
}

TEST_CASE("a path is absolute if it has a root of its own") {
    CHECK_FALSE(prFilePathIsAbsolute(""));
    CHECK_FALSE(prFilePathIsAbsolute("notes.txt"));
    CHECK_FALSE(prFilePathIsAbsolute("dir/notes.txt"));
#if defined(_WIN32)
    CHECK_FALSE(prFilePathIsAbsolute("/notes.txt"));
    CHECK(prFilePathIsAbsolute("C:\\dir\\notes.txt"));
    CHECK(prFilePathIsAbsolute("c:/notes.txt"));
    CHECK(prFilePathIsAbsolute("\\\\server\\share\\notes.txt"));
#else
    CHECK(prFilePathIsAbsolute("/notes.txt"));
#endif
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

TEST_CASE("a file moves or copies to an unused name with its contents, and never replaces an existing file") {
    REQUIRE(prFileCreateDirectory(pathOf("moved/into")));
    String original = pathOf("moved/original.txt");
    String moved = pathOf("moved/into/moved.txt");
    String copied = pathOf("moved/copied.txt");
    String taken = pathOf("moved/taken.txt");
    REQUIRE(prFileWrite(original, bytesOf("contents")));
    REQUIRE(prFileWrite(taken, bytesOf("other")));

    CHECK(prFileCopy(original, copied) == PrStatus_Ok);
    CHECK(prFileCopy(original, taken) == PrStatus_NotAllowed);
    CHECK(prFileCopy(pathOf("moved/missing.txt"), pathOf("moved/never.txt")) == PrStatus_NotFound);
    CHECK(prFileCopy(pathOf("moved/into"), pathOf("moved/never.txt")) == PrStatus_NotFound);
    PrFileInfo source{};
    PrFileInfo copy{};
    REQUIRE(prFileInfo(original, &source));
    REQUIRE(prFileInfo(copied, &copy));
    CHECK(copy.size == source.size);
    // The copy keeps the time of the original's last change, as coarsely as the file system records it.
    double apart = prTimeSystemMilliseconds(source.modified, copy.modified);
    CHECK(apart > -2000.0);
    CHECK(apart < 2000.0);

    CHECK(prFileMove(original, taken) == PrStatus_NotAllowed);
    CHECK(prFileMove(original, pathOf("moved/into")) == PrStatus_NotAllowed);
    CHECK(prFileMove(original, moved) == PrStatus_Ok);
    CHECK(prFileMove(original, pathOf("moved/again.txt")) == PrStatus_NotFound);
    PrBlob* blobs[3] = {prFileOpen(moved), prFileOpen(copied), prFileOpen(taken)};
    PrStringView expected[3] = {"contents", "contents", "other"};
    for (uint32_t i = 0; i < 3; ++i) {
        REQUIRE(blobs[i] != nullptr);
        CHECK(contentsOf(prBlobBytes(blobs[i])) == expected[i]);
        prBlobRelease(blobs[i]);
    }

    CHECK(prFileRemove(moved));
    CHECK(prFileRemove(copied));
    CHECK(prFileRemove(taken));
    CHECK(prFileRemove(pathOf("moved/into")));
    CHECK(prFileRemove(pathOf("moved")));
}

TEST_CASE("a watch calls back when a file is added to its directory, renamed in it or removed from it") {
    REQUIRE(prFileCreateDirectory(pathOf("watched")));
    Watched watched{};
    prMutexInit(&watched.mutex);
    prConditionInit(&watched.changed);
    PrFileWatch* watch = prFileWatchCreate(pathOf("watched"), watchCalled, &watched);
#if defined(_WIN32) || defined(__APPLE__) || (defined(__linux__) && !defined(__EMSCRIPTEN__))
    REQUIRE(watch != nullptr);
#endif
    CHECK(prFileWatchCreate(pathOf("watched/missing"), watchCalled, &watched) == nullptr);
    if (watch != nullptr) {
        REQUIRE(prFileWrite(pathOf("watched/added.txt"), bytesOf("x")));
        uint32_t added = waitForCall(watched, 0);
        CHECK(added > 0);
        REQUIRE(prFileMove(pathOf("watched/added.txt"), pathOf("watched/renamed.txt")) == PrStatus_Ok);
        uint32_t renamed = waitForCall(watched, added);
        CHECK(renamed > added);
        REQUIRE(prFileRemove(pathOf("watched/renamed.txt")));
        CHECK(waitForCall(watched, renamed) > renamed);
        prFileWatchDestroy(watch);
    }
    prFileWatchDestroy(nullptr);
    prConditionReset(&watched.changed);
    prMutexReset(&watched.mutex);
    CHECK(prFileRemove(pathOf("watched")));
}
