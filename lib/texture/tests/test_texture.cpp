#include "pr/array.h"
#include "pr/color_space.h"
#include "pr/float16.h"
#include "pr/gpu.h"
#include "pr/gpu_memory.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/texture.h"
#include "pr/texture_backend.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// The bits of an application's texture. It holds a description, and the texels of every level and layer in the order
// that a read of all of them returns. It counts its reads and records its release. A read completes during the call. If
// `deferred` is set, a read waits until the test completes the waiting reads.
struct Bits {
    PrTextureDesc desc{};
    Array<uint8_t> texels;
    uint32_t reads = 0;
    bool released = false;
    bool deferred = false;
    Array<PrTextureRead*> waiting;

    static void read(void* PR_NULLABLE context, PrTextureRead* read) {
        Bits* bits = static_cast<Bits*>(context);
        ++bits->reads;
        if (bits->deferred) {
            bits->waiting.push(read);
            return;
        }
        bits->complete(read);
    }

    void complete(PrTextureRead* read) {
        const PrTextureReadDesc& asked = *prTextureReadInfo(read);
        PrTextureReadDesc all = prTextureReadDescAll(&desc);
        for (uint32_t level = asked.firstLevel; level < asked.firstLevel + asked.levelCount; ++level) {
            for (uint32_t layer = asked.firstLayer; layer < asked.firstLayer + asked.layerCount; ++layer) {
                prMemCopy(prTextureReadStorage(read) + prTextureReadOffset(&desc, &asked, level, layer),
                    texels.begin() + prTextureReadOffset(&desc, &all, level, layer), prTextureLevelSize(&desc, level));
            }
        }
        prTextureReadComplete(read, PrStatus_Ok);
    }

    void completeWaiting() {
        Array<PrTextureRead*> pending = waiting;
        waiting.clear();
        for (PrTextureRead* read : pending)
            complete(read);
    }

    static void release(void* PR_NULLABLE context) { static_cast<Bits*>(context)->released = true; }

    // Returns a source of these bits with `format`, `extent`, one layer and `levels` levels.
    const PrTextureSource* source(PrGpuFormat format, PrUVec2 extent, uint32_t levels = 1,
            PrTextureAlpha alpha = PrTextureAlpha_Straight) {
        desc = PrTextureDesc{
            .kind = PrGpuTextureKind_2d,
            .extent = extent,
            .depth = 1,
            .layerCount = 1,
            .levelCount = levels,
            .format = format,
            .alpha = alpha,
        };
        PrTextureSourceDesc source{.texture = desc, .read = read, .release = release, .context = this};
        return prTextureSourceCreate(&source);
    }
};

// Reads every level and layer of `source` into `out`. The read must complete during the call.
PrStatus readAll(const PrTextureSource* source, Array<uint8_t>* out) {
    PrTextureReadDesc desc = prTextureReadDescAll(prTextureSourceDesc(source));
    PrTextureRead* read = prTextureReadCreate(source, &desc);
    prTextureReadWait(read);
    PrStatus status = prTextureReadStatus(read);
    out->clear();
    if (status == PrStatus_Ok)
        out->append(PrByteView{prTextureReadStorage(read), prTextureReadSize(prTextureSourceDesc(source), &desc)});
    prTextureReadDestroy(read);
    return status;
}

// Returns the 16-bit float at `index` of `texels`.
float halfAt(const uint8_t* texels, uint32_t index) {
    PrFloat16 h{};
    prMemCopy(&h, texels + index * 2, 2);
    return prFloat16ToFloat(h);
}

// Returns the texels of a texture of a memory device.
PrGpuNativePixels nativeOf(PrGpuDevice* device, PrGpuTexture texture) {
    PrGpuNativePixels native{};
    REQUIRE(prGpuTextureNative(device, texture, PrGpuNativeKind_Pixels, &native));
    return native;
}

// A format for the media type image/png. It fails every open with `status`, or opens a source of one texel if `status`
// is Ok. A set reaches the fake through a const record. The fake's record of its opens and the bits of its sources are
// therefore mutable.
struct FakeFormat {
    PrStatus status;
    mutable uint32_t opened = 0;
    mutable Bits bits;

