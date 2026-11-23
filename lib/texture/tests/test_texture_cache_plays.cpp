// The texture cache and sources whose pictures change: the picture that an ask to play is answered with on each
// timeline at each time, the arrivals that make consumers ask again, asks that do not play, what the cache asks the
// source, and what a trim stops and frees.

#include "pr/gpu.h"
#include "pr/gpu_memory.h"
#include "pr/mem.h"
#include "pr/notifier.h"
#include "pr/texture.h"
#include "pr/texture_cache.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns a source of 4 by 4 texels whose every byte is `value`.
const PrTextureSource* filled(uint8_t value) {
    uint8_t texels[4 * 4 * 4];
    prMemFill(texels, value, sizeof(texels));
    return prTextureSourceCreateCopy(PrPixelView{texels, 16, {4, 4}}, PrPixelFormat_Rgba8,
        PrTextureAlpha_Premultiplied);
}

// A source of 4 by 4 texels that shows three pictures in turn, one second each, forever. Every byte of picture `i` is
// 10 * (i + 1). The source counts the times that it is asked what it shows.
struct Flip {
    uint32_t asked = 0;
    uint32_t made = 0;                  // the pictures after the first that the source has made

    static void read(void* PR_NULLABLE, PrTextureRead* read) {
        prMemFill(prTextureReadStorage(read), 10, 4 * 4 * 4);
        prTextureReadComplete(read, PrStatus_Ok);
    }

    static void show(void* PR_NULLABLE context, double time, PrTextureShow* out) {
        Flip* flip = static_cast<Flip*>(context);
        ++flip->asked;
        uint32_t second = time > 0 ? uint32_t(time) : 0;
        uint32_t index = second % 3;
        out->serial = index;
        out->next = double(second + 1);
        if (index == 0)
            return;
        out->picture = filled(uint8_t(10 * (index + 1)));
        ++flip->made;
    }

    const PrTextureSource* source() {
        PrTextureSourceDesc desc{
            .texture = prTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{4, 4}, PrTextureAlpha_Premultiplied),
            .read = read,
            .show = show,
            .context = this,
        };
        return prTextureSourceCreate(&desc);
    }
};

// A source that changes when it is told to, as an image whose bytes arrive does: it shows `revision` until it is told
// that it is done, when it shows its own picture and says that it never changes again. It counts the times that it is
// asked what it shows.
struct Revised {
    PrNotifier* notifier = prNotifierCreate();
    uint32_t revision = 0;
    bool done = false;
    uint32_t asked = 0;

    static void read(void* PR_NULLABLE, PrTextureRead* read) {
        prMemFill(prTextureReadStorage(read), 200, 4 * 4 * 4);
        prTextureReadComplete(read, PrStatus_Ok);
    }

    static void show(void* PR_NULLABLE context, double, PrTextureShow* out) {
        Revised* revised = static_cast<Revised*>(context);
        ++revised->asked;
        if (revised->done)
            return;
        out->serial = revised->revision + 1;
        out->notifies = true;
        out->complete = false;
        out->picture = filled(uint8_t(revised->revision + 1));
    }

    // Shows the next revision, or the final picture, and notifies.
    void advance(bool last) {
        ++revision;
        done = last;
        prNotifierNotify(notifier);
    }

    const PrTextureSource* source() {
        PrTextureSourceDesc desc{
            .texture = prTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{4, 4}, PrTextureAlpha_Premultiplied),
            .read = read,
            .show = show,
            .notifier = notifier,
            .context = this,
        };
        return prTextureSourceCreate(&desc);
    }

    ~Revised() { prNotifierRelease(notifier); }
};

// Returns the first byte of the region's texels on the memory device.
uint8_t firstByte(PrGpuDevice* device, const PrTextureRegion& region) {
    PrGpuNativePixels native{};
    REQUIRE(prGpuTextureNative(device, region.texture.texture, PrGpuNativeKind_Pixels, &native));
    return native.pixels.data[size_t(region.rect.top) * native.pixels.stride + size_t(region.rect.left) * 4];
}

// Asks to play `source` on `timeline`, and lets go of the picture that the answer names. Returns the state and the
// region.
PrTextureCacheState playOf(PrTextureCache* cache, const PrTextureSource* source, PrTextureRegion* region,
        const PrTextureTimeline* PR_NULLABLE timeline = nullptr) {
    const PrTextureSource* picture = nullptr;
    PrTextureCacheState state = prTextureCachePlay(cache, source, prTextureCacheAskEncoded(0), timeline, region, &picture);
    CHECK((picture != nullptr) == (state == PrTextureCacheState_Ready || state == PrTextureCacheState_Partial));
    prTextureSourceRelease(picture);
    return state;
}

// Counts the arrivals that a cache reports to its host.
void countArrival(void* PR_NULLABLE context) {
    ++*static_cast<uint32_t*>(context);
}

