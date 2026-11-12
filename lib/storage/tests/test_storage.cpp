#include "pr/file.h"
#include "pr/storage_disk.h"
#include "pr/storage_memory.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/task_builtin.h"

#include "tools/harness/test.h"

extern "C" int prTestStorageCApi(void);

namespace pr_impl {

namespace {

PrByteView bytesOf(PrStringView text) {
    return prStringBytes(text);
}

// Returns the record at `key` as text, or "<none>" if the store has none.
String readText(PrStorage* storage, const char* key) {
    PrStorageRecord record{};
    if (!prStorageRead(storage, key, &record))
        return "<none>";
    String text(prStringFromBytes(record.bytes));
    prStorageRecordRelease(storage, &record);
    return text;
}

String directoryOf(const char* name) {
    return format("{}/{}", PR_STORAGE_TEST_DIR, name);
}

// Returns the store's keys in the order of their positions, separated by spaces.
String keysOf(PrStorage* storage) {
    String keys;
    for (uint32_t i = 0; i < prStorageCount(storage); ++i) {
        PrRawString key = prStorageKey(storage, i);
        if (!keys.isEmpty())
            keys.append(' ');
        keys.append(prRawStringView(&key));
        prRawStringReset(&key);
    }
    PrRawString past = prStorageKey(storage, prStorageCount(storage));
    CHECK(prRawStringIsNull(&past));
    return keys;
}

// The pool that writes the files of the tests' disk stores, which outlives them.
PrTaskPool* writer() {
    static struct Pool {
        PrTaskPool* pool = prTaskBuiltinCreate(2);
        ~Pool() { prTaskPoolDestroy(pool); }
    } pool;
    return pool.pool;
}

// Opens a disk store in a directory of its own, emptied first.
PrStorage* openDisk(const char* name, uint64_t limit, PrStorageEviction eviction = PrStorageEviction_LeastRecentlyUsed) {
    String directory = directoryOf(name);
    PrStorageDiskDesc desc{.directory = directory, .limitBytes = limit, .eviction = eviction, .pool = writer()};
    PrStorage* storage = nullptr;
    REQUIRE(prStorageDiskCreate(&desc, &storage) == PrStatus_Ok);
    return storage;
}

PrStorage* openEmptyDisk(const char* name, uint64_t limit,
        PrStorageEviction eviction = PrStorageEviction_LeastRecentlyUsed) {
    PrStorage* storage = openDisk(name, limit, eviction);
    prStorageClear(storage);
    return storage;
}

// The behavior that every store that evicts shares, at a limit of ten bytes.
void checkStore(PrStorage* storage) {
    CHECK(readText(storage, "a") == "<none>");
    CHECK(prStorageWrite(storage, "a", bytesOf("1234")));
    CHECK(readText(storage, "a") == "1234");
    CHECK(prStorageSize(storage) == 5);

    // A record that is held keeps its bytes while the store replaces it.
    PrStorageRecord held{};
    REQUIRE(prStorageRead(storage, "a", &held));
    CHECK(prStorageWrite(storage, "a", bytesOf("5678")));
    CHECK(prStringFromBytes(held.bytes) == "1234");
    prStorageRecordRelease(storage, &held);
    CHECK(readText(storage, "a") == "5678");

    // At the limit, the record used least recently goes first.
    CHECK(prStorageWrite(storage, "b", bytesOf("bbbb")));
    CHECK(readText(storage, "a") == "5678");
    CHECK(prStorageWrite(storage, "c", bytesOf("cccc")));
    CHECK(readText(storage, "b") == "<none>");
    CHECK(readText(storage, "a") == "5678");
    CHECK(readText(storage, "c") == "cccc");
    CHECK(prStorageSize(storage) == 10);
    CHECK(prStorageCount(storage) == 2);

    // A record larger than the limit is not kept, and the record it would replace goes.
    CHECK_FALSE(prStorageWrite(storage, "c", bytesOf("much too large")));
    CHECK(readText(storage, "c") == "<none>");

    CHECK(prStorageWrite(storage, "empty", bytesOf("")));
    CHECK(readText(storage, "empty") == "");
    prStorageRemove(storage, "a");
    CHECK(readText(storage, "a") == "<none>");
    prStorageClear(storage);
    CHECK(prStorageSize(storage) == 0);
    CHECK(prStorageCount(storage) == 0);
    CHECK(readText(storage, "empty") == "<none>");
}

// The behavior that every store that does not evict shares, at a limit of ten bytes.
void checkKeepingStore(PrStorage* storage) {
    CHECK(prStorageWrite(storage, "a", bytesOf("1111")));
    CHECK(prStorageWrite(storage, "b", bytesOf("2222")));
    CHECK(keysOf(storage) == "a b");

    // A write that would pass the limit is refused, and the record it would replace stays.
    CHECK_FALSE(prStorageWrite(storage, "c", bytesOf("3")));
    CHECK_FALSE(prStorageWrite(storage, "a", bytesOf("11111")));
    CHECK(readText(storage, "a") == "1111");
    CHECK(prStorageSize(storage) == 10);

    // A record that replaces another keeps its position.
    CHECK(prStorageWrite(storage, "a", bytesOf("9")));
    CHECK(keysOf(storage) == "a b");
    CHECK(readText(storage, "a") == "9");
    CHECK(prStorageSize(storage) == 7);
    CHECK(prStorageWrite(storage, "c", bytesOf("3")));
    CHECK(keysOf(storage) == "a b c");
    prStorageClear(storage);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("storage: the public headers compile as C") {
    CHECK(prTestStorageCApi() == 0);
}

TEST_CASE("storage: a store in memory reads, replaces, evicts and removes its records") {
    PrStorage* storage = prStorageMemoryCreate(10, PrStorageEviction_LeastRecentlyUsed);
    checkStore(storage);
    prStorageDestroy(storage);
}

TEST_CASE("storage: a store in memory that does not evict refuses what does not fit") {
    PrStorage* storage = prStorageMemoryCreate(10, PrStorageEviction_None);
    checkKeepingStore(storage);
    prStorageDestroy(storage);
}

TEST_CASE("storage: a store on disk reads, replaces, evicts and removes its records") {
    PrStorage* storage = openEmptyDisk("shared", 10);
    checkStore(storage);
    prStorageDestroy(storage);
}

TEST_CASE("storage: a store on disk that does not evict refuses what does not fit") {
    PrStorage* storage = openEmptyDisk("keeping", 10, PrStorageEviction_None);
    checkKeepingStore(storage);
    prStorageDestroy(storage);
}

TEST_CASE("storage: a store on disk keeps its records and their order of use when it is opened again") {
    PrStorage* storage = openEmptyDisk("reopened", 20);
    prStorageWrite(storage, "first", bytesOf("1111"));
    prStorageWrite(storage, "second", bytesOf("2222"));
    CHECK(readText(storage, "first") == "1111");
    prStorageDestroy(storage);

    storage = openDisk("reopened", 20);
    CHECK(keysOf(storage) == "first second");
    CHECK(readText(storage, "second") == "2222");
    CHECK(readText(storage, "first") == "1111");
    CHECK(prStorageSize(storage) == 19);
    // "second" was used before "first" in this run, so it goes first.
    prStorageWrite(storage, "third", bytesOf("3333"));
    CHECK(readText(storage, "second") == "<none>");
    CHECK(readText(storage, "first") == "1111");
    prStorageDestroy(storage);

    // A lower limit leaves a store that does not evict as it was, and refuses its writes.
    storage = openDisk("reopened", 9, PrStorageEviction_None);
    CHECK(prStorageSize(storage) == 18);
    CHECK_FALSE(prStorageWrite(storage, "first", bytesOf("1")));
    CHECK(readText(storage, "first") == "1111");
    prStorageDestroy(storage);

    // It evicts what no longer fits when a store that evicts opens.
    storage = openDisk("reopened", 9);
    CHECK(prStorageSize(storage) <= 9);
    prStorageClear(storage);
    prStorageDestroy(storage);
}

TEST_CASE("storage: a record whose file is gone is not read, and its entry goes") {
    PrStorage* storage = openEmptyDisk("lost", 100);
    prStorageWrite(storage, "kept", bytesOf("bytes"));
    prStorageDestroy(storage);
    // The store's first file holds the record.
    REQUIRE(prFileRemove(format("{}/1", directoryOf("lost"))));
    storage = openDisk("lost", 100);
    CHECK(prStorageSize(storage) == 9);
    CHECK(readText(storage, "kept") == "<none>");
    CHECK(prStorageSize(storage) == 0);
    prStorageDestroy(storage);
}

TEST_CASE("storage: stores on disk that share a pool each keep their records") {
    const uint32_t threadCounts[] = {0u, 1u};
    for (uint32_t threads : threadCounts) {
        PrTaskPool* pool = prTaskBuiltinCreate(threads);
        const char* names[] = {"shared-a", "shared-b", "shared-c"};
        PrStorage* stores[3];
        for (uint32_t i = 0; i < 3; ++i) {
            String directory = directoryOf(names[i]);
            PrStorageDiskDesc desc{.directory = directory, .limitBytes = 100, .pool = pool};
            REQUIRE(prStorageDiskCreate(&desc, &stores[i]) == PrStatus_Ok);
            prStorageClear(stores[i]);
            for (uint32_t j = 0; j < 20; ++j)
                prStorageWrite(stores[i], "key", bytesOf(format("{}{}", names[i], j)));
        }
        for (PrStorage* store : stores)
            prStorageDestroy(store);
        for (uint32_t i = 0; i < 3; ++i) {
            String directory = directoryOf(names[i]);
            PrStorageDiskDesc desc{.directory = directory, .limitBytes = 100, .pool = pool};
            REQUIRE(prStorageDiskCreate(&desc, &stores[i]) == PrStatus_Ok);
            CHECK(readText(stores[i], "key") == format("{}19", names[i]));
            CHECK(prStorageCount(stores[i]) == 1);
            prStorageDestroy(stores[i]);
        }
        prTaskPoolDestroy(pool);
    }
}
