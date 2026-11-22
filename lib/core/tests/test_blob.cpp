#include "pr/array.h"
#include "pr/blob.h"
#include "pr/file.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

PrStringView contentsOf(const PrBlob* blob) {
    return prStringFromBytes(prBlobBytes(blob));
}

// The context of a source. It counts the reads of the source and records whether the source was released.
struct Counted {
    uint32_t reads = 0;
    bool released = false;

    static void start(void* PR_NULLABLE context, PrBlobRead* read) {
        Counted* counted = static_cast<Counted*>(context);
        ++counted->reads;
        prBlobReadComplete(read, PrStatus_Ok, prBlobCopy(prStringBytes("counted")), 0);
    }

    static void release(void* PR_NULLABLE context) { static_cast<Counted*>(context)->released = true; }
};

// A provider that completes its reads later, when its owner polls it. A provider over an event loop behaves this way.
struct Deferred {
    Array<PrBlobRead*> pending;

    static void start(void* PR_NULLABLE context,
            PrBlobRead* read) { static_cast<Deferred*>(context)->pending.push(read); }

    // Completes the reads that were started, with the bytes "deferred" in the ranges that they ask for.
    void poll() {
        for (PrBlobRead* read : pending) {
            PrBlob* whole = prBlobCopy(prStringBytes("deferred"));
            PrBlobRange range = prBlobReadRange(read);
            PrBlob* part = prBlobSlice(whole, range.offset, range.size);
            prBlobRelease(whole);
            prBlobReadComplete(read, PrStatus_Ok, part, 0);
        }
        pending.clear();
    }
};

PrStatus readAll(const PrBlobSource* source, PrBlob* PR_NULLABLE* PR_NONNULL out) {
    return prBlobSourceRead(source, prBlobRangeWhole(), out);
}

} // namespace

} // namespace pr_impl

using namespace harness;
using namespace pr_impl;

TEST_SUITE_BEGIN("core");

TEST_CASE("a blob source calls its provider for each read, and releases its context when the last reference is "
        "released") {
    Counted counted;
    PrBlobSourceDesc desc{
        .start = Counted::start,
        .release = Counted::release,
        .context = &counted,
        .size = kPrBlobSizeUnknown,
    };
    PrBlobSource* source = prBlobSourceCreate(&desc);
    prBlobSourceRetain(source);
    for (uint32_t i = 0; i < 2; ++i) {
        PrBlob* blob = nullptr;
        REQUIRE(readAll(source, &blob) == PrStatus_Ok);
        CHECK(contentsOf(blob) == "counted");
        prBlobRelease(blob);
    }
    CHECK(counted.reads == 2);
    prBlobSourceRelease(source);
    CHECK(!counted.released);
    prBlobSourceRelease(source);
    CHECK(counted.released);
}

TEST_CASE("a blob source made from a blob keeps the blob, and one made from a file reads the file's current contents") {
    PrBlob* blob = prBlobCopy(prStringBytes("kept"));
    PrBlobSource* kept = prBlobSourceCreateBlob(blob);
    prBlobRelease(blob);
    PrBlob* read = nullptr;
    REQUIRE(readAll(kept, &read) == PrStatus_Ok);
    CHECK(contentsOf(read) == "kept");
    prBlobRelease(read);
    prBlobSourceRelease(kept);

    String path = format("{}/blob_source.txt", testFolder());
    REQUIRE(prFileWrite(path, prStringBytes("first")));
    PrBlobSource* file = prBlobSourceCreateFile(path);
    REQUIRE(readAll(file, &read) == PrStatus_Ok);
    CHECK(contentsOf(read) == "first");
    prBlobRelease(read);
    REQUIRE(prFileWrite(path, prStringBytes("second")));
    REQUIRE(readAll(file, &read) == PrStatus_Ok);
    CHECK(contentsOf(read) == "second");
    prBlobRelease(read);
    REQUIRE(prFileRemove(path));
    CHECK(readAll(file, &read) == PrStatus_Io);
    CHECK(read == nullptr);
    prBlobSourceRelease(file);
}