// Returns a cache on `device` without a pool, which realizes inside the ask, and with a texture for each picture.
// Reports arrivals to `arrived` if it is not null.
PrTextureCache* cacheOn(PrGpuDevice* device, uint32_t* PR_NULLABLE arrived = nullptr) {
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.pack = PrTextureCachePack_Never;
    if (arrived != nullptr) {
        desc.arrived = countArrival;
        desc.arrivedContext = arrived;
    }
    return prTextureCacheCreate(&desc);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("texture cache plays");

TEST_CASE("a timeline is a time that runs at a rate from a time of the host") {
    PrTextureTimeline a = prTextureTimelineCreate(10);
    PrTextureTimeline b = prTextureTimelineCreate(10);
    CHECK(a.id != 0);
    CHECK(a.id != b.id);
    CHECK(prTextureTimelineAt(&a, 12.5) == 2.5);
    CHECK(prTextureTimelineHostTime(&a, 4) == 14);
    prTextureTimelineSetRate(&a, 12, 0);
    CHECK(prTextureTimelineAt(&a, 20) == 2);
    CHECK(prTextureTimelineHostTime(&a, 3) == kPrF64Infinity);
    CHECK(prTextureTimelineHostTime(&a, 1) == 12);
    prTextureTimelineSetRate(&a, 20, 2);
    CHECK(prTextureTimelineAt(&a, 21) == 4);
    prTextureTimelineSeek(&a, 30, 0.5);
    CHECK(prTextureTimelineAt(&a, 30.25) == 1);
}

TEST_CASE("a cache answers an ask to play with the picture that the source shows at the time of the timeline") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    Flip flip;
    const PrTextureSource* source = flip.source();
    CHECK(prTextureSourceChanges(source));
    PrTextureCacheAsk ask = prTextureCacheAskEncoded(0);
    PrTextureRegion region{};

    // The cache's own timeline starts at the first ask. A cache whose time was never set is at time zero.
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 10);
    CHECK(region.playing);
    CHECK(flip.asked == 1);
    CHECK(prTextureCacheNextFrame(cache) == 1);

    // The source is asked once for a picture, however often consumers ask before the next is due.
    prTextureCacheSetTime(cache, 0.25);
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
    CHECK(flip.asked == 1);
    prTextureCacheTrim(cache);

    // A time before the next picture is no arrival. The time of the next picture is one, so that consumers ask.
    uint64_t arrivals = prTextureCacheArrivals(cache);
    prTextureCacheSetTime(cache, 0.75);
    CHECK(prTextureCacheArrivals(cache) == arrivals);
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
    prTextureCacheTrim(cache);
    prTextureCacheSetTime(cache, 1.5);
    CHECK(prTextureCacheArrivals(cache) != arrivals);
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 20);
    CHECK(prTextureCacheNextFrame(cache) == 2);
    prTextureCacheTrim(cache);

    // An ask that does not play gets the source's own picture, and does not ask the source.
    uint32_t asked = flip.asked;
    REQUIRE(prTextureCacheRealize(cache, source, ask, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 10);
    CHECK(!region.playing);
    CHECK(flip.asked == asked);

    // After the last picture, the first shows again.
    prTextureCacheSetTime(cache, 2.5);
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 30);
    prTextureCacheTrim(cache);
    prTextureCacheSetTime(cache, 3.5);
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 10);

    prTextureCacheDestroy(cache);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(device);
}

