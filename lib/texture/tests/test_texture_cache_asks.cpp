// Tests of the texture that the cache realizes from a source for each ask and device. They cover the format of the
// texture, its levels, and the asks that share a texture.

#include "pr/array.h"
#include "pr/gpu.h"
#include "pr/gpu_memory.h"
#include "pr/gpu_select.h"
#include "pr/mem.h"
#include "pr/texture.h"
#include "pr/texture_cache.h"

#include "tools/harness/harness_gpu.h"
#include "tools/harness/texture_await.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// A source of bits in memory. It produces the formats that a test lists, each with its own texels, and it counts its
// reads. It answers a need with its own blocks if the need takes them, with R8 for a need of the red channel of data if
// it lists R8, and otherwise with its own pixels or the first pixels that it lists.
struct FakeSource {
    PrTextureDesc desc{};
    Array<PrGpuFormat> formats;
    uint32_t reads = 0;

    static void read(void* PR_NULLABLE context, PrTextureRead* read) {
        FakeSource* fake = static_cast<FakeSource*>(context);
        ++fake->reads;
        const PrTextureDesc& desc = *prTextureSourceDesc(prTextureReadSource(read));
        size_t size = prTextureReadSize(&desc, prTextureReadInfo(read));
        // Each byte of the texels is its offset, so that a test can tell the channels apart.
        for (size_t i = 0; i < size; ++i)
            prTextureReadStorage(read)[i] = uint8_t(i * 3 + 1);
        prTextureReadComplete(read, PrStatus_Ok);
    }

    static PrStatus reformat(void* PR_NULLABLE context, PrGpuFormat format,
            const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
        FakeSource* fake = static_cast<FakeSource*>(context);
        bool listed = false;
        for (PrGpuFormat other : fake->formats)
            listed = listed || other == format;
        if (!listed)
            return PrStatus_NotSupported;
        PrTextureDesc desc = fake->desc;
        desc.format = format;
        PrTextureSourceDesc made{.texture = desc, .read = read, .context = fake};
        *out = prTextureSourceCreate(&made);
        return PrStatus_Ok;
    }

    static PrStatus answer(void* PR_NULLABLE context, const PrTextureSource* own, const PrTextureNeed* need,
            const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
        FakeSource* fake = static_cast<FakeSource*>(context);
        const PrTextureDesc& desc = fake->desc;
        if (prTextureNeedTakesBlocks(need, desc.format, desc.alpha, desc.extent) ||
                (!prGpuFormatIsCompressed(desc.format) && prTextureNeedSingleChannel(need) != 0)) {
            prTextureSourceRetain(own);
            *out = own;
            return PrStatus_Ok;
        }
        for (PrGpuFormat format : fake->formats) {
            if (format == PrGpuFormat_R8 && prTextureNeedSingleChannel(need) == 0)
                return reformat(context, format, out);
        }
        if (!prGpuFormatIsCompressed(desc.format)) {
            prTextureSourceRetain(own);
            *out = own;
            return PrStatus_Ok;
        }
        for (PrGpuFormat format : fake->formats) {
            if (!prGpuFormatIsCompressed(format))
                return reformat(context, format, out);
        }
        return PrStatus_NotSupported;
    }

    const PrTextureSource* source() {
        PrTextureSourceDesc made{
            .texture = desc,
            .read = read,
            .reformat = reformat,
            .answer = answer,
            .context = this,
        };
        return prTextureSourceCreate(&made);
    }
};

// Returns a fake source of pixels with `extent` and `levels` levels. It decodes to Rgba8, R8 and floats.
FakeSource pixels(PrUVec2 extent, uint32_t levels = 1, PrGpuFormat format = PrGpuFormat_Rgba8,
        PrTextureAlpha alpha = PrTextureAlpha_Straight) {
    FakeSource fake;
    fake.desc = PrTextureDesc{
        .kind = PrGpuTextureKind_2d,
        .extent = extent,
        .depth = 1,
        .layerCount = 1,
        .levelCount = levels,
        .format = format,
        .alpha = alpha,
    };
    const PrGpuFormat decoded[4] = {PrGpuFormat_R8, PrGpuFormat_Rgba8, PrGpuFormat_Rgba16F, PrGpuFormat_Rgba32F};
    for (PrGpuFormat other : decoded) {
        if (other != format)
            fake.formats.push(other);
    }
    return fake;
}

