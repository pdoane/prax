// DDS files as texture sources: the texels as stored, the pixels that they decode to, and the format of a format set.

#include "pr/array.h"
#include "pr/blob.h"
#include "pr/dds_texture_source.h"
#include "pr/gpu.h"
#include "pr/mem.h"
#include "pr/texture.h"

#include "dds/tests/dds_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A blob source over a file in memory.
struct FileBytes {
    Array<uint8_t> file;
    PrBlobSource* source = nullptr;

    explicit FileBytes(Array<uint8_t> bytes) : file(static_cast<Array<uint8_t>&&>(bytes)) {
        PrBlobSourceDesc desc{.start = start, .context = this, .size = kPrBlobSizeUnknown};
        source = prBlobSourceCreate(&desc);
    }
    FileBytes(const FileBytes&) = delete;
    FileBytes& operator=(const FileBytes&) = delete;
    ~FileBytes() { prBlobSourceRelease(source); }

    static void start(void* PR_NULLABLE context, PrBlobRead* read) {
        FileBytes* bytes = static_cast<FileBytes*>(context);
        prBlobReadComplete(read, PrStatus_Ok, prBlobCopy(PrByteView{bytes->file.begin(), bytes->file.count()}), 0);
    }
};

const PrTextureSource* sourceOf(const PrBlobSource* bytes) {
    const PrTextureSource* source = nullptr;
    REQUIRE(prDdsTextureSourceOpen(bytes, nullptr, &source) == PrStatus_Ok);
    return source;
}

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

// Two levels of BC1, white, for a 4 by 4 texture.
Array<uint8_t> whiteBc1Levels() {
    return Array<uint8_t>({
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    });
}

// The formats that a test's consumer samples, at any extent.
struct Sampled {
    Array<PrGpuFormat> formats;

    static bool samples(void* PR_NULLABLE context, PrGpuFormat format, PrUVec2) {
        for (PrGpuFormat sampled : static_cast<const Sampled*>(context)->formats) {
            if (sampled == format)
                return true;
        }
        return false;
    }
};

