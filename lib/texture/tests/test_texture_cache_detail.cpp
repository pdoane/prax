// Tests of the versions and the ordering of the texture cache. They cover the preview that the detail of an ask
// realizes, the smaller version that an ask returns in the meantime, the order in which waiting realizations start, the
// realizations that a trim cancels, and sources that fill textures themselves.

#include "pr/array.h"
#include "pr/gpu.h"
#include "pr/gpu_memory.h"
#include "pr/mem.h"
#include "pr/mutex.h"
#include "pr/task.h"
#include "pr/texture.h"
#include "pr/texture_cache.h"
#include "pr/thread.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// The reads of the sources of a test, in the order that they began. The providers of the sources record the reads on
// any thread.
struct ReadLog {
    PrMutex lock;
    Array<uint32_t> began;                  // source id of each read
    Array<PrTextureRead*> waiting;          // reads that their sources deferred

    ReadLog() { prMutexInit(&lock); }
    ~ReadLog() { prMutexReset(&lock); }

    uint32_t count() {
        prMutexLock(&lock);
        uint32_t count = began.count();
        prMutexUnlock(&lock);
        return count;
    }

    uint32_t at(uint32_t index) {
        prMutexLock(&lock);
        uint32_t id = began[index];
        prMutexUnlock(&lock);
        return id;
    }

    // Blocks until `count` reads have begun, or five seconds pass.
    void waitFor(uint32_t wanted) {
        for (uint32_t i = 0; i < 5000 && count() < wanted; ++i)
            prThreadSleep(1);
    }
};

// A square Rgba8 picture with a side of `side` texels. Every byte of every texel is `fill`. The log records the reads
// of the picture. A deferred picture completes its reads when the test calls `complete`.
struct Picture {
    ReadLog* log;
    uint32_t id;
    uint32_t side;
    uint8_t fill;
    bool deferred = false;
    bool fills = false;                     // true if the provider writes the texture of a read itself
    uint32_t filled = 0;                    // number of reads into a texture that the provider served

    static void write(Picture* picture, PrTextureRead* read) {
        const PrTextureReadDesc& asked = *prTextureReadInfo(read);
        size_t size = size_t(picture->side) * picture->side * 4;
        if (asked.device != nullptr) {
            // The provider's own path to the device. It uploads texels that it creates.
            Array<uint8_t> texels;
            texels.resize(uint32_t(size));
            prMemFill(texels.begin(), picture->fill, size);
            PrGpuTextureRegion region{
                .x = uint32_t(asked.origin.x),
                .y = uint32_t(asked.origin.y),
                .extent = {picture->side, picture->side, 1},
            };
            prGpuTextureUpload(asked.device, asked.texture, &region, texels.begin(), picture->side * 4, 0);
            ++picture->filled;
        } else {
            prMemFill(prTextureReadStorage(read), picture->fill, size);
        }
        prTextureReadComplete(read, PrStatus_Ok);
    }

    static void read(void* PR_NULLABLE context, PrTextureRead* read) {
        Picture* picture = static_cast<Picture*>(context);
        prMutexLock(&picture->log->lock);
        picture->log->began.push(picture->id);
        bool defer = picture->deferred;
        if (defer)
            picture->log->waiting.push(read);
        prMutexUnlock(&picture->log->lock);
        if (!defer)
            write(picture, read);
    }

    // Completes the picture's deferred reads.
    void complete() {
        prMutexLock(&log->lock);
        Array<PrTextureRead*> mine;
        for (uint32_t i = log->waiting.count(); i-- > 0;) {
            if (prTextureSourceDesc(prTextureReadSource(log->waiting[i]))->extent.x == side) {
                mine.push(log->waiting[i]);
                log->waiting.erase(i);
            }
        }
        prMutexUnlock(&log->lock);
        for (PrTextureRead* waiting : mine)
            write(this, waiting);
    }

    // Returns a source of the picture, with `previews`, smallest first.
    const PrTextureSource* source(const PrTextureSource* PR_NONNULL const* PR_NULLABLE previews = nullptr,
            uint32_t previewCount = 0) {
        PrTextureSourceDesc desc{
            .texture = prTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{side, side}, PrTextureAlpha_Premultiplied),
            .flags = PrTextureSourceFlags(fills ? PrTextureSourceFlag_FillsTextures : 0),
            .read = read,
            .previews = previews,
            .previewCount = previewCount,
            .context = this,
        };
        return prTextureSourceCreate(&desc);
    }
};