// Returns an ask for every level in straight alpha, with `values` and `channels`. lib/render makes asks of this form.
PrTextureCacheAsk askOf(PrTextureValues values, PrTextureChannelFlags channels = 0) {
    return PrTextureCacheAsk{
        .flags = PrTextureCacheFlag_Levels,
        .values = values,
        .alpha = PrTextureAlpha_Straight,
        .channels = channels,
    };
}

// Returns the description of the texture that `cache` realizes from `source` for `ask`. The realization must arrive.
PrGpuTextureDesc realized(PrTextureCache* cache, const PrTextureSource* source, PrTextureCacheAsk ask,
        PrTextureRegion* out = nullptr) {
    PrTextureRegion region{};
    REQUIRE(textureCacheAwait(cache, source, ask, &region) == PrTextureCacheState_Ready);
    if (out != nullptr)
        *out = region;
    return prGpuTextureDesc(prTextureCacheDevice(cache), region.texture.texture);
}

// Returns the device on which a test samples blocks, or null in a run without a GPU (harness::gpuTests). In a run with
// a GPU, the test case fails if the device cannot be created. It does not pass without the device.
PrGpuDevice* PR_NULLABLE blockDevice() {
    static PrGpuDevice* device = [] {
        PrGpuDevice* made = nullptr;
        if (gpuTests())
            gpuDeviceCreate(PrGpuBackend_Default, nullptr, &made);
        return made;
    }();
    if (gpuTests())
        REQUIRE_MESSAGE(device != nullptr, "no GPU device for the tests; set PR_TEST_NO_GPU to run without a GPU");
    return device;
}