    // The format record that a set owns. It points to the fake.
    struct Record : PrTextureFormat {
        const FakeFormat* fake;
    };

    static bool supportsType(const PrTextureFormat*, PrStringView type) { return type == "image/png"; }

    static PrStatus create(const PrTextureFormat* record, const PrBlobSource*, const PrBlob*, const PrTextureLimits*,
            const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
        const FakeFormat* format = static_cast<const Record*>(record)->fake;
        ++format->opened;
        *out = format->status == PrStatus_Ok ? format->bits.source(PrGpuFormat_Rgba8, {1, 1}) : nullptr;
        return format->status;
    }

    PrTextureFormat* entry() const {
        Record* record = static_cast<Record*>(prMemoryAllocZeroed(sizeof(Record)));
        record->destroyFn = [](PrTextureFormat* made) { prMemoryFree(made); };
        record->supportsTypeFn = supportsType;
        record->createFn = create;
        record->fake = this;
        return record;
    }
};

// The first bytes of a PNG file, which lib/mime classifies as image/png.
constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

} // namespace

} // namespace pr_impl

using namespace harness;
using namespace pr_impl;

TEST_SUITE_BEGIN("texture");

TEST_CASE("a source of an application's bits is read on demand, and releases its context with its last reference") {
    Bits bits;
    const uint8_t texels[8] = {255, 128, 0, 255, 10, 20, 30, 64};
    bits.texels.append(PrByteView{texels, sizeof(texels)});
    const PrTextureSource* source = bits.source(PrGpuFormat_Rgba8, PrUVec2{2, 1});
    CHECK(prTextureSourceDesc(source)->format == PrGpuFormat_Rgba8);
    CHECK(prTextureSourceProduces(source, PrGpuFormat_Rgba8));
    CHECK(!prTextureSourceProduces(source, PrGpuFormat_Rgba16F));
    CHECK(prTextureSourceFormats(source, nullptr, 0) == 0);
    const PrTextureSource* other = nullptr;
    CHECK(prTextureSourceReformat(source, PrGpuFormat_Bc7, &other) == PrStatus_NotSupported);
    CHECK(other == nullptr);
    REQUIRE(prTextureSourceReformat(source, PrGpuFormat_Rgba8, &other) == PrStatus_Ok);
    CHECK(other == source);
    prTextureSourceRelease(other);
    CHECK(bits.reads == 0);
    Array<uint8_t> read;
    REQUIRE(readAll(source, &read) == PrStatus_Ok);
    CHECK(bits.reads == 1);
    CHECK(prMemEquals(read.begin(), texels, 8));
    // A read retains its source. The source outlives the caller's reference until the read is destroyed.
    bits.deferred = true;
    PrTextureReadDesc desc = prTextureReadDescAll(prTextureSourceDesc(source));
    PrTextureRead* pending = prTextureReadCreate(source, &desc);
    prTextureSourceRelease(source);
    CHECK(!bits.released);
    CHECK(!prTextureReadIsDone(pending));
    bits.completeWaiting();
    CHECK(prTextureReadIsDone(pending));
    CHECK(prTextureReadStatus(pending) == PrStatus_Ok);
    prTextureReadDestroy(pending);
    CHECK(bits.released);
}

