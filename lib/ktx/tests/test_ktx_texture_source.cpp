#include "pr/array.h"
#include "pr/blob.h"
#include "pr/gpu.h"
#include "pr/gpu_memory.h"
#include "pr/ktx.h"
#include "pr/ktx_texture_source.h"
#include "pr/mem.h"
#include "pr/texture.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace harness;

namespace {

// Returns a KTX 2.0 file of format `vkFormat` and `extent` texels. Its `levelCount` levels hold the bytes at `data`,
// one level after another, and `sizes` gives the size of each level. The descriptor states the sRGB transfer function
// if `srgb` is set, and linear values otherwise. It states the BT.709 primaries.
Array<uint8_t> ktxFile(uint32_t vkFormat, PrUVec2 extent, uint32_t levelCount, const uint8_t* data, const uint32_t* sizes,
        bool srgb = false) {
    Array<uint8_t> file;
    auto word = [&](uint32_t value) {
        for (int k = 0; k < 4; ++k)
            file.push(uint8_t(value >> (8 * k)));
    };
    const uint8_t identifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    for (uint8_t byte : identifier)
        file.push(byte);
    uint32_t dfdOffset = 80 + 24 * levelCount;
    // The header fields: the format, its type size, the extent, no depth, no layers, one face, the level count, no
    // supercompression, the location of the descriptor, and no metadata.
    const uint32_t header[] = {vkFormat, 1, extent.x, extent.y, 0, 0, 1, levelCount, 0, dfdOffset, 44, 0, 0, 0, 0, 0, 0};
    for (uint32_t value : header)
        word(value);
    uint32_t at = dfdOffset + 44;
    for (uint32_t level = 0; level < levelCount; ++level) {
        const uint32_t entry[] = {at, 0, sizes[level], 0, sizes[level], 0};
        for (uint32_t value : entry)
            word(value);
        at += sizes[level];
    }
    // A descriptor of one sample, with the color model RGBSDA, the BT.709 primaries and the transfer function.
    uint32_t transfer = srgb ? 2 : 1;
    const uint32_t descriptor[] = {44, 0, 2 | 40u << 16, 1u | 1u << 8 | transfer << 16, 0, 0, 0, 0, 0, 0, 0};
    for (uint32_t value : descriptor)
        word(value);
    for (uint32_t i = 0; i < at - dfdOffset - 44; ++i)
        file.push(data[i]);
    return file;
}

// A blob source over a file in memory. It counts the number of times its bytes are read.
struct CountedBytes {
    Array<uint8_t> file;
    uint32_t reads = 0;
    PrBlobSource* source = nullptr;

    explicit CountedBytes(Array<uint8_t> bytes) : file(static_cast<Array<uint8_t>&&>(bytes)) {
        PrBlobSourceDesc desc{.start = start, .context = this, .size = kPrBlobSizeUnknown};
        source = prBlobSourceCreate(&desc);
    }
    CountedBytes(const CountedBytes&) = delete;
    CountedBytes& operator=(const CountedBytes&) = delete;
    ~CountedBytes() { prBlobSourceRelease(source); }

    static void start(void* PR_NULLABLE context, PrBlobRead* read) {
        CountedBytes* counted = static_cast<CountedBytes*>(context);
        ++counted->reads;
        prBlobReadComplete(read, PrStatus_Ok, prBlobCopy(PrByteView{counted->file.begin(), counted->file.count()}), 0);
    }
};

// Returns a source of the KTX 2.0 file that `bytes` provides.
const PrTextureSource* sourceOf(const PrBlobSource* bytes) {
    const PrTextureSource* source = nullptr;
    REQUIRE(prKtxTextureSourceOpen(bytes, nullptr, nullptr, &source) == PrStatus_Ok);
    return source;
}

// Reads every level and layer of `source` into `out`. Returns the status of the read.
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

// Two levels of BC7, one block each. The blocks are mode 6 with both ends white and opaque.
constexpr uint8_t kWhiteBc7[32] = {
    0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0, 0, 0, 0, 0, 0, 0,
    0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0, 0, 0, 0, 0, 0, 0,
};

} // namespace

} // namespace pr_impl

using namespace harness;
using namespace pr_impl;

TEST_SUITE_BEGIN("ktx");

