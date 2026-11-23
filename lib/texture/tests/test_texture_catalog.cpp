// Tests of the texture catalog. They cover one source for each name, names that last as long as their sources,
// failures, and names that are forgotten.

#include "pr/atomic.h"
#include "pr/blob.h"
#include "pr/memory.h"
#include "pr/task.h"
#include "pr/texture.h"
#include "pr/texture_backend.h"
#include "pr/texture_catalog.h"
#include "pr/thread.h"

#include "tools/harness/texture_await.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// The first bytes of a PNG file, which lib/mime classifies as image/png.
constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

// A format for the media type image/png. It opens a source of one texel and counts its opens.
struct CountingFormat {
    mutable uint32_t opened = 0;

    struct Record : PrTextureFormat {
        const CountingFormat* counting;
    };

    static PrStatus create(const PrTextureFormat* record, const PrBlobSource*, const PrBlob*, const PrTextureLimits*,
            bool, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
        ++static_cast<const Record*>(record)->counting->opened;
        PrTextureSourceDesc desc{
            .texture = prTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{1, 1}, PrTextureAlpha_None),
            .read = [](void* PR_NULLABLE, PrTextureRead* read) { prTextureReadComplete(read, PrStatus_Ok); },
        };
        *out = prTextureSourceCreate(&desc);
        return PrStatus_Ok;
    }

    PrTextureFormatSet* set() const {
        Record* record = static_cast<Record*>(prMemoryAllocZeroed(sizeof(Record)));
        record->destroyFn = [](PrTextureFormat* made) { prMemoryFree(made); };
        record->supportsTypeFn = [](const PrTextureFormat*, PrStringView type) { return type == "image/png"; };
        record->createFn = create;
        record->counting = this;
        PrTextureFormat* formats[1] = {record};
        return prTextureFormatSetCreate(formats, 1);
    }
};

// A blob source of the first bytes of a PNG. Its reads complete when the test calls `complete`.
struct LaterBytes {
    Atomic<PrBlobRead*> pending;

    LaterBytes() { pending.store(nullptr); }

    PrBlobSource* source() {
        PrBlobSourceDesc desc{
            .start = [](void* PR_NULLABLE context, PrBlobRead* read) {
                static_cast<LaterBytes*>(context)->pending.store(read);
            },
            .context = this,
            .size = sizeof(kPngSignature),
        };
        return prBlobSourceCreate(&desc);
    }

    // Waits for the read that a thread of the pool begins, then completes it.
    void complete() {
        PrBlobRead* read = nullptr;
        for (uint32_t i = 0; i < 5000 && read == nullptr; ++i) {
            read = pending.exchange(nullptr);
            if (read == nullptr)
                prThreadSleep(1);
        }
        REQUIRE(read != nullptr);
        prBlobReadComplete(read, PrStatus_Ok, prBlobCopy(PrByteView{kPngSignature, sizeof(kPngSignature)}), 0);
    }
};