PrTextureCache* cacheOn(PrGpuDevice* device) {
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    return prTextureCacheCreate(&desc);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("texture cache asks");

TEST_CASE("color gets the Srgb form of the pixels, and data gets their stored values, each in its own texture") {
    FakeSource fake = pixels({4, 4});
    const PrTextureSource* source = fake.source();
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    PrTextureRegion color{};
    PrTextureRegion data{};
    PrGpuTextureDesc colorDesc = realized(cache, source, askOf(PrTextureValues_Color), &color);
    PrGpuTextureDesc dataDesc = realized(cache, source, askOf(PrTextureValues_Data), &data);
    CHECK(colorDesc.format == PrGpuFormat_Rgba8Srgb);
    CHECK(dataDesc.format == PrGpuFormat_Rgba8);
    CHECK(color.texture.texture.id != data.texture.texture.id);
    // Each texture has every level, and each realization read the bits of the source for itself.
    CHECK(colorDesc.levels == 3);
    CHECK(dataDesc.levels == 3);
    CHECK(fake.reads == 2);
    prTextureCacheDestroy(cache);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(device);
}

TEST_CASE("encoded 8-bit values share the color texture through its plain form, in either alpha for an opaque source") {
    FakeSource fake = pixels({4, 4}, 1, PrGpuFormat_Rgba8, PrTextureAlpha_None);
    const PrTextureSource* source = fake.source();
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.pack = PrTextureCachePack_Never;
    PrTextureCache* cache = prTextureCacheCreate(&desc);
    // First the color of a material, with every level. Then the encoded color of a drawing, with one level and
    // premultiplied alpha.
    PrTextureRegion color{};
    CHECK(realized(cache, source, askOf(PrTextureValues_Color), &color).format == PrGpuFormat_Rgba8Srgb);
    PrTextureRegion drawn{};
    REQUIRE(prTextureCacheRealize(cache, source, prTextureCacheAskEncoded(0), &drawn) == PrTextureCacheState_Ready);
    PrGpuTextureDesc plain = prGpuTextureDesc(device, drawn.texture.texture);
    CHECK(plain.format == PrGpuFormat_Rgba8);
    CHECK(plain.levels == 3);
    CHECK(drawn.texture.texture.id != color.texture.texture.id);
    CHECK(prTextureCacheInfo(cache).textures == 1);
    CHECK(fake.reads == 1);
    // Data is read as stored, and its levels average the stored values. It therefore has a texture of its own.
    PrTextureRegion data{};
    CHECK(realized(cache, source, askOf(PrTextureValues_Data), &data).format == PrGpuFormat_Rgba8);
    CHECK(prTextureCacheInfo(cache).textures == 2);
    prTextureCacheDestroy(cache);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(device);
}

TEST_CASE("data whose consumer reads one channel is realized as that channel alone, in R8") {
    // A device with swizzled formats, if the system has one. A swizzled format reads one channel as gray.
    PrGpuDevice* memory = prGpuMemoryCreate(nullptr);
    PrGpuDevice* device = blockDevice() != nullptr ? blockDevice() : memory;
    PrTextureCache* cache = cacheOn(device);
    FakeSource fake = pixels({4, 4});
    const PrTextureSource* source = fake.source();
    // Red alone decodes to R8, and the device generates its levels.
    PrGpuTextureDesc red = realized(cache, source, askOf(PrTextureValues_Data, PrTextureChannelFlag_R));
    CHECK(red.format == PrGpuFormat_R8);
    CHECK(red.levels == 3);
    // On a device with swizzled formats, green alone is R8 that a sampler reads as gray. Its levels are generated on
    // the CPU, because no device renders to a swizzled format.
    bool swizzles = (prGpuDeviceInfo(device).features & PrGpuFeatureFlag_TextureSwizzle) != 0;
    PrGpuTextureDesc green = realized(cache, source, askOf(PrTextureValues_Data, PrTextureChannelFlag_G));
    CHECK(green.format == (swizzles ? PrGpuFormat_R8Luminance : PrGpuFormat_Rgba8));
    CHECK(green.levels == 3);
    // Color of one channel keeps its four channels.
    PrGpuTextureDesc color = realized(cache, source, askOf(PrTextureValues_Color, PrTextureChannelFlag_R));
    CHECK(color.format == PrGpuFormat_Rgba8Srgb);
    prTextureCacheDestroy(cache);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(memory);
}

TEST_CASE("floats become 16-bit floats of the same values, for color as for data") {
    FakeSource fake = pixels({2, 2}, 1, PrGpuFormat_Rgba32F);
    const PrTextureSource* source = fake.source();
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    const PrTextureValues values[2] = {PrTextureValues_Data, PrTextureValues_Color};
    for (PrTextureValues value : values) {
        PrGpuTextureDesc desc = realized(cache, source, askOf(value));
        CHECK(desc.format == PrGpuFormat_Rgba16F);
        CHECK(desc.levels == 2);
    }
    prTextureCacheDestroy(cache);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(device);
}

TEST_CASE("data gets pixels of a Srgb form in the plain form, with their bits unchanged") {
    FakeSource fake = pixels({4, 4});
    fake.desc.format = PrGpuFormat_Rgba8Srgb;
    fake.formats.clear();
    const PrTextureSource* source = fake.source();
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrTextureCache* cache = cacheOn(device);
    PrTextureRegion data{};
    PrTextureRegion color{};
    CHECK(realized(cache, source, askOf(PrTextureValues_Data), &data).format == PrGpuFormat_Rgba8);
    CHECK(realized(cache, source, askOf(PrTextureValues_Color), &color).format == PrGpuFormat_Rgba8Srgb);
    PrGpuNativePixels first{};
    PrGpuNativePixels second{};
    REQUIRE(prGpuTextureNative(device, data.texture.texture, PrGpuNativeKind_Pixels, &first));
    REQUIRE(prGpuTextureNative(device, color.texture.texture, PrGpuNativeKind_Pixels, &second));
    CHECK(prMemEquals(first.pixels.data, second.pixels.data, 64));
    prTextureCacheDestroy(cache);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(device);
}

TEST_CASE("blocks that the device samples are kept, and blocks that the device cannot sample are decoded to pixels") {
    FakeSource blocks;
    blocks.desc = PrTextureDesc{
        .kind = PrGpuTextureKind_2d,
        .extent = {8, 8},
        .depth = 1,
        .layerCount = 1,
        .levelCount = 4,
        .format = PrGpuFormat_Bc7,
        .alpha = PrTextureAlpha_Straight,
    };
    blocks.formats.push(PrGpuFormat_Rgba8);
    const PrTextureSource* source = blocks.source();
    // On a device that does not sample the blocks, the blocks are decoded to pixels.
    PrGpuDevice* memory = prGpuMemoryCreate(nullptr);
    PrTextureCache* memoryCache = cacheOn(memory);
    CHECK(realized(memoryCache, source, askOf(PrTextureValues_Color)).format == PrGpuFormat_Rgba8Srgb);
    prTextureCacheDestroy(memoryCache);
    PrGpuDevice* device = blockDevice();
    PrTextureCache* cache = device != nullptr ? cacheOn(device) : nullptr;
    if (device != nullptr && prGpuDeviceSupportsFormat(device, PrGpuFormat_Bc7Srgb, PrGpuTextureUsageFlag_Sampled)) {
        PrGpuTextureDesc kept = realized(cache, source, askOf(PrTextureValues_Color));
        CHECK(kept.format == PrGpuFormat_Bc7Srgb);
        CHECK(kept.levels == 4);
    }
    prTextureCacheDestroy(cache);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(memory);
}

TEST_SUITE_END();