TEST_CASE("a conversion between the plain and Srgb forms of a format reads the source's bits into the read's storage") {
    Bits bits;
    const uint8_t texels[4] = {10, 20, 30, 255};
    bits.texels.append(PrByteView{texels, sizeof(texels)});
    const PrTextureSource* source = bits.source(PrGpuFormat_Rgba8, PrUVec2{1, 1}, 1, PrTextureAlpha_None);
    PrTextureConvertDesc desc = prTextureConvertDescDefault(PrGpuFormat_Rgba8Srgb);
    const PrTextureSource* srgb = prTextureSourceCreateConvert(source, &desc);
    CHECK(prTextureSourceDesc(srgb)->format == PrGpuFormat_Rgba8Srgb);
    // A target of straight alpha over an opaque source stays opaque.
    CHECK(prTextureSourceDesc(srgb)->alpha == PrTextureAlpha_None);
    uint8_t storage[4] = {};
    PrTextureReadDesc read = prTextureReadDescAll(prTextureSourceDesc(srgb));
    read.storage = storage;
    PrTextureRead* made = prTextureReadCreate(srgb, &read);
    REQUIRE(prTextureReadStatus(made) == PrStatus_Ok);
    CHECK(prTextureReadStorage(made) == storage);
    CHECK(prMemEquals(storage, texels, 4));
    prTextureReadDestroy(made);
    prTextureSourceRelease(srgb);
    // The conversion retained its source. The source is released after the last release of either.
    CHECK(!bits.released);
    prTextureSourceRelease(source);
    CHECK(bits.released);
}

TEST_CASE("a conversion takes the requested channels, straightens alpha, and decodes sRGB values to linear floats") {
    Bits bits;
    // Premultiplied texels: half of white, and a quarter of red at alpha one half.
    const uint8_t texels[8] = {128, 128, 128, 128, 64, 0, 0, 128};
    bits.texels.append(PrByteView{texels, sizeof(texels)});
    const PrTextureSource* source = bits.source(PrGpuFormat_Rgba8, PrUVec2{2, 1}, 1, PrTextureAlpha_Premultiplied);

    // One channel of four, alone in R8. The swizzled form of R8 holds the same channel, and a sampler reads it as gray.
    PrTextureConvertDesc desc = prTextureConvertDescDefault(PrGpuFormat_R8Luminance);
    desc.alpha = PrTextureAlpha_None;
    desc.channels[0] = 3;
    const PrTextureSource* alpha = prTextureSourceCreateConvert(source, &desc);
    Array<uint8_t> read;
    REQUIRE(readAll(alpha, &read) == PrStatus_Ok);
    REQUIRE(read.count() == 2);
    CHECK(read[0] == 128);
    CHECK(read[1] == 128);
    // A conversion of the gray form reads it as gray in red, green and blue.
    PrTextureConvertDesc back = prTextureConvertDescDefault(PrGpuFormat_Rgba8);
    back.alpha = PrTextureAlpha_None;
    const PrTextureSource* gray = prTextureSourceCreateConvert(alpha, &back);
    REQUIRE(readAll(gray, &read) == PrStatus_Ok);
    CHECK(read[1] == 128);
    CHECK(read[2] == 128);
    CHECK(read[3] == 255);
    prTextureSourceRelease(gray);
    prTextureSourceRelease(alpha);

    // Straight alpha, the form that a material samples.
    desc = prTextureConvertDescDefault(PrGpuFormat_Rgba8);
    const PrTextureSource* straight = prTextureSourceCreateConvert(source, &desc);
    CHECK(prTextureSourceDesc(straight)->alpha == PrTextureAlpha_Straight);
    REQUIRE(readAll(straight, &read) == PrStatus_Ok);
    CHECK(read[0] == 255);
    CHECK(read[4] == 128);
    CHECK(read[7] == 128);
    prTextureSourceRelease(straight);

    // Between formats of 8 bits, the Srgb form keeps the values unchanged. In 16-bit floats, the values become linear
    // light, and stay premultiplied.
    desc = prTextureConvertDescDefault(PrGpuFormat_Rgba8Srgb);
    desc.alpha = PrTextureAlpha_Premultiplied;
    const PrTextureSource* srgb = prTextureSourceCreateConvert(source, &desc);
    REQUIRE(readAll(srgb, &read) == PrStatus_Ok);
    CHECK(prMemEquals(read.begin(), texels, 8));
    desc = prTextureConvertDescDefault(PrGpuFormat_Rgba16F);
    desc.alpha = PrTextureAlpha_Premultiplied;
    const PrTextureSource* wide = prTextureSourceCreateConvert(srgb, &desc);
    REQUIRE(readAll(wide, &read) == PrStatus_Ok);
    REQUIRE(read.count() == 16);
    // White straightens to white. The linear light of white is white, and it is multiplied by its alpha.
    CHECK(halfAt(read.begin(), 0) == doctest::Approx(128.0 / 255).epsilon(5e-3));
    CHECK(halfAt(read.begin(), 1) == doctest::Approx(128.0 / 255).epsilon(5e-3));
    // The half red is straightened, decoded to linear light, and multiplied by its alpha.
    float red = prColorTransferDecode(PrColorTransfer_Srgb, 128.0f / 255) * (128.0f / 255);
    CHECK(halfAt(read.begin(), 4) == doctest::Approx(red).epsilon(5e-3));
    CHECK(halfAt(read.begin(), 5) == 0);
    CHECK(halfAt(read.begin(), 7) == doctest::Approx(128.0 / 255).epsilon(5e-3));
    prTextureSourceRelease(wide);
    // Values of the plain form become floats unchanged. Red straightens to one half.
    desc = prTextureConvertDescDefault(PrGpuFormat_Rgba16F);
    const PrTextureSource* plain = prTextureSourceCreateConvert(source, &desc);
    REQUIRE(readAll(plain, &read) == PrStatus_Ok);
    CHECK(halfAt(read.begin(), 4) == doctest::Approx(0.5).epsilon(1e-3));
    prTextureSourceRelease(plain);
    prTextureSourceRelease(srgb);
    prTextureSourceRelease(source);
}