// Returns a blob source of `bytes`.
PrBlobSource* bytesOf(PrByteView bytes) {
    PrBlob* blob = prBlobCopy(bytes);
    PrBlobSource* source = prBlobSourceCreateBlob(blob);
    prBlobRelease(blob);
    return source;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("texture");

TEST_CASE("a catalog gives one source for each name, and forgets the name when the last holder releases the source") {
    CountingFormat format;
    PrTextureFormatSet* formats = format.set();
    PrTextureCatalogDesc desc = prTextureCatalogDescDefault(formats);
    PrTextureCatalog* catalog = prTextureCatalogCreate(&desc);
    PrBlobSource* png = bytesOf(PrByteView{kPngSignature, sizeof(kPngSignature)});

    // Without a pool, the first ask opens the source.
    const PrTextureSource* first = nullptr;
    REQUIRE(prTextureCatalogSourceFromBytes(catalog, "a", png, &first) == PrTextureCatalogState_Ready);
    const PrTextureSource* second = nullptr;
    REQUIRE(prTextureCatalogSourceFromBytes(catalog, "a", png, &second) == PrTextureCatalogState_Ready);
    CHECK(first == second);
    CHECK(format.opened == 1);
    // Another name is another source, even with the same bytes.
    const PrTextureSource* other = nullptr;
    REQUIRE(prTextureCatalogSourceFromBytes(catalog, "b", png, &other) == PrTextureCatalogState_Ready);
    CHECK(other != first);
    CHECK(prTextureCatalogNameCount(catalog) == 2);

    // The catalog holds no reference. The two references of the callers are all that "a" has.
    CHECK(prTextureSourceReferences(first) == 2);
    prTextureSourceRelease(second);
    CHECK(prTextureCatalogNameCount(catalog) == 2);
    prTextureSourceRelease(first);
    CHECK(prTextureCatalogNameCount(catalog) == 1);
    // The name opens again.
    REQUIRE(prTextureCatalogSourceFromBytes(catalog, "a", png, &first) == PrTextureCatalogState_Ready);
    CHECK(format.opened == 3);
    prTextureSourceRelease(first);

    // A source outlives its catalog.
    prTextureCatalogDestroy(catalog);
    CHECK(prTextureSourceReferences(other) == 1);
    prTextureSourceRelease(other);
    prBlobSourceRelease(png);
    prTextureFormatSetDestroy(formats);
}

TEST_CASE("a catalog keeps a failure until its name is forgotten, and a forgotten name opens as a new source") {
    CountingFormat format;
    PrTextureFormatSet* formats = format.set();
    PrTextureCatalogDesc desc = prTextureCatalogDescDefault(formats);
    PrTextureCatalog* catalog = prTextureCatalogCreate(&desc);
    const uint8_t text[4] = {'t', 'e', 'x', 't'};
    PrBlobSource* notPng = bytesOf(PrByteView{text, sizeof(text)});
    PrBlobSource* png = bytesOf(PrByteView{kPngSignature, sizeof(kPngSignature)});

    const PrTextureSource* source = nullptr;
    CHECK(prTextureCatalogSourceFromBytes(catalog, "a", notPng, &source) == PrTextureCatalogState_Failed);
    CHECK(source == nullptr);
    CHECK(prTextureCatalogStatus(catalog, "a") == PrStatus_NotSupported);
    // The failure stands, whatever bytes a later ask brings.
    CHECK(prTextureCatalogSourceFromBytes(catalog, "a", png, &source) == PrTextureCatalogState_Failed);
    prTextureCatalogForget(catalog, "a");
    CHECK(prTextureCatalogStatus(catalog, "a") == PrStatus_Ok);
    REQUIRE(prTextureCatalogSourceFromBytes(catalog, "a", png, &source) == PrTextureCatalogState_Ready);

    // A name that is forgotten while its source lives opens as a new source. The old source stays valid.
    prTextureCatalogForget(catalog, "a");
    const PrTextureSource* again = nullptr;
    REQUIRE(prTextureCatalogSourceFromBytes(catalog, "a", png, &again) == PrTextureCatalogState_Ready);
    CHECK(again != source);
    CHECK(prTextureSourceDesc(source)->extent == PrUVec2{1, 1});
    prTextureSourceRelease(source);
    // The release of the old source leaves the name with the new source.
    CHECK(prTextureCatalogNameCount(catalog) == 1);
    prTextureSourceRelease(again);

    // A missing file fails with the status of its read.
    CHECK(prTextureCatalogSource(catalog, "no/such/file.png", &source) == PrTextureCatalogState_Failed);
    CHECK(prTextureCatalogStatus(catalog, "no/such/file.png") != PrStatus_Ok);

    prTextureCatalogDestroy(catalog);
    prBlobSourceRelease(png);
    prBlobSourceRelease(notPng);
    prTextureFormatSetDestroy(formats);
}

TEST_CASE("a catalog opens a source on its pool, tells the host, and holds the source until an ask or a trim" *
        doctest::skip(!prThreadSupported())) {
    CountingFormat format;
    PrTextureFormatSet* formats = format.set();
    PrTaskPool* pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
    PrTextureCatalogDesc desc = prTextureCatalogDescDefault(formats);
    desc.tasks = pool;
    Atomic<uint32_t> opened;
    opened.store(0);
    desc.opened = [](void* PR_NULLABLE context) { static_cast<Atomic<uint32_t>*>(context)->fetchAdd(1); };
    desc.openedContext = &opened;
    PrTextureCatalog* catalog = prTextureCatalogCreate(&desc);
    LaterBytes bytes;
    PrBlobSource* png = bytes.source();

    // The open waits for its bytes on the pool. Asks in the meantime find the name pending.
    const PrTextureSource* source = nullptr;
    CHECK(prTextureCatalogSourceFromBytes(catalog, "a", png, &source) == PrTextureCatalogState_Pending);
    CHECK(prTextureCatalogSourceFromBytes(catalog, "a", png, &source) == PrTextureCatalogState_Pending);
    bytes.complete();
    CHECK(textureCatalogSettle(catalog));
    CHECK(opened.load() == 1);
    // The catalog held the source for the ask that takes it. It no longer holds the source.
    REQUIRE(prTextureCatalogSourceFromBytes(catalog, "a", png, &source) == PrTextureCatalogState_Ready);
    CHECK(prTextureSourceReferences(source) == 1);
    prTextureSourceRelease(source);
    CHECK(prTextureCatalogNameCount(catalog) == 0);

    // If no ask comes back for an open, the trim of the first frame that does not ask for the name releases the source.
    CHECK(prTextureCatalogSourceFromBytes(catalog, "b", png, &source) == PrTextureCatalogState_Pending);
    prTextureCatalogTrim(catalog);
    bytes.complete();
    CHECK(textureCatalogSettle(catalog));
    CHECK(prTextureCatalogNameCount(catalog) == 1);
    prTextureCatalogTrim(catalog);
    CHECK(prTextureCatalogNameCount(catalog) == 0);
    CHECK(format.opened == 2);

    prTextureCatalogDestroy(catalog);
    prBlobSourceRelease(png);
    prTaskPoolDestroy(pool);
    prTextureFormatSetDestroy(formats);
}

TEST_SUITE_END();

TEST_CASE("destroying a catalog runs none of the opens that wait in the pool's queue" *
        doctest::skip(!prThreadSupported())) {
    CountingFormat format;
    PrTextureFormatSet* formats = format.set();
    PrTaskPool* pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
    PrTextureCatalogDesc desc = prTextureCatalogDescDefault(formats);
    desc.tasks = pool;
    PrTextureCatalog* catalog = prTextureCatalogCreate(&desc);
    // The first open's bytes take a while to read on the pool's one thread, and the opens of the other names wait in
    // the pool's queue behind it. Each of their reads counts.
    struct Slow {
        static void start(void* PR_NULLABLE, PrBlobRead* read) {
            prThreadSleep(30);
            prBlobReadComplete(read, PrStatus_Ok, prBlobCopy(PrByteView{kPngSignature, sizeof(kPngSignature)}), 0);
        }
    };
    PrBlobSourceDesc slowDesc{.start = Slow::start, .size = sizeof(kPngSignature)};
    PrBlobSource* slow = prBlobSourceCreate(&slowDesc);
    Atomic<uint32_t> reads;
    reads.store(0);
    PrBlobSourceDesc countedDesc{
        .start = [](void* PR_NULLABLE context, PrBlobRead* read) {
            static_cast<Atomic<uint32_t>*>(context)->fetchAdd(1);
            prBlobReadComplete(read, PrStatus_Ok, prBlobCopy(PrByteView{kPngSignature, sizeof(kPngSignature)}), 0);
        },
        .context = &reads,
        .size = sizeof(kPngSignature),
    };
    PrBlobSource* counted = prBlobSourceCreate(&countedDesc);
    const PrTextureSource* source = nullptr;
    CHECK(prTextureCatalogSourceFromBytes(catalog, "slow", slow, &source) == PrTextureCatalogState_Pending);
    for (uint32_t i = 0; i < 40; ++i) {
        char name[8] = {'n', char('0' + i / 10), char('0' + i % 10)};
        CHECK(prTextureCatalogSourceFromBytes(catalog, PrStringView{name, 3}, counted, &source) ==
            PrTextureCatalogState_Pending);
    }
    prTextureCatalogDestroy(catalog);
    // The queued opens are cancelled as the pool takes them, without reading.
    prTaskPoolDestroy(pool);
    CHECK(reads.load() == 0);
    CHECK(format.opened <= 1);
    prBlobSourceRelease(slow);
    prBlobSourceRelease(counted);
    prTextureFormatSetDestroy(formats);
}