// Returns the format of the answer of `source` to `need`, or None if it does not answer.
PrGpuFormat answered(const PrTextureSource* source, const PrTextureNeed& need) {
    const PrTextureSource* answer = nullptr;
    if (prTextureSourceAnswer(source, &need, &answer) != PrStatus_Ok)
        return PrGpuFormat_None;
    PrGpuFormat format = prTextureSourceDesc(answer)->format;
    prTextureSourceRelease(answer);
    return format;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("dds");

TEST_CASE("a DDS file of blocks is a source of its blocks as stored, and decodes to pixels") {
    FileBytes bytes(ddsFile(DdsLayout{.levels = 2, .dxgi = 72}, whiteBc1Levels()));
    const PrTextureSource* source = sourceOf(bytes.source);
    const PrTextureDesc& desc = *prTextureSourceDesc(source);
    CHECK(desc.format == PrGpuFormat_Bc1Srgb);
    CHECK(desc.levelCount == 2);
    CHECK(desc.alpha == PrTextureAlpha_Straight);
    Array<uint8_t> read;
    REQUIRE(readAll(source, &read) == PrStatus_Ok);
    CHECK(read == whiteBc1Levels());
    const PrTextureSource* pixels = nullptr;
    REQUIRE(prTextureSourceReformat(source, PrGpuFormat_Rgba8Srgb, &pixels) == PrStatus_Ok);
    REQUIRE(readAll(pixels, &read) == PrStatus_Ok);
    REQUIRE(read.count() == 64 + 16);
    for (uint8_t value : read)
        CHECK(value == 255);
    prTextureSourceRelease(pixels);
    prTextureSourceRelease(source);
}

TEST_CASE("a DDS file of pixels of a GPU format keeps them, and a layout without one decodes") {
    // B5G6R5 is a format of lib/gpu, and decodes to Rgba8 too.
    const uint32_t v565 = 31u << 11;
    Array<uint8_t> red({uint8_t(v565), uint8_t(v565 >> 8)});
    FileBytes packed(ddsFile(DdsLayout{.width = 1, .height = 1, .dxgi = 85}, red));
    const PrTextureSource* source = sourceOf(packed.source);
    CHECK(prTextureSourceDesc(source)->format == PrGpuFormat_B5G6R5);
    Array<uint8_t> read;
    REQUIRE(readAll(source, &read) == PrStatus_Ok);
    CHECK(read == red);
    const PrTextureSource* pixels = nullptr;
    REQUIRE(prTextureSourceReformat(source, PrGpuFormat_Rgba8, &pixels) == PrStatus_Ok);
    REQUIRE(readAll(pixels, &read) == PrStatus_Ok);
    CHECK(read == Array<uint8_t>({255, 0, 0, 255}));
    prTextureSourceRelease(pixels);
    prTextureSourceRelease(source);

    // A luminance of 8 bits is R8 read as gray.
    FileBytes gray(ddsFile(DdsLayout{
        .width = 1,
        .height = 1,
        .pixelFlags = 0x20000,
        .bits = 8,
        .masks = {0xff, 0, 0, 0},
    }, Array<uint8_t>({90})));
    source = sourceOf(gray.source);
    CHECK(prTextureSourceDesc(source)->format == PrGpuFormat_R8Luminance);
    prTextureSourceRelease(source);

    // 24-bit RGB has no format of lib/gpu, so the source is its pixels.
    FileBytes rgb(ddsFile(DdsLayout{
        .width = 1,
        .height = 1,
        .pixelFlags = 0x40,
        .bits = 24,
        .masks = {0xff0000, 0xff00, 0xff, 0},
    }, Array<uint8_t>({1, 2, 3})));
    source = sourceOf(rgb.source);
    CHECK(prTextureSourceDesc(source)->format == PrGpuFormat_Rgba8);
    CHECK(prTextureSourceDesc(source)->alpha == PrTextureAlpha_None);
    REQUIRE(readAll(source, &read) == PrStatus_Ok);
    CHECK(read == Array<uint8_t>({3, 2, 1, 255}));
    prTextureSourceRelease(source);
}

TEST_CASE("a DDS cube and a volume are sources of their kinds") {
    Array<uint8_t> faces;
    for (uint32_t i = 0; i < 6; ++i)
        faces.append(PrByteView{whiteBc1Levels().data(), 8});
    FileBytes cube(ddsFile(DdsLayout{.dxgi = 71, .misc = 0x4}, faces));
    const PrTextureSource* source = sourceOf(cube.source);
    CHECK(prTextureSourceDesc(source)->kind == PrGpuTextureKind_Cube);
    CHECK(prTextureSourceDesc(source)->layerCount == 6);
    prTextureSourceRelease(source);

    Array<uint8_t> voxels({1, 2, 3, 4, 5, 6, 7, 8});
    FileBytes volume(ddsFile(DdsLayout{.width = 2, .height = 2, .depth = 2, .dxgi = 61, .dimension = 4}, voxels));
    source = sourceOf(volume.source);
    CHECK(prTextureSourceDesc(source)->kind == PrGpuTextureKind_3d);
    CHECK(prTextureSourceDesc(source)->depth == 2);
    Array<uint8_t> read;
    REQUIRE(readAll(source, &read) == PrStatus_Ok);
    CHECK(read == voxels);
    prTextureSourceRelease(source);
}

TEST_CASE("the DDS texture format reads image/vnd.ms-dds through a format set") {
    PrTextureFormat* format = prDdsTextureFormatCreate();
    PrTextureFormatSet* set = prTextureFormatSetCreate(&format, 1);
    CHECK(prTextureFormatSetSupportsType(set, "image/vnd.ms-dds"));
    FileBytes bytes(ddsFile(DdsLayout{.levels = 2, .dxgi = 71}, whiteBc1Levels()));
    const PrTextureSource* source = nullptr;
    REQUIRE(prTextureFormatSetRead(set, bytes.source, "white.dds", nullptr, &source) == PrStatus_Ok);
    CHECK(prTextureSourceDesc(source)->format == PrGpuFormat_Bc1);
    prTextureSourceRelease(source);
    prTextureFormatSetDestroy(set);
}

TEST_CASE("a DDS past the limits' extent on one side is refused, and one at the extent opens") {
    Array<uint8_t> texels(8 * 4 * 4);
    FileBytes bytes(ddsFile(DdsLayout{.width = 8, .height = 4, .dxgi = 28}, texels));
    PrTextureLimits limits{.extent = 7};
    const PrTextureSource* source = nullptr;
    CHECK(prDdsTextureSourceOpen(bytes.source, &limits, &source) == PrStatus_Limit);
    CHECK(source == nullptr);
    limits.extent = 8;
    REQUIRE(prDdsTextureSourceOpen(bytes.source, &limits, &source) == PrStatus_Ok);
    Array<uint8_t> read;
    CHECK(readAll(source, &read) == PrStatus_Ok);
    prTextureSourceRelease(source);
    // The slices of a volume are bounded as the sides are.
    Array<uint8_t> voxels(2 * 2 * 3);
    FileBytes volume(ddsFile(DdsLayout{.width = 2, .height = 2, .depth = 3, .dxgi = 61, .dimension = 4}, voxels));
    limits.extent = 2;
    CHECK(prDdsTextureSourceOpen(volume.source, &limits, &source) == PrStatus_Limit);
}

TEST_CASE("a DDS source answers with its blocks if the consumer samples them, and with pixels otherwise") {
    FileBytes bytes(ddsFile(DdsLayout{.levels = 2, .dxgi = 71}, whiteBc1Levels()));
    const PrTextureSource* source = sourceOf(bytes.source);
    Sampled sampled;
    PrTextureNeed need{
        .samples = Sampled::samples,
        .context = &sampled,
        .values = PrTextureValues_Data,
        .alpha = PrTextureAlpha_Straight,
    };
    CHECK(answered(source, need) == PrGpuFormat_Rgba8);
    sampled.formats.push(PrGpuFormat_Bc1);
    CHECK(answered(source, need) == PrGpuFormat_Bc1);
    // A need of pixels takes the decoded pixels, and one of the red channel of data takes R8.
    need.pixels = true;
    CHECK(answered(source, need) == PrGpuFormat_Rgba8);
    need.channels = PrTextureChannelFlag_R;
    CHECK(answered(source, need) == PrGpuFormat_R8);
    prTextureSourceRelease(source);

    // Stored pixels that lib/texture does not convert answer as the pixels that they decode to.
    const uint32_t v565 = 31u << 11;
    FileBytes packed(ddsFile(DdsLayout{.width = 1, .height = 1, .dxgi = 85},
        Array<uint8_t>({uint8_t(v565), uint8_t(v565 >> 8)})));
    source = sourceOf(packed.source);
    need.channels = 0;
    CHECK(answered(source, need) == PrGpuFormat_Rgba8);
    prTextureSourceRelease(source);
}

TEST_SUITE_END();