TEST_CASE("a KTX file of pixels is a texture source of its levels, and the levels also convert to floats") {
    // Three levels of red, green, blue and alpha, with 16, 4 and 1 texels.
    uint8_t texels[84];
    for (uint32_t i = 0; i < sizeof(texels); ++i)
        texels[i] = uint8_t(i * 3);
    const uint32_t sizes[3] = {64, 16, 4};
    CountedBytes rgba(ktxFile(37, PrUVec2{4, 4}, 3, texels, sizes));
    const PrTextureSource* source = sourceOf(rgba.source);
    const PrTextureDesc& desc = *prTextureSourceDesc(source);
    CHECK(desc.extent == PrUVec2{4, 4});
    // The file states linear values, so the format is the plain form.
    CHECK(desc.format == PrGpuFormat_Rgba8);
    CHECK(desc.levelCount == 3);
    Array<uint8_t> read;
    REQUIRE(readAll(source, &read) == PrStatus_Ok);
    REQUIRE(read.count() == 84);
    CHECK(prMemEquals(read.begin(), texels, sizeof(texels)));
    CHECK(prTextureSourceProduces(source, PrGpuFormat_Rgba16F));
    const PrTextureSource* floats = nullptr;
    REQUIRE(prTextureSourceReformat(source, PrGpuFormat_Rgba32F, &floats) == PrStatus_Ok);
    CHECK(prTextureSourceDesc(floats)->format == PrGpuFormat_Rgba32F);
    REQUIRE(readAll(floats, &read) == PrStatus_Ok);
    float first = 0;
    prMemCopy(&first, read.begin() + 4, 4);
    CHECK(first == doctest::Approx(3.0 / 255));
    prTextureSourceRelease(floats);
    prTextureSourceRelease(source);
}

TEST_CASE("a KTX file of blocks is a texture source of the blocks, and decodes to pixels if lib/basis decodes the format") {
    const uint32_t sizes[2] = {16, 16};
    CountedBytes bc7(ktxFile(145, PrUVec2{4, 4}, 2, kWhiteBc7, sizes));
    const PrTextureSource* source = sourceOf(bc7.source);
    CHECK(prTextureSourceDesc(source)->format == PrGpuFormat_Bc7);
    CHECK(prTextureSourceDesc(source)->levelCount == 2);
    Array<uint8_t> read;
    REQUIRE(readAll(source, &read) == PrStatus_Ok);
    REQUIRE(read.count() == 32);
    CHECK(prMemEquals(read.begin(), kWhiteBc7, 32));
    const PrTextureSource* pixels = nullptr;
    REQUIRE(prTextureSourceReformat(source, PrGpuFormat_Rgba8, &pixels) == PrStatus_Ok);
    REQUIRE(readAll(pixels, &read) == PrStatus_Ok);
    REQUIRE(read.count() == 64 + 16);
    for (uint32_t i = 0; i < 64; ++i)
        CHECK(read[i] == 255);
    prTextureSourceRelease(pixels);
    prTextureSourceRelease(source);

    // A file that states the sRGB transfer function is a source of the Srgb forms of its blocks and of its 8-bit
    // pixels. The Srgb forms hold the same bits as the plain forms.
    CountedBytes srgb(ktxFile(146, PrUVec2{4, 4}, 2, kWhiteBc7, sizes, true));
    source = sourceOf(srgb.source);
    CHECK(prTextureSourceDesc(source)->format == PrGpuFormat_Bc7Srgb);
    CHECK(!prTextureSourceProduces(source, PrGpuFormat_Rgba8));
    CHECK(prTextureSourceProduces(source, PrGpuFormat_Rgba16F));
    REQUIRE(prTextureSourceReformat(source, PrGpuFormat_Rgba8Srgb, &pixels) == PrStatus_Ok);
    REQUIRE(readAll(pixels, &read) == PrStatus_Ok);
    for (uint32_t i = 0; i < 64; ++i)
        CHECK(read[i] == 255);
    prTextureSourceRelease(pixels);
    prTextureSourceRelease(source);

    // ASTC blocks do not decode to pixels.
    const uint32_t block = 16;
    CountedBytes astc(ktxFile(157, PrUVec2{4, 4}, 1, kWhiteBc7, &block));
    source = sourceOf(astc.source);
    CHECK(!prTextureSourceProduces(source, PrGpuFormat_Rgba8));
    CHECK(prTextureSourceReformat(source, PrGpuFormat_Rgba8, &pixels) == PrStatus_NotSupported);
    prTextureSourceRelease(source);
}

