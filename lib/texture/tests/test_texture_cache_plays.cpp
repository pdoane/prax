// The texture cache and sources that change over time: the picture that an ask to play is answered with at each time,
// the arrivals that make consumers ask again, asks that do not play, and what a trim stops and frees.

#include "pr/gpu.h"
#include "pr/gpu_memory.h"
#include "pr/mem.h"
#include "pr/texture.h"
#include "pr/texture_cache.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A source of 4 by 4 texels that shows three pictures in turn, one second each, forever. Every byte of picture `i` is
// 10 * (i + 1). The source counts the times that it is asked what it shows.
struct Flip {
    uint32_t asked = 0;
    uint32_t made = 0;                  // the pictures after the first that the source has made

    static void read(void* PR_NULLABLE, PrTextureRead* read) {
        prMemFill(prTextureReadStorage(read), 10, 4 * 4 * 4);
        prTextureReadComplete(read, PrStatus_Ok);
    }

    static void frame(void* PR_NULLABLE context, double time, PrTextureFrame* out) {
        Flip* flip = static_cast<Flip*>(context);
        ++flip->asked;
        uint32_t second = uint32_t(time);
        uint32_t index = second % 3;
        out->serial = index + 1;
        out->next = double(second + 1);
        if (index == 0)
            return;
        uint8_t texels[4 * 4 * 4];
        prMemFill(texels, uint8_t(10 * (index + 1)), sizeof(texels));
        out->source = prTextureSourceCreateCopy(PrPixelView{texels, 16, {4, 4}}, PrPixelFormat_Rgba8,
            PrTextureAlpha_Premultiplied);
        ++flip->made;
    }

    const PrTextureSource* source() {
        PrTextureSourceDesc desc{
            .texture = prTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{4, 4}, PrTextureAlpha_Premultiplied),
            .read = read,
            .frame = frame,
            .context = this,
        };
        return prTextureSourceCreate(&desc);
    }
};

// Returns the first byte of the region's texels on the memory device.
uint8_t firstByte(PrGpuDevice* device, const PrTextureRegion& region) {
    PrGpuNativePixels native{};
    REQUIRE(prGpuTextureNative(device, region.texture.texture, PrGpuNativeKind_Pixels, &native));
    return native.pixels.data[size_t(region.rect.top) * native.pixels.stride + size_t(region.rect.left) * 4];
}

// Asks to play `source`, and lets go of the picture that the answer names. Returns the state and the region.
PrTextureCacheState playOf(PrTextureCache* cache, const PrTextureSource* source, PrTextureRegion* region) {
    const PrTextureSource* picture = nullptr;
    PrTextureCacheState state = prTextureCachePlay(cache, source, prTextureCacheAskEncoded(0), region, &picture);
    CHECK((picture != nullptr) == (state == PrTextureCacheState_Ready || state == PrTextureCacheState_Partial));
    prTextureSourceRelease(picture);
    return state;
}

// Returns a cache on `device` without a pool, which realizes inside the ask, and with a texture for each picture.
PrTextureCache* cacheOn(PrGpuDevice* device) {
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.pack = PrTextureCachePack_Never;
    return prTextureCacheCreate(&desc);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("texture cache plays");

TEST_CASE("a cache answers an ask for a source that changes over time with the picture that the source shows then") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    Flip flip;
    const PrTextureSource* source = flip.source();
    CHECK(prTextureSourcePlays(source));
    PrTextureCacheAsk ask = prTextureCacheAskEncoded(0);
    PrTextureRegion region{};

    // A cache whose time was never set plays nothing, and answers with the first picture.
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 10);
    CHECK(!region.playing);
    CHECK(flip.asked == 0);
    CHECK(prTextureCacheNextFrame(cache) == 0);

    // At a time, the answer is the picture of that time, and the cache says when the next one is due.
    prTextureCacheSetTime(cache, 0.25);
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 10);
    CHECK(region.playing);
    CHECK(prTextureCacheNextFrame(cache) == 1);
    // The source is asked once for a picture, however often consumers ask before the next is due.
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
    CHECK(region.playing);
    CHECK(prTextureCacheNextFrame(cache) == 2);
    prTextureCacheTrim(cache);

    // An ask that does not play gets the first picture, and does not ask the source.
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

TEST_CASE("a source that nothing asks for any more stops, and the textures of the pictures that are past are freed") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    Flip flip;
    const PrTextureSource* source = flip.source();
    PrTextureRegion region{};
    // The source plays through four pictures, a frame for each.
    for (uint32_t second = 0; second < 4; ++second) {
        prTextureCacheSetTime(cache, second + 0.5);
        REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
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

    // The cache moves to another device with a source playing, and plays it there.
    prTextureCacheSetTime(cache, 10.5);
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(device, region) == 20);
    PrGpuDevice* next = prGpuMemoryCreate(nullptr);
    prTextureCacheSetDevice(cache, next);
    REQUIRE(playOf(cache, source, &region) == PrTextureCacheState_Ready);
    CHECK(firstByte(next, region) == 20);

    prTextureCacheDestroy(cache);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(next);
    prGpuDeviceDestroy(device);
}

TEST_SUITE_END();