TEST_CASE("a mip chain adds levels after the source's own, each the average of 2x2 texels, in linear light for sRGB") {
    Bits bits;
    // A 4 by 2 texture of one channel: a row of 0 and 255, and a row of 255 and 0.
    const uint8_t texels[8] = {0, 255, 0, 255, 255, 0, 255, 0};
    bits.texels.append(PrByteView{texels, sizeof(texels)});
    const PrTextureSource* source = bits.source(PrGpuFormat_R8, PrUVec2{4, 2}, 1, PrTextureAlpha_None);
    const PrTextureSource* mips = prTextureSourceCreateMips(source, 0);
    REQUIRE(prTextureSourceDesc(mips)->levelCount == 3);
    Array<uint8_t> read;
    REQUIRE(readAll(mips, &read) == PrStatus_Ok);
    REQUIRE(read.count() == 8 + 2 + 1);
    CHECK(prMemEquals(read.begin(), texels, 8));
    CHECK(read[8] == 128);
    CHECK(read[9] == 128);
    CHECK(read[10] == 128);
    // A read of a level past the first reads the first level of the source, once.
    uint32_t before = bits.reads;
    PrTextureReadDesc desc = prTextureReadDescAll(prTextureSourceDesc(mips));
    desc.firstLevel = 2;
    desc.levelCount = 1;
    PrTextureRead* last = prTextureReadCreate(mips, &desc);
    REQUIRE(prTextureReadStatus(last) == PrStatus_Ok);
    CHECK(prTextureReadStorage(last)[0] == 128);
    prTextureReadDestroy(last);
    CHECK(bits.reads == before + 1);
    prTextureSourceRelease(mips);
    prTextureSourceRelease(source);

    // The black and white of sRGB average to the sRGB value of half the light, which is not 128.
    Bits encoded;
    const uint8_t pair[8] = {0, 0, 0, 255, 255, 255, 255, 255};
    encoded.texels.append(PrByteView{pair, sizeof(pair)});
    source = encoded.source(PrGpuFormat_Rgba8Srgb, PrUVec2{2, 1});
    mips = prTextureSourceCreateMips(source, 0);
    REQUIRE(readAll(mips, &read) == PrStatus_Ok);
    REQUIRE(read.count() == 12);
    uint8_t half = uint8_t(prColorTransferEncode(PrColorTransfer_Srgb, 0.5f) * 255 + 0.5f);
    CHECK(read[8] == half);
    CHECK(read[11] == 255);
    prTextureSourceRelease(mips);
    prTextureSourceRelease(source);
}