TEST_CASE("the bytes of a KTX file before its levels open a source with the file's description, and reads of that source fail") {
    const uint32_t sizes[2] = {16, 16};
    Array<uint8_t> file = ktxFile(145, PrUVec2{4, 4}, 2, kWhiteBc7, sizes);
    uint64_t needed = prKtxInfoBytes(PrByteView{file.begin(), 80});
    REQUIRE(needed < file.count());
    Array<uint8_t> header;
    header.append(PrByteView{file.begin(), size_t(needed)});
    CountedBytes start(static_cast<Array<uint8_t>&&>(header));
    const PrTextureSource* described = sourceOf(start.source);
    CHECK(prTextureSourceDesc(described)->format == PrGpuFormat_Bc7);
    CHECK(prTextureSourceDesc(described)->levelCount == 2);
    Array<uint8_t> read;
    CHECK(readAll(described, &read) == PrStatus_InvalidData);
    prTextureSourceRelease(described);
}

TEST_CASE("a KTX file of single-channel blocks decodes to pixels with the value in red, green and blue") {
    // Two levels of BC4 and of EAC R11, one block each. Every block holds one value. The BC4 ends are 128. The EAC base
    // is 128 with a multiplier of zero.
    const uint8_t bc4[16] = {128, 128, 0, 0, 0, 0, 0, 0, 128, 128, 0, 0, 0, 0, 0, 0};
    const uint8_t eac[16] = {128, 0, 0, 0, 0, 0, 0, 0, 128, 0, 0, 0, 0, 0, 0, 0};
    const uint32_t sizes[2] = {8, 8};
    struct Case {
        uint32_t vkFormat;
        const uint8_t* blocks;
        PrGpuFormat stored;
    };
    const Case kCases[] = {{139, bc4, PrGpuFormat_Bc4}, {153, eac, PrGpuFormat_EacR11}};
    for (const Case& c : kCases) {
        CAPTURE(c.vkFormat);
        CountedBytes bytes(ktxFile(c.vkFormat, PrUVec2{4, 4}, 2, c.blocks, sizes));
        const PrTextureSource* source = sourceOf(bytes.source);
        CHECK(prTextureSourceDesc(source)->format == c.stored);
        const PrTextureSource* pixels = nullptr;
        REQUIRE(prTextureSourceReformat(source, PrGpuFormat_Rgba8, &pixels) == PrStatus_Ok);
        Array<uint8_t> read;
        REQUIRE(readAll(pixels, &read) == PrStatus_Ok);
        CHECK(read[0] == read[1]);
        CHECK(read[0] == read[2]);
        CHECK(read[3] == 255);
        prTextureSourceRelease(pixels);
        prTextureSourceRelease(source);
    }
}

TEST_CASE("a KTX source reads the bytes from its provider again for each read, and keeps none of them") {
    uint8_t texels[16];
    for (uint32_t i = 0; i < sizeof(texels); ++i)
        texels[i] = uint8_t(i * 7);
    const uint32_t size = 16;
    CountedBytes bytes(ktxFile(37, PrUVec2{2, 2}, 1, texels, &size));
    const PrTextureSource* source = sourceOf(bytes.source);
    CHECK(bytes.reads == 1);
    for (uint32_t round = 0; round < 2; ++round) {
        PrGpuDevice* device = prGpuMemoryCreate(nullptr);
        struct Uploaded {
            PrStatus status = PrStatus_InvalidData;
            PrTexture texture{};
        } uploaded;
        prTextureUpload(device, source, 0, "ktx", [](void* PR_NULLABLE context, PrStatus status, PrTexture texture) {
            static_cast<Uploaded*>(context)->status = status;
            static_cast<Uploaded*>(context)->texture = texture;
        }, &uploaded);
        REQUIRE(uploaded.status == PrStatus_Ok);
        CHECK(bytes.reads == round + 2);
        PrGpuNativePixels native{};
        REQUIRE(prGpuTextureNative(device, uploaded.texture.texture, PrGpuNativeKind_Pixels, &native));
        for (uint32_t y = 0; y < 2; ++y)
            CHECK(prMemEquals(native.pixels.data + y * native.pixels.stride, texels + y * 8, 8));
        prGpuTextureRelease(device, uploaded.texture.texture);
        prGpuDeviceDestroy(device);
    }
    CHECK(bytes.reads == 3);
    prTextureSourceRelease(source);
}

TEST_SUITE_END();