TEST_CASE("a read whose provider completes later calls its callback at that time, and a wait returns once the read is done") {
    Deferred deferred;
    PrBlobSourceDesc desc{.start = Deferred::start, .context = &deferred, .size = 8};
    PrBlobSource* source = prBlobSourceCreate(&desc);
    CHECK(prBlobSourceSize(source) == 8);
    uint32_t calls = 0;
    PrBlobReadDesc read = prBlobReadDescDefault();
    read.done = [](void* PR_NULLABLE context, PrBlobRead*) { ++*static_cast<uint32_t*>(context); };
    read.context = &calls;
    PrBlobRead* started = prBlobReadCreate(source, &read);
    CHECK(!prBlobReadIsDone(started));
    CHECK(calls == 0);
    deferred.poll();
    CHECK(prBlobReadIsDone(started));
    CHECK(calls == 1);
    prBlobReadWait(started);
    REQUIRE(prBlobReadStatus(started) == PrStatus_Ok);
    CHECK(contentsOf(prBlobReadBlob(started)) == "deferred");
    prBlobReadDestroy(started);
    prBlobSourceRelease(source);
}

TEST_CASE("a ranged read through nested range sources reads the matching bytes of the base source, into a blob or "
        "storage") {
    PrBlob* blob = prBlobCopy(prStringBytes("an archive of entries"));
    PrBlobSource* archive = prBlobSourceCreateBlob(blob);
    prBlobRelease(blob);
    // An entry of the archive, and a part of that entry.
    PrBlobSource* entry = prBlobSourceCreateRange(archive, PrBlobRange{3, 7});
    PrBlobSource* part = prBlobSourceCreateRange(entry, PrBlobRange{2, 100});
    CHECK(prBlobSourceSize(entry) == 7);
    CHECK(prBlobSourceSize(part) == 5);
    PrBlob* read = nullptr;
    REQUIRE(readAll(entry, &read) == PrStatus_Ok);
    CHECK(contentsOf(read) == "archive");
    prBlobRelease(read);
    REQUIRE(prBlobSourceRead(part, PrBlobRange{1, 3}, &read) == PrStatus_Ok);
    CHECK(contentsOf(read) == "hiv");
    prBlobRelease(read);
    uint8_t storage[4] = {};
    PrBlobReadDesc desc{.range = {0, 4}, .storage = storage};
    PrBlobRead* into = prBlobReadCreate(part, &desc);
    prBlobReadWait(into);
    REQUIRE(prBlobReadStatus(into) == PrStatus_Ok);
    CHECK(prBlobReadByteCount(into) == 4);
    CHECK(prBlobReadBlob(into) == nullptr);
    CHECK(prStringFromBytes(PrByteView{storage, 4}) == "chiv");
    prBlobReadDestroy(into);
    prBlobSourceRelease(part);
    prBlobSourceRelease(entry);

    // A range over a provider that completes later completes when that provider does.
    Deferred deferred;
    PrBlobSourceDesc later{.start = Deferred::start, .context = &deferred, .size = 8};
    PrBlobSource* slow = prBlobSourceCreate(&later);
    PrBlobSource* tail = prBlobSourceCreateRange(slow, PrBlobRange{5, kPrBlobSizeUnknown});
    PrBlobReadDesc whole = prBlobReadDescDefault();
    PrBlobRead* pending = prBlobReadCreate(tail, &whole);
    CHECK(!prBlobReadIsDone(pending));
    deferred.poll();
    REQUIRE(prBlobReadIsDone(pending));
    CHECK(contentsOf(prBlobReadBlob(pending)) == "red");
    prBlobReadDestroy(pending);
    prBlobSourceRelease(tail);
    prBlobSourceRelease(slow);
    prBlobSourceRelease(archive);
}

TEST_SUITE_END();