// A picture of 1024 texels with previews of 64 and 256 texels. The fills of its versions are 1, 2 and 3, starting at
// the smallest.
struct Versions {
    ReadLog log;
    Picture small{&log, 0, 64, 1};
    Picture medium{&log, 1, 256, 2};
    Picture whole{&log, 2, 1024, 3};
    const PrTextureSource* source;

    Versions() {
        const PrTextureSource* previews[2] = {small.source(), medium.source()};
        source = whole.source(previews, 2);
        prTextureSourceRelease(previews[0]);
        prTextureSourceRelease(previews[1]);
    }

    ~Versions() { prTextureSourceRelease(source); }
};

PrTextureCacheAsk askOf(uint32_t detail, PrTextureCachePriority priority = PrTextureCachePriority_Normal) {
    PrTextureCacheAsk ask = prTextureCacheAskEncoded(0);
    ask.detail = detail;
    ask.priority = priority;
    return ask;
}

// Returns the first byte of the first texel of the region, on a memory device.
uint8_t firstByte(PrGpuDevice* device, const PrTextureRegion& region) {
    PrGpuNativePixels native{};
    REQUIRE(prGpuTextureNative(device, region.texture.texture, PrGpuNativeKind_Pixels, &native));
    return native.pixels.data[size_t(region.rect.top) * native.pixels.stride + size_t(region.rect.left) * 4];
}