TEST_CASE("a grid of tiles puts each tile in its cell, resampled to the largest tile's extent") {
    // Tile 1001 is 4 by 4 texels of red. Tile 1002, to its right, is 2 by 2 texels of green. Each tile is one flat
    // color, so its cell holds that color however the tile is resampled.
    Bits red;
    Bits green;
    for (uint32_t i = 0; i < 16; ++i) {
        const uint8_t texel[4] = {255, 0, 0, 255};
        red.texels.append(PrByteView{texel, 4});
    }
    for (uint32_t i = 0; i < 4; ++i) {
        const uint8_t texel[4] = {0, 255, 0, 255};
        green.texels.append(PrByteView{texel, 4});
    }
    const PrTextureSource* first = red.source(PrGpuFormat_Rgba8, {4, 4}, 1, PrTextureAlpha_None);
    const PrTextureSource* second = green.source(PrGpuFormat_Rgba8, {2, 2}, 1, PrTextureAlpha_None);
    const PrTextureTile tiles[2] = {{.source = first, .cell = {0, 0}}, {.source = second, .cell = {1, 0}}};
    // Returns the expected color of the texel at (x, y) of a texture whose cells have `cell` texels. The first column
    // of cells is red and the second is green. A cell that no tile holds is transparent black.
    auto expected = [](uint32_t x, uint32_t y, PrUVec2 cell, uint32_t channel) -> uint8_t {
        if (y >= cell.y)
            return 0;
        if (channel == 3)
            return 255;
        return channel == (x < cell.x ? 0u : 1u) ? 255 : 0;
    };
    struct Case {
        PrUVec2 grid;
        uint32_t maxExtent;
        PrUVec2 extent;
    };
    const Case cases[] = {
        {{2, 1}, 16384, {8, 4}},    // the cells have the extent of the red tile, and the green tile is magnified
        {{2, 1}, 4, {4, 2}},        // the cells are halved to fit, and the red tile is minified
        {{2, 2}, 16384, {8, 8}},    // the cells of the second row hold no tile
    };
    for (const Case& c : cases) {
        PrTextureTilesDesc desc{.tiles = tiles, .tileCount = 2, .grid = c.grid, .maxExtent = c.maxExtent};
        const PrTextureSource* grid = prTextureSourceCreateTiles(&desc);
        REQUIRE(grid != nullptr);
        const PrTextureDesc& made = *prTextureSourceDesc(grid);
        CHECK(made.format == PrGpuFormat_Rgba8);
        CHECK(made.alpha == PrTextureAlpha_None);
        REQUIRE(made.extent.x == c.extent.x);
        REQUIRE(made.extent.y == c.extent.y);
        Array<uint8_t> read;
        REQUIRE(readAll(grid, &read) == PrStatus_Ok);
        PrUVec2 cell{c.extent.x / c.grid.x, c.extent.y / c.grid.y};
        uint32_t wrong = 0;
        for (uint32_t y = 0; y < c.extent.y; ++y) {
            for (uint32_t x = 0; x < c.extent.x; ++x) {
                for (uint32_t channel = 0; channel < 4; ++channel)
                    wrong += read[(y * c.extent.x + x) * 4 + channel] != expected(x, y, cell, channel);
            }
        }
        CHECK(wrong == 0);
        prTextureSourceRelease(grid);
    }
    prTextureSourceRelease(first);
    prTextureSourceRelease(second);
    CHECK((red.released && green.released));

    // A magnified tile interpolates linearly between the two nearest texels. Texel x of 4 reads the tile at (x + 0.5) *
    // 2 / 4 - 0.5. At the ends, that position clamps to the first and last texels of the tile.
    Bits ramp;
    const uint8_t ends[2] = {0, 255};
    ramp.texels.append(PrByteView{ends, 2});
    const PrTextureSource* rampSource = ramp.source(PrGpuFormat_R8, {2, 1}, 1, PrTextureAlpha_None);
    const PrTextureSource* bigger = red.source(PrGpuFormat_Rgba8, {4, 4}, 1, PrTextureAlpha_None);
    const PrTextureTile pair[2] = {{.source = rampSource, .cell = {0, 0}}, {.source = bigger, .cell = {0, 1}}};
    PrTextureTilesDesc desc{.tiles = pair, .tileCount = 2, .grid = {1, 2}, .maxExtent = 16384};
    const PrTextureSource* grid = prTextureSourceCreateTiles(&desc);
    Array<uint8_t> read;
    REQUIRE(readAll(grid, &read) == PrStatus_Ok);
    const float at[4] = {0, 0.25f, 0.75f, 1};
    for (uint32_t x = 0; x < 4; ++x)
        CHECK(read[x * 4] == uint8_t(at[x] * 255 + 0.5f));
    prTextureSourceRelease(grid);
    prTextureSourceRelease(rampSource);
    prTextureSourceRelease(bigger);
}