TEST_CASE("one source plays on two timelines at once, which pause and seek apart and share the pictures they show") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    Flip flip;
    const PrTextureSource* source = flip.source();
    PrTextureRegion region{};
    PrTextureCacheAsk ask = prTextureCacheAskEncoded(0);

    // Two timelines started a second apart show different pictures at the same time of the cache.
    PrTextureTimeline early = prTextureTimelineCreate(0);
    PrTextureTimeline late = prTextureTimelineCreate(1);
    prTextureCacheSetTime(cache, 1.5);
    REQUIRE(playOf(cache, source, &region, &early) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 20);
    PrGpuTexture earlyTexture = region.texture.texture;
    REQUIRE(playOf(cache, source, &region, &late) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 10);
    // A row of thumbnails, which does not play, shows the first picture whatever the timelines show.
    REQUIRE(prTextureCacheRealize(cache, source, ask, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 10);
    prTextureCacheTrim(cache);

    // Pausing the early timeline holds its picture while the late one plays on.
    prTextureTimelineSetRate(&early, 1.5, 0);
    prTextureCacheSetTime(cache, 2.5);
    REQUIRE(playOf(cache, source, &region, &early) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 20);
    CHECK(region.texture.texture.id == earlyTexture.id);
    // The late timeline now shows the picture that the early one shows. The two share its texture, though the source
    // made the picture again for the second timeline.
    REQUIRE(playOf(cache, source, &region, &late) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 20);
    CHECK(region.texture.texture.id == earlyTexture.id);
    // A paused timeline adds no time at which the cache draws again.
    CHECK(prTextureCacheNextFrame(cache) == 3);
    prTextureCacheTrim(cache);

    // Seeking back shows the earlier picture, however far the timeline had played.
    prTextureTimelineSeek(&late, 2.5, 0.25);
    REQUIRE(playOf(cache, source, &region, &late) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 10);
    REQUIRE(playOf(cache, source, &region, &early) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 20);

    prTextureCacheDestroy(cache);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a source that notifies is asked again, and once it says that it never changes, it is not asked again") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    uint32_t arrived = 0;
    PrTextureCache* cache = cacheOn(device, &arrived);
    Revised revised;
    const PrTextureSource* source = revised.source();
    PrTextureRegion region{};

    // A picture that is not complete is answered as Partial, and adds no time at which the cache draws again.
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Partial);
    CHECK(firstByte(device, region) == 1);
    CHECK(prTextureCacheNextFrame(cache) == 0);
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Partial);
    CHECK(revised.asked == 1);

    // A notification is an arrival, and the next ask asks the source again.
    uint64_t arrivals = prTextureCacheArrivals(cache);
    uint32_t reported = arrived;
    revised.advance(false);
    CHECK(arrived == reported + 1);
    CHECK(prTextureCacheArrivals(cache) != arrivals);
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Partial);
    CHECK(firstByte(device, region) == 2);
    CHECK(revised.asked == 2);
    prTextureCacheTrim(cache);

    // The final picture is the source's own, so it shares the texture of an ask that does not play.
    revised.advance(true);
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 200);
    PrTextureRegion still{};
    REQUIRE(prTextureCacheRealize(cache, source, prTextureCacheAskEncoded(0), &still) == PrTextureCacheState_Ready);
    CHECK(still.texture.texture.id == region.texture.texture.id);

    // From then on, asks and frames cost the source nothing, and the cache wakes the host for nothing.
    uint32_t asked = revised.asked;
    arrivals = prTextureCacheArrivals(cache);
    for (uint32_t frame = 0; frame < 100; ++frame) {
        prTextureCacheSetTime(cache, frame * 0.1);
        REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
        prTextureCacheTrim(cache);
    }
    CHECK(revised.asked == asked);
    CHECK(prTextureCacheArrivals(cache) == arrivals);
    CHECK(prTextureCacheNextFrame(cache) == 0);

    prTextureCacheDestroy(cache);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(device);
}

TEST_CASE("a source that nothing asks for any more stops, and the textures of the pictures that are past are freed") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    Flip flip;
    const PrTextureSource* source = flip.source();
    PrTextureRegion region{};
    PrTextureTimeline timeline = prTextureTimelineCreate(0);
    // The source plays through four pictures, a frame for each.
    for (uint32_t second = 0; second < 4; ++second) {
        prTextureCacheSetTime(cache, second + 0.5);
        REQUIRE(playOf(cache, source, &region, &timeline) == PrTextureCacheState_Ready);
        prTextureCacheTrim(cache);
    }
    // The cache keeps the first picture, the picture that shows and the one before it. The budget is not what frees the
    // others: the cache is far under it.
    CHECK(flip.made == 2);
    CHECK(prTextureCacheInfo(cache).textures <= 3);
    CHECK(prTextureCacheNextFrame(cache) == 4);

    // The source plays for a second after the last ask for it, so a frame without an ask does not stop it.
    prTextureCacheSetTime(cache, 3.75);
    prTextureCacheTrim(cache);
    prTextureCacheTrim(cache);
    CHECK(prTextureCacheNextFrame(cache) == 4);
    // After that second it stops. Nothing is due any more, and a later time is no arrival.
    prTextureCacheSetTime(cache, 5);
    prTextureCacheTrim(cache);
    CHECK(prTextureCacheNextFrame(cache) == 0);
    uint64_t arrivals = prTextureCacheArrivals(cache);
    prTextureCacheSetTime(cache, 9);
    CHECK(prTextureCacheArrivals(cache) == arrivals);
    prTextureCacheTrim(cache);
    CHECK(prTextureCacheInfo(cache).textures == 1);

    // The cache moves to another device with a source playing, and plays it there at the same picture, without asking
    // the source again.
    prTextureCacheSetTime(cache, 10.5);
    REQUIRE(playOf(cache, source, &region, &timeline) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 20);
    uint32_t asked = flip.asked;
    PrGpuDevice* next = prGpuMemoryCreate(nullptr);
    prTextureCacheSetDevice(cache, next);
    REQUIRE(playOf(cache, source, &region, &timeline) == PrTextureCacheState_Ready);
    CHECK(firstByte(next, region) == 20);
    CHECK(flip.asked == asked);

    prTextureCacheDestroy(cache);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(next);
    prGpuDeviceDestroy(device);
}

TEST_SUITE_END();