// Returns a cache on `device` that never packs, so that each version is a texture with its own extent. The cache
// realizes on `pool`, one source at a time.
PrTextureCache* cacheOn(PrGpuDevice* device, PrTaskPool* PR_NULLABLE pool = nullptr) {
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.pack = PrTextureCachePack_Never;
    desc.tasks = pool;
    desc.concurrency = 1;
    return prTextureCacheCreate(&desc);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("texture");

TEST_CASE("an ask realizes the smallest version of a source with enough detail, or uses a larger realized version") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    Versions v;

    // A thumbnail reads the smallest preview alone. The region holds the whole picture in the preview's texels.
    PrTextureRegion region{};
    REQUIRE(prTextureCacheRealize(cache, v.source, askOf(48), &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{64, 64});
    CHECK(region.source == PrUVec2{1024, 1024});
    CHECK(firstByte(device, region) == 1);
    CHECK(v.log.count() == 1);
    PrRect half = prTextureRegionRect(&region, PrRect{0, 0, 512, 1024}, 0);
    CHECK(half.right == 32);
    CHECK(half.bottom == 64);

    // After the whole picture is realized, it serves an ask of less detail that no smaller version serves.
    REQUIRE(prTextureCacheRealize(cache, v.source, askOf(0), &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{1024, 1024});
    CHECK(v.log.count() == 2);
    REQUIRE(prTextureCacheRealize(cache, v.source, askOf(200), &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{1024, 1024});
    CHECK(v.log.count() == 2);
    // The smallest still serves the thumbnail.
    REQUIRE(prTextureCacheRealize(cache, v.source, askOf(48), &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{64, 64});

    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("an ask returns a smaller version that has arrived while the version that it needs is pending") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    Versions v;
    PrTextureRegion region{};
    REQUIRE(prTextureCacheRealize(cache, v.source, askOf(48), &region) == PrTextureCacheState_Ready);
    v.whole.deferred = true;
    CHECK(prTextureCacheRealize(cache, v.source, askOf(0), &region) == PrTextureCacheState_Partial);
    CHECK(region.extent == PrUVec2{64, 64});
    CHECK(firstByte(device, region) == 1);
    CHECK(prTextureCacheInfo(cache).inFlight == 1);
    v.whole.complete();
    REQUIRE(prTextureCacheRealize(cache, v.source, askOf(0), &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{1024, 1024});
    CHECK(firstByte(device, region) == 3);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a cache with a pool reads a source's smallest preview first, so that an ask has something to return" *
        doctest::skip(!prThreadSupported())) {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTaskPool* pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
    PrTextureCache* cache = cacheOn(device, pool);
    Versions v;
    PrTextureRegion region{};
    CHECK(prTextureCacheRealize(cache, v.source, askOf(0), &region) == PrTextureCacheState_Pending);
    prTextureCacheWaitAll(cache);
    // The pool is one thread wide, so the two realizations ran in the order that the cache started them.
    REQUIRE(v.log.count() == 2);
    CHECK(v.log.at(0) == 0);
    CHECK(v.log.at(1) == 2);
    REQUIRE(prTextureCacheRealize(cache, v.source, askOf(0), &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{1024, 1024});
    prTextureCacheDestroy(cache);
    prTaskPoolDestroy(pool);
    prGpuDeviceDestroy(device);
}

TEST_CASE("waiting realizations start by priority, then by the frame that asked, and a trim cancels the stale ones" *
        doctest::skip(!prThreadSupported())) {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTaskPool* pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
    PrTextureCache* cache = cacheOn(device, pool);
    ReadLog log;
    // The first picture holds the pool's one place in flight until the test completes its read.
    Picture blocker{&log, 0, 8, 9};
    blocker.deferred = true;
    Picture low{&log, 1, 8, 1};
    Picture normal{&log, 2, 8, 2};
    Picture stale{&log, 3, 8, 3};
    Picture high{&log, 4, 8, 4};
    const PrTextureSource* sources[5] = {
        blocker.source(), low.source(), normal.source(), stale.source(), high.source(),
    };
    PrTextureRegion region{};
    CHECK(prTextureCacheRealize(cache, sources[0], askOf(0), &region) == PrTextureCacheState_Pending);
    log.waitFor(1);
    REQUIRE(log.count() == 1);

    // The asks of the first frame wait, because the limit of one realization in flight is reached.
    CHECK(prTextureCacheRealize(cache, sources[1], askOf(0, PrTextureCachePriority_Low), &region) ==
        PrTextureCacheState_Pending);
    CHECK(prTextureCacheRealize(cache, sources[3], askOf(0), &region) == PrTextureCacheState_Pending);
    CHECK(prTextureCacheInfo(cache).waiting == 2);
    CHECK(log.count() == 1);
    prTextureCacheTrim(cache);
    // The second frame asks again for the low one, and for a new one. It leaves out the stale one, and the trim of the
    // frame cancels that one.
    CHECK(prTextureCacheRealize(cache, sources[1], askOf(0, PrTextureCachePriority_Low), &region) ==
        PrTextureCacheState_Pending);
    CHECK(prTextureCacheRealize(cache, sources[2], askOf(0), &region) == PrTextureCacheState_Pending);
    prTextureCacheTrim(cache);
    PrTextureCacheInfo info = prTextureCacheInfo(cache);
    CHECK(info.waiting == 2);
    CHECK(info.cancelled == 1);

    // A high priority starts at once, past the limit.
    CHECK(prTextureCacheRealize(cache, sources[4], askOf(0, PrTextureCachePriority_High), &region) ==
        PrTextureCacheState_Pending);
    log.waitFor(2);
    REQUIRE(log.count() == 2);
    CHECK(log.at(1) == 4);

    // The place that the blocker frees goes to the normal one, and then to the low one.
    blocker.complete();
    prTextureCacheWaitAll(cache);
    REQUIRE(log.count() == 4);
    CHECK(log.at(2) == 2);
    CHECK(log.at(3) == 1);
    // The cancelled one was never read. A later ask queues it again.
    CHECK(prTextureCacheRealize(cache, sources[3], askOf(0), &region) == PrTextureCacheState_Pending);
    prTextureCacheWaitAll(cache);
    CHECK(log.count() == 5);
    CHECK(prTextureCacheRealize(cache, sources[3], askOf(0), &region) == PrTextureCacheState_Ready);

    prTextureCacheDestroy(cache);
    for (const PrTextureSource* source : sources)
        prTextureSourceRelease(source);
    prTaskPoolDestroy(pool);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a source that fills textures writes its realization itself, in its own texture or in a slot of a page") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    ReadLog log;
    Picture picture{&log, 0, 16, 7};
    picture.fills = true;
    const PrTextureSource* source = picture.source();
    PrTextureCache* cache = cacheOn(device);
    PrTextureRegion region{};
    REQUIRE(prTextureCacheRealize(cache, source, askOf(0), &region) == PrTextureCacheState_Ready);
    CHECK(picture.filled == 1);
    CHECK(firstByte(device, region) == 7);
    prTextureCacheDestroy(cache);

    // In a shared page, the provider fills the slot at the read's origin.
    PrTextureCacheDesc packed = prTextureCacheDescDefault(device);
    packed.pack = PrTextureCachePack_Always;
    cache = prTextureCacheCreate(&packed);
    Picture first{&log, 1, 16, 5};
    const PrTextureSource* before = first.source();
    REQUIRE(prTextureCacheRealize(cache, before, askOf(0), &region) == PrTextureCacheState_Ready);
    REQUIRE(prTextureCacheRealize(cache, source, askOf(0), &region) == PrTextureCacheState_Ready);
    CHECK(picture.filled == 2);
    CHECK(prTextureCacheInfo(cache).slots == 2);
    CHECK(region.rect.left + region.rect.top > 0);
    CHECK(firstByte(device, region) == 7);
    prTextureCacheDestroy(cache);

    // A read into memory still reaches the provider, which fills the storage.
    PrTextureReadDesc desc = prTextureReadDescAll(prTextureSourceDesc(source));
    PrTextureRead* read = prTextureReadCreate(source, &desc);
    REQUIRE(prTextureReadStatus(read) == PrStatus_Ok);
    CHECK(prTextureReadStorage(read)[0] == 7);
    prTextureReadDestroy(read);
    CHECK(picture.filled == 2);

    prTextureSourceRelease(before);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a read into a texture leaves no bits in memory, and after a device loss the sources are read again") {
    ReadLog log;
    Picture picture{&log, 0, 32, 6};
    const PrTextureSource* source = picture.source();
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);

    // For a source that does not fill textures, the library serves the read. It uploads the bits and frees them before
    // the read completes.
    PrTextureDesc described = *prTextureSourceDesc(source);
    PrGpuTextureDesc gpu = prGpuTextureDesc2d(described.format, described.extent, PrGpuTextureUsageFlag_Sampled);
    PrGpuTexture texture{};
    REQUIRE(prGpuTextureCreate(device, &gpu, &texture) == PrStatus_Ok);
    PrTextureReadDesc desc = prTextureReadDescAll(&described);
    desc.device = device;
    desc.texture = texture;
    PrTextureRead* read = prTextureReadCreate(source, &desc);
    REQUIRE(prTextureReadStatus(read) == PrStatus_Ok);
    CHECK(prTextureReadStorage(read) == nullptr);
    prTextureReadDestroy(read);
    PrGpuNativePixels native{};
    REQUIRE(prGpuTextureNative(device, texture, PrGpuNativeKind_Pixels, &native));
    CHECK(native.pixels.data[0] == 6);
    prGpuTextureRelease(device, texture);

    PrTextureCache* cache = cacheOn(device);
    PrTextureRegion region{};
    REQUIRE(prTextureCacheRealize(cache, source, askOf(0), &region) == PrTextureCacheState_Ready);
    CHECK(log.count() == 2);
    // The device is lost. A realization on it fails as Lost. After the cache moves to a new device, it reads the source
    // again there. The source is all that the cache kept of the texture.
    prGpuDeviceSimulateLoss(device);
    Picture later{&log, 1, 32, 8};
    const PrTextureSource* afterLoss = later.source();
    CHECK(prTextureCacheRealize(cache, afterLoss, askOf(0), &region) == PrTextureCacheState_Failed);
    CHECK(prTextureCacheStatus(cache, afterLoss, askOf(0)) == PrStatus_Lost);
    // The host takes the cache off the lost device before it has the next one. The cache frees its textures, and
    // answers Pending without a read.
    uint64_t generation = prTextureCacheGeneration(cache);
    prTextureCacheSetDevice(cache, nullptr);
    CHECK(prTextureCacheDevice(cache) == nullptr);
    CHECK(prTextureCacheGeneration(cache) == generation + 1);
    CHECK(prTextureCacheInfo(cache).textures == 0);
    CHECK(prTextureCacheRealize(cache, source, askOf(0), &region) == PrTextureCacheState_Pending);
    CHECK(log.count() == 2);
    prTextureCacheTrim(cache);
    PrGpuDevice* next = prGpuMemoryCreate(nullptr);
    prTextureCacheSetDevice(cache, next);
    CHECK(prTextureCacheGeneration(cache) == generation + 2);
    REQUIRE(prTextureCacheRealize(cache, source, askOf(0), &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(next, region) == 6);
    CHECK(log.count() == 3);
    REQUIRE(prTextureCacheRealize(cache, afterLoss, askOf(0), &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(next, region) == 8);

    prTextureCacheDestroy(cache);
    prTextureSourceRelease(afterLoss);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(next);
    prGpuDeviceDestroy(device);
}

TEST_CASE("for an ask that samples mip levels, the cache makes the levels only if the ask minifies its version") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    Versions v;
    PrTextureCacheAsk ask = askOf(64);
    ask.flags = PrTextureCacheFlag_Levels;
    // An ask that shows every texel of the smallest preview gets that preview with its one level.
    PrTextureRegion region{};
    REQUIRE(prTextureCacheRealize(cache, v.source, ask, &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{64, 64});
    CHECK(region.levelCount == 1);
    // The ask shows 200 texels of the preview of 256. The preview is minified, so the cache generates its levels.
    ask.detail = 200;
    REQUIRE(prTextureCacheRealize(cache, v.source, ask, &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{256, 256});
    CHECK(region.levelCount == 9);
    // An ask that shows every texel of the same preview minifies nothing. The texture with levels serves it.
    ask.detail = 256;
    REQUIRE(prTextureCacheRealize(cache, v.source, ask, &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{256, 256});
    CHECK(prTextureCacheInfo(cache).textures == 2);
    // Without a detail, an ask that samples levels gets every level of the picture.
    ask.detail = 0;
    REQUIRE(prTextureCacheRealize(cache, v.source, ask, &region) == PrTextureCacheState_Ready);
    CHECK(region.extent == PrUVec2{1024, 1024});
    CHECK(region.levelCount == 11);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a trim over the budget evicts the least recently asked textures first, and keeps those with held sources") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.pack = PrTextureCachePack_Never;
    // Each picture is 8 by 8 texels of four bytes. The budget holds 100 of the 400.
    constexpr uint32_t kCount = 400;
    constexpr uint64_t kBytes = 8 * 8 * 4;
    desc.budget = 100 * kBytes;
    PrTextureCache* cache = prTextureCacheCreate(&desc);
    ReadLog log;
    // The array has its full size before a source takes the address of one of its pictures.
    Array<Picture> pictures(kCount);
    Array<const PrTextureSource*> sources;
    PrTextureRegion region{};
    for (uint32_t i = 0; i < kCount; ++i) {
        pictures[i] = Picture{&log, i, 8, uint8_t(i)};
        sources.push(pictures[i].source());
        REQUIRE(prTextureCacheRealize(cache, sources[i], askOf(0), &region) == PrTextureCacheState_Ready);
        // Each picture is asked for in a frame of its own, so the first is the least recently asked for.
        prTextureCacheTrim(cache);
    }
    // The test holds every source, so nothing was evicted.
    CHECK(prTextureCacheInfo(cache).textures == kCount);
    // The test releases all but the first 50 sources. The trims that follow find the released sources a few at a time.
    // They evict, starting at the least recently asked for, until the cache fits its budget.
    for (uint32_t i = 50; i < kCount; ++i)
        prTextureSourceRelease(sources[i]);
    for (uint32_t trim = 0; trim < kCount; ++trim)
        prTextureCacheTrim(cache);
    PrTextureCacheInfo info = prTextureCacheInfo(cache);
    CHECK(info.textures == 100);
    CHECK(info.bytes == 100 * kBytes);
    // The 50 that are held stay. The other 50 that remain are the most recently asked for.
    REQUIRE(prTextureCacheRealize(cache, sources[0], askOf(0), &region) == PrTextureCacheState_Ready);
    CHECK(log.count() == kCount);
    for (uint32_t i = 0; i < 50; ++i)
        prTextureSourceRelease(sources[i]);
    prTextureCacheDestroy(cache);
    prGpuDeviceDestroy(device);
}

TEST_SUITE_END();