TEST_CASE("an upload reads a chain's bits once, makes the levels on the device, and rereads the bits after a loss") {
    Bits bits;
    const uint8_t texels[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    bits.texels.append(PrByteView{texels, sizeof(texels)});
    const PrTextureSource* source = bits.source(PrGpuFormat_Rgba8, PrUVec2{2, 2}, 1, PrTextureAlpha_Premultiplied);
    PrTextureConvertDesc desc = prTextureConvertDescDefault(PrGpuFormat_Rgba8);
    desc.alpha = PrTextureAlpha_Premultiplied;
    const PrTextureSource* chain = prTextureSourceCreateConvert(source, &desc);
    struct Uploaded {
        uint32_t calls = 0;
        PrStatus status = PrStatus_InvalidData;
        PrTexture texture{};
    };
    auto done = [](void* PR_NULLABLE context, PrStatus status, PrTexture texture) {
        Uploaded* uploaded = static_cast<Uploaded*>(context);
        ++uploaded->calls;
        uploaded->status = status;
        uploaded->texture = texture;
    };
    for (uint32_t round = 0; round < 2; ++round) {
        PrGpuDevice* device = prGpuMemoryCreate(nullptr);
        // The read completes later, and the upload with it.
        bits.deferred = true;
        Uploaded uploaded;
        prTextureUpload(device, chain, PrTextureUploadFlag_MakesLevels, "test", done, &uploaded);
        CHECK(uploaded.calls == 0);
        bits.completeWaiting();
        REQUIRE(uploaded.calls == 1);
        REQUIRE(uploaded.status == PrStatus_Ok);
        CHECK(uploaded.texture.alpha == PrTextureAlpha_Premultiplied);
        CHECK(bits.reads == round + 1);
        PrGpuTextureDesc gpu = prGpuTextureDesc(device, uploaded.texture.texture);
        CHECK(gpu.levels == 2);
        CHECK(prTextureUploadSize(device, prTextureSourceDesc(chain), PrTextureUploadFlag_MakesLevels) == 16 + 4);
        PrGpuNativePixels native = nativeOf(device, uploaded.texture.texture);
        CHECK(prMemEquals(native.pixels.data, texels, 8));
        prGpuTextureRelease(device, uploaded.texture.texture);
        // The device is lost. An upload to it fails without a read. An upload to a new device reads the bits again.
        prGpuDeviceSimulateLoss(device);
        bits.deferred = false;
        Uploaded lost;
        prTextureUpload(device, chain, 0, "test", done, &lost);
        CHECK(lost.status == PrStatus_Lost);
        CHECK(lost.texture.texture.id == 0);
        CHECK(bits.reads == round + 1);
        prGpuDeviceDestroy(device);
    }
    prTextureSourceRelease(chain);
    prTextureSourceRelease(source);
}

TEST_CASE("a format set opens bytes with the first format of their media type that does not refuse them") {
    FakeFormat first{.status = PrStatus_NotSupported};
    FakeFormat second{.status = PrStatus_InvalidData};
    FakeFormat third{.status = PrStatus_Ok};
    PrTextureFormat* formats[3] = {first.entry(), second.entry(), third.entry()};
    PrTextureFormatSet* opener = prTextureFormatSetCreate(formats, 3);
    CHECK(prTextureFormatSetSupportsType(opener, "image/png"));
    CHECK(!prTextureFormatSetSupportsType(opener, "image/jpeg"));
    PrBlob* blob = prBlobCopy(PrByteView{kPngSignature, sizeof(kPngSignature)});
    PrBlobSource* bytes = prBlobSourceCreateBlob(blob);
    prBlobRelease(blob);
    const PrTextureSource* source = nullptr;
    CHECK(prTextureFormatSetRead(opener, bytes, "a.png", nullptr, &source) == PrStatus_InvalidData);
    CHECK(source == nullptr);
    CHECK(first.opened == 1);
    CHECK(second.opened == 1);
    CHECK(third.opened == 0);
    prBlobSourceRelease(bytes);
    // No format opens bytes of another media type.
    const uint8_t text[4] = {'t', 'e', 'x', 't'};
    blob = prBlobCopy(PrByteView{text, 4});
    bytes = prBlobSourceCreateBlob(blob);
    prBlobRelease(blob);
    CHECK(prTextureFormatSetRead(opener, bytes, "a.txt", nullptr, &source) == PrStatus_NotSupported);
    prBlobSourceRelease(bytes);
    prTextureFormatSetDestroy(opener);

    // If the blob source completes its read later, the bytes open at that time.
    PrTextureFormat* opens[1] = {third.entry()};
    opener = prTextureFormatSetCreate(opens, 1);
    struct Later {
        PrBlobRead* pending = nullptr;
        uint32_t calls = 0;
        PrStatus status = PrStatus_InvalidData;
    } later;
    PrBlobSourceDesc desc{
        .start = [](void* PR_NULLABLE context, PrBlobRead* read) { static_cast<Later*>(context)->pending = read; },
        .context = &later,
        .size = sizeof(kPngSignature),
    };
    bytes = prBlobSourceCreate(&desc);
    prTextureFormatSetOpen(opener, bytes, "", nullptr,
        [](void* PR_NULLABLE context, PrStatus status, const PrTextureSource* PR_NULLABLE opened) {
            Later* held = static_cast<Later*>(context);
            ++held->calls;
            held->status = status;
            prTextureSourceRelease(opened);
        }, &later);
    CHECK(later.calls == 0);
    REQUIRE(later.pending != nullptr);
    prBlobReadComplete(later.pending, PrStatus_Ok, prBlobCopy(PrByteView{kPngSignature, sizeof(kPngSignature)}), 0);
    CHECK(later.calls == 1);
    CHECK(later.status == PrStatus_Ok);
    CHECK(third.opened == 1);
    prBlobSourceRelease(bytes);
    prTextureFormatSetDestroy(opener);
}

TEST_CASE("stored pixels unpack to four channels, with gray in red, green and blue and opaque alpha") {
    const uint16_t grayAlpha[2] = {65535, 32768};
    uint8_t rgba[4] = {};
    prTexturePixelsUnpack(PrPixelView{reinterpret_cast<const uint8_t*>(grayAlpha), 4, {1, 1}},
        PrTextureChannels_GrayAlpha, PrTextureSample_U16, PrGpuFormat_Rgba8, PrPixels{rgba, 4, {1, 1}});
    CHECK(rgba[0] == 255);
    CHECK(rgba[2] == 255);
    CHECK(rgba[3] == 128);
    const uint8_t rg[2] = {10, 20};
    float values[4] = {};
    prTexturePixelsUnpack(PrPixelView{rg, 2, {1, 1}}, PrTextureChannels_Rg, PrTextureSample_U8, PrGpuFormat_Rgba32F,
        PrPixels{reinterpret_cast<uint8_t*>(values), 16, {1, 1}});
    CHECK(values[1] == doctest::Approx(20.0 / 255));
    CHECK(values[2] == 0);
    CHECK(values[3] == 1);
    uint8_t halves[8] = {};
    prTexturePixelsUnpack(PrPixelView{rg, 2, {1, 1}}, PrTextureChannels_Rg, PrTextureSample_U8, PrGpuFormat_Rgba16F,
        PrPixels{halves, 8, {1, 1}});
    CHECK(halfAt(halves, 0) == doctest::Approx(10.0 / 255).epsilon(1e-3));
    CHECK(halfAt(halves, 3) == 1);
}
