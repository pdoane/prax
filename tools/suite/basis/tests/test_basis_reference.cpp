// Tests lib/basis's decoders of Basis Universal's formats, read through lib/ktx, against the reference transcoder. The
// inputs are the KTX 2.0 textures of Khronos's sample assets, if the checkout has them, and textures that the reference
// encoder writes. The encoded textures cover ETC1S at several qualities and UASTC at several levels, with and without
// rate-distortion optimization and Zstandard, with and without alpha, as a normal map, with mip levels, as a cube and
// as an array, and with each channel layout that a descriptor may name. Both implementations decode a texture to pixels
// of 8 bits a channel, and the pixels must be equal. Both transcode UASTC to blocks of BC7 and of ASTC 4x4, and the
// blocks must be equal too. Both transcode ETC1S to blocks of BC1, BC3, BC4 and BC5, by different means. Those blocks
// are decoded, and the test compares the error that each implementation's blocks leave against the pixels.

#include "pr/basis.h"
#include "pr/compress_select.h"
#include "pr/ktx.h"
#include "pr/mem.h"

#include "tools/harness/cpu_counters.h"

#include "basisu_comp.h"
#include "basisu_gpu_texture.h"
#include "basisu_transcoder.h"
#include "tools/harness/test.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

double seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Returns the number of instructions that the process has retired. The count changes little with the machine's other
// work.
double instructions() {
    using namespace harness;
    return cpuCounters().instructions;
}

// What comparing one texture found.
struct Comparison {
    uint64_t texels = 0;
    uint64_t different = 0;
    double ours = 0;        // seconds
    double reference = 0;
    double oursInstructions = 0;
    double referenceInstructions = 0;
    bool decoded = false;
};

// The channel ids of the first two samples of a texture's descriptor, and the number of its samples.
struct Channels {
    uint32_t samples;
    uint32_t first;
    uint32_t second;
    bool etc1s;
};

Channels channelsOf(const Bytes& file) {
    const uint8_t* dfd = file.data() + prMemReadLeU32(file.data() + 48);
    uint32_t samples = (uint32_t(prMemReadLeU16(dfd + 10)) - 24) / 16;
    return Channels{samples, dfd[31] & 15u, samples > 1 ? dfd[47] & 15u : 0u, dfd[12] == 163};
}

// Opens the texture in `file` with lib/ktx and prepares it, or returns null if it does not open.
PrKtxTexture* PR_NULLABLE openTexture(const Bytes& file) {
    PrKtxTexture* texture = nullptr;
    if (prKtxTextureCreate(PrByteView(file), prCompressCodecSetCreateDefault(), &texture) != PrStatus_Ok ||
            prKtxTexturePrepare(texture) != PrStatus_Ok) {
        prKtxTextureDestroy(texture);
        return nullptr;
    }
    return texture;
}

// Decodes every level of every layer and face of `file` with lib/ktx and with the reference, and compares the channels
// that lib/ktx decodes with the reference's red, green, blue and alpha.
Comparison compare(const Bytes& file) {
    Comparison result;
    PrKtxTexture* texture = openTexture(file);
    basist::ktx2_transcoder reference;
    bool opened = texture != nullptr;
    REQUIRE(reference.init(file.data(), uint32_t(file.size())));
    REQUIRE(reference.start_transcoding());
    if (opened) {
        PrKtxInfo info = *prKtxTextureInfo(texture);
        Channels channels = channelsOf(file);
        uint32_t pixelBytes = info.channels;
        // The channel in which the reference has the second of two values. It is alpha, unless the texture keeps the
        // value in green.
        bool pair = pixelBytes == 3 && (channels.etc1s ? channels.samples == 2 : channels.first >= 5);
        uint32_t second = !channels.etc1s && channels.first == 6 ? 1 : 3;
        uint32_t layers = info.layerCount;
        uint32_t frameCount = info.layerCount * info.faceCount;
        CHECK(info.levelCount == reference.get_levels());
        CHECK(info.faceCount == reference.get_faces());
        CHECK(layers == (reference.get_layers() != 0 ? reference.get_layers() : 1));
        result.decoded = true;
        for (uint32_t level = 0; level < info.levelCount; ++level) {
            PrUVec2 extent = prKtxLevelExtent(info.extent, level);
            size_t texels = size_t(extent.x) * extent.y;
            Bytes ours(texels * pixelBytes);
            Bytes theirs(texels * 4);
            for (uint32_t frame = 0; frame < frameCount; ++frame) {
                double start = seconds();
                double startInstructions = instructions();
                PrStatus status =
                    prKtxTextureRead(texture, frame, level, PrPixels{ours.data(), size_t(extent.x) * pixelBytes, extent});
                double middle = seconds();
                double middleInstructions = instructions();
                bool transcoded = reference.transcode_image_level(level, frame / info.faceCount, frame % info.faceCount,
                    theirs.data(), uint32_t(texels), basist::transcoder_texture_format::cTFRGBA32);
                result.ours += middle - start;
                result.reference += seconds() - middle;
                result.oursInstructions += middleInstructions - startInstructions;
                result.referenceInstructions += instructions() - middleInstructions;
                REQUIRE(transcoded);
                if (status != PrStatus_Ok) {
                    result.decoded = false;
                    continue;
                }
                result.texels += texels;
                for (size_t i = 0; i < texels; ++i) {
                    const uint8_t* a = ours.data() + i * pixelBytes;
                    const uint8_t* b = theirs.data() + i * 4;
                    bool same = pair ? a[0] == b[0] && a[1] == b[second] && a[2] == 0 : prMemEquals(a, b, pixelBytes);
                    result.different += same ? 0 : 1;
                }
            }
        }
    }
    prKtxTextureDestroy(texture);
    return result;
}

// The result of comparing the blocks of one target that both implementations transcode a texture to.
struct BlockComparison {
    uint64_t blocks = 0;
    uint64_t different = 0;
    double ours = 0;        // seconds
    double reference = 0;
};

// Transcodes every level of every layer and face of `file` to `target` with lib/ktx, if lib/ktx offers the target, and
// to `format` with the reference. Compares the blocks, starting at byte `from` of each block.
BlockComparison compareBlocks(const Bytes& file, PrBasisBlocks target, basist::transcoder_texture_format format,
        size_t from = 0) {
    BlockComparison result;
    PrKtxTexture* texture = openTexture(file);
    basist::ktx2_transcoder reference;
    REQUIRE(reference.init(file.data(), uint32_t(file.size())));
    REQUIRE(reference.start_transcoding());
    if (texture != nullptr) {
        PrKtxInfo info = *prKtxTextureInfo(texture);
        uint32_t frameCount = info.layerCount * info.faceCount;
        for (uint32_t level = 0; (info.transcodes >> target & 1) != 0 && level < info.levelCount; ++level) {
            PrUVec2 extent = prKtxLevelExtent(info.extent, level);
            size_t size = prBasisBlocksSize(extent, target);
            size_t blockBytes = prBasisBlocksBytes(target);
            Bytes ours(size);
            Bytes theirs(size);
            for (uint32_t frame = 0; frame < frameCount; ++frame) {
                double start = seconds();
                REQUIRE(prKtxTextureTranscode(texture, frame, level, target, ours.data(), size) == PrStatus_Ok);
                double middle = seconds();
                REQUIRE(reference.transcode_image_level(level, frame / info.faceCount, frame % info.faceCount,
                    theirs.data(), uint32_t(size / blockBytes), format));
                result.ours += middle - start;
                result.reference += seconds() - middle;
                for (size_t at = 0; at < size; at += blockBytes) {
                    result.blocks += 1;
                    bool same = prMemEquals(ours.data() + at + from, theirs.data() + at + from, blockBytes - from);
                    result.different += same ? 0 : 1;
                }
            }
        }
    }
    prKtxTextureDestroy(texture);
    return result;
}

// Decodes the 8 bytes of BC1 at `block` to the red, green and blue of its 16 texels, 4 bytes a texel.
void decodeBc1(const uint8_t* block, uint8_t* texels) {
    uint32_t ends[2] = {prMemReadLeU16(block), prMemReadLeU16(block + 2)};
    uint32_t colors[4][3];
    for (uint32_t end = 0; end < 2; ++end) {
        uint32_t r = ends[end] >> 11;
        uint32_t g = ends[end] >> 5 & 63;
        uint32_t b = ends[end] & 31;
        colors[end][0] = r << 3 | r >> 2;
        colors[end][1] = g << 2 | g >> 4;
        colors[end][2] = b << 3 | b >> 2;
    }
    for (uint32_t c = 0; c < 3; ++c) {
        bool four = ends[0] > ends[1];
        colors[2][c] = four ? (2 * colors[0][c] + colors[1][c] + 1) / 3 : (colors[0][c] + colors[1][c]) / 2;
        colors[3][c] = four ? (colors[0][c] + 2 * colors[1][c] + 1) / 3 : 0;
    }
    for (uint32_t texel = 0; texel < 16; ++texel) {
        uint32_t index = block[4 + texel / 4] >> (2 * (texel % 4)) & 3;
        for (uint32_t c = 0; c < 3; ++c)
            texels[texel * 4 + c] = uint8_t(colors[index][c]);
    }
}

// Decodes the 8 bytes of BC4 at `block` to channel `channel` of its 16 texels, 4 bytes a texel.
void decodeBc4(const uint8_t* block, uint32_t channel, uint8_t* texels) {
    uint32_t values[8] = {block[0], block[1]};
    for (uint32_t k = 2; k < 8; ++k) {
        if (values[0] > values[1])
            values[k] = ((8 - k) * values[0] + (k - 1) * values[1] + 3) / 7;
        else
            values[k] = k < 6 ? ((6 - k) * values[0] + (k - 1) * values[1] + 2) / 5 : (k == 6 ? 0 : 255);
    }
    uint64_t indices = 0;
    for (uint32_t k = 0; k < 6; ++k)
        indices |= uint64_t(block[2 + k]) << (8 * k);
    for (uint32_t texel = 0; texel < 16; ++texel)
        texels[texel * 4 + channel] = uint8_t(values[indices >> (3 * texel) & 7]);
}

// The squared error that the blocks of each transcoder leave for one target, against the pixels that the texture
// decodes to, and the number of values that the error is summed over.
struct BlockQuality {
    double ours = 0;
    double reference = 0;
    uint64_t values = 0;

    double psnr(double error) const { return error == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 * double(values) / error); }
};

// Transcodes every level of the ETC1S texture in `file` to `target` with lib/ktx and to `format` with the reference,
// decodes both sets of blocks, and adds the error of each against the decoded pixels to `*quality`. Only the channels
// that the target holds are compared: red, green and blue for BC1, red for BC4, all four for BC3, and red and alpha for
// BC5, whose second value the reference decodes into alpha.
void addQuality(const Bytes& file, PrBasisBlocks target, basist::transcoder_texture_format format, BlockQuality* quality) {
    PrKtxTexture* texture = openTexture(file);
    basist::ktx2_transcoder reference;
    REQUIRE(reference.init(file.data(), uint32_t(file.size())));
    REQUIRE(reference.start_transcoding());
    if (texture != nullptr) {
        PrKtxInfo info = *prKtxTextureInfo(texture);
        bool two = target == PrBasisBlocks_Bc3 || target == PrBasisBlocks_Bc5;
        size_t blockBytes = two ? 16 : 8;
        for (uint32_t level = 0; (info.transcodes >> target & 1) != 0 && level < info.levelCount; ++level) {
            PrUVec2 extent = prKtxLevelExtent(info.extent, level);
            uint32_t blocksX = (extent.x + 3) / 4;
            size_t size = prBasisBlocksSize(extent, target);
            Bytes blocks[2] = {Bytes(size), Bytes(size)};
            Bytes pixels(size_t(extent.x) * extent.y * 4);
            REQUIRE(prKtxTextureTranscode(texture, 0, level, target, blocks[0].data(), size) == PrStatus_Ok);
            REQUIRE(reference.transcode_image_level(level, 0, 0, blocks[1].data(), uint32_t(size / blockBytes), format));
            REQUIRE(reference.transcode_image_level(level, 0, 0, pixels.data(), uint32_t(pixels.size() / 4),
                basist::transcoder_texture_format::cTFRGBA32));
            for (size_t block = 0; block < size / blockBytes; ++block) {
                uint32_t blockX = uint32_t(block % blocksX);
                uint32_t blockY = uint32_t(block / blocksX);
                for (uint32_t which = 0; which < 2; ++which) {
                    const uint8_t* at = blocks[which].data() + block * blockBytes;
                    // The texels of the block. The channels that the target does not hold keep the values of the
                    // pixels.
                    uint8_t texels[64];
                    bool held[4] = {false, false, false, false};
                    if (target == PrBasisBlocks_Bc1 || target == PrBasisBlocks_Bc3) {
                        decodeBc1(at + (two ? 8 : 0), texels);
                        held[0] = held[1] = held[2] = true;
                    }
                    if (target != PrBasisBlocks_Bc1) {
                        decodeBc4(at, target == PrBasisBlocks_Bc3 ? 3 : 0, texels);
                        held[target == PrBasisBlocks_Bc3 ? 3 : 0] = true;
                    }
                    if (target == PrBasisBlocks_Bc5) {
                        decodeBc4(at + 8, 3, texels);
                        held[3] = true;
                    }
                    double error = 0;
                    uint64_t values = 0;
                    for (uint32_t y = 0; y < 4 && blockY * 4 + y < extent.y; ++y) {
                        for (uint32_t x = 0; x < 4 && blockX * 4 + x < extent.x; ++x) {
                            const uint8_t* pixel = pixels.data() + (size_t(blockY * 4 + y) * extent.x + blockX * 4 + x) * 4;
                            for (uint32_t c = 0; c < 4; ++c) {
                                double difference = held[c] ? double(texels[(y * 4 + x) * 4 + c]) - pixel[c] : 0;
                                error += difference * difference;
                                values += held[c] ? 1 : 0;
                            }
                        }
                    }
                    (which == 0 ? quality->ours : quality->reference) += error;
                    quality->values += which == 0 ? values : 0;
                }
            }
        }
    }
    prKtxTextureDestroy(texture);
}

// The targets that ETC1S transcodes to, and the reference's name for each.
struct Target {
    PrBasisBlocks blocks;
    basist::transcoder_texture_format format;
    const char* name;
};
const Target kEtc1sTargets[] = {
    {PrBasisBlocks_Bc1, basist::transcoder_texture_format::cTFBC1_RGB, "BC1"},
    {PrBasisBlocks_Bc3, basist::transcoder_texture_format::cTFBC3_RGBA, "BC3"},
    {PrBasisBlocks_Bc4, basist::transcoder_texture_format::cTFBC4_R, "BC4"},
    {PrBasisBlocks_Bc5, basist::transcoder_texture_format::cTFBC5_RG, "BC5"},
};

// The targets for which ETC1S transcodes to exactly the reference's blocks. For BC3, only the color of each block
// equals the reference's.
const Target kExactTargets[] = {
    {PrBasisBlocks_Etc2Rgb8, basist::transcoder_texture_format::cTFETC1_RGB, "ETC2 RGB8"},
    {PrBasisBlocks_Etc2Rgba8, basist::transcoder_texture_format::cTFETC2_RGBA, "ETC2 RGBA8"},
    {PrBasisBlocks_EacR11, basist::transcoder_texture_format::cTFETC2_EAC_R11, "EAC R11"},
    {PrBasisBlocks_EacRg11, basist::transcoder_texture_format::cTFETC2_EAC_RG11, "EAC RG11"},
    {PrBasisBlocks_Bc1, basist::transcoder_texture_format::cTFBC1_RGB, "BC1"},
    {PrBasisBlocks_Bc3, basist::transcoder_texture_format::cTFBC3_RGBA, "BC3's color"},
};

// Adds the quality of each target that the ETC1S texture in `file` transcodes to. Each target has its own element of
// `quality`.
void addQualities(const Bytes& file, BlockQuality* quality) {
    for (size_t i = 0; i < 4; ++i)
        addQuality(file, kEtc1sTargets[i].blocks, kEtc1sTargets[i].format, &quality[i]);
}

// Checks that the blocks that lib/ktx transcodes ETC1S to are within half a decibel of the reference's, and reports
// both.
void checkQualities(const BlockQuality* quality) {
    for (size_t i = 0; i < 4; ++i) {
        const BlockQuality& q = quality[i];
        if (q.values == 0)
            continue;
        MESSAGE("ETC1S to " << std::string(kEtc1sTargets[i].name) << ", values: " <<
            q.values << ", PSNR against the pixels in decibels: " <<
            q.psnr(q.ours) << ", the reference's: " << q.psnr(q.reference));
        CHECK(q.psnr(q.ours) >= q.psnr(q.reference) - 0.5);
    }
}

// Returns an image that gives an encoder varied content: gradients, a disc, stripes and noise. Its alpha is a gradient
// with a hole.
basisu::image picture(uint32_t width, uint32_t height, uint32_t seed, bool alpha) {
    basisu::image image(width, height);
    uint32_t state = seed * 2654435761u + 1;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            int dx = int(x) - int(width) / 2;
            int dy = int(y) - int(height) / 2;
            bool disc = dx * dx + dy * dy < int(width * height) / 16;
            uint32_t r = x * 255 / width + (state & 15);
            uint32_t g = disc ? 220 : y * 255 / height;
            uint32_t b = (x / 4 + y / 8) % 2 != 0 ? 40 + seed * 20 % 200 : (state >> 8) & 255;
            uint32_t a = alpha ? (disc ? 0 : 255 - x * 200 / width) : 255;
            image(x, y) = basisu::color_rgba(int(r > 255 ? 255 : r), int(g), int(b), int(a));
        }
    }
    return image;
}

// The options for the reference encoder.
struct Options {
    bool uastc = false;
    int quality = 128;          // the ETC1S quality
    uint32_t level = 1;         // UASTC's pack level
    bool rdo = false;
    bool zstd = false;
    bool alpha = false;
    bool rg = false;            // two values: red in the color, which is gray, and green in the alpha
    bool normalMap = false;
    bool mips = false;
    uint32_t images = 1;
    bool cube = false;
    uint32_t width = 64;
    uint32_t height = 64;
};

Bytes encode(const Options& options) {
    static basisu::job_pool pool(1);
    basisu::basis_compressor_params params;
    for (uint32_t i = 0; i < options.images; ++i)
        params.m_source_images.push_back(picture(options.width, options.height, i + 1, options.alpha));
    params.m_pJob_pool = &pool;
    params.m_multithreading = false;
    params.m_status_output = false;
    params.m_create_ktx2_file = true;
    params.m_uastc = options.uastc;
    params.m_quality_level = options.quality;
    params.m_compression_level = 1;
    params.m_pack_uastc_flags = options.level;
    params.m_rdo_uastc = options.rdo;
    params.m_mip_gen = options.mips;
    params.m_ktx2_uastc_supercompression = options.zstd ? basist::KTX2_SS_ZSTANDARD : basist::KTX2_SS_NONE;
    if (options.rg) {
        const uint8_t kRrrg[4] = {0, 0, 0, 1};
        prMemCopy(params.m_swizzle, kRrrg, sizeof(kRrrg));
        params.m_force_alpha = true;
    }
    if (options.normalMap) {
        params.m_perceptual = false;
        params.m_mip_srgb = false;
        params.m_no_selector_rdo = true;
        params.m_no_endpoint_rdo = true;
    }
    if (options.images > 1)
        params.m_tex_type = options.cube ? basist::cBASISTexTypeCubemapArray : basist::cBASISTexType2DArray;
    basisu::basis_compressor compressor;
    REQUIRE(compressor.init(params));
    REQUIRE(compressor.process() == basisu::basis_compressor::cECSuccess);
    const basisu::uint8_vec& file = compressor.get_output_ktx2_file();
    return Bytes(file.begin(), file.end());
}

// The numbers of blocks of BC7 and of ASTC 4x4 that checkFile has compared.
uint64_t gBc7Blocks;
uint64_t gAstcBlocks;

// `gEtcBlocks` holds the results of comparing the blocks that checkFile transcoded ETC1S to: ETC2 RGB8 and RGBA8, EAC
// R11 and RG11, BC1, and the color of BC3. `gQuality` holds the quality of the blocks that checkFile transcoded ETC1S
// to.
BlockComparison gEtcBlocks[6];
BlockQuality gQuality[4];

// Checks that lib/ktx decodes `file` to what the reference does.
void checkFile(const Bytes& file, uint64_t* texels) {
    Comparison comparison = compare(file);
    CHECK(comparison.decoded);
    CHECK(comparison.different == 0);
    *texels += comparison.texels;
    // The BC7 blocks that UASTC transcodes to equal the reference's.
    BlockComparison bc7 = compareBlocks(file, PrBasisBlocks_Bc7, basist::transcoder_texture_format::cTFBC7_RGBA);
    CHECK(bc7.different == 0);
    gBc7Blocks += bc7.blocks;
    BlockComparison astc =
        compareBlocks(file, PrBasisBlocks_Astc4x4, basist::transcoder_texture_format::cTFASTC_4x4_RGBA);
    CHECK(astc.different == 0);
    gAstcBlocks += astc.blocks;
    // The ETC2, EAC and BC1 blocks that ETC1S transcodes to equal the reference's, for each target that the texture
    // offers. So do the BC1 blocks that are the color of BC3, which follow the alpha in each BC3 block.
    for (size_t i = 0; i < 6; ++i) {
        BlockComparison etc = compareBlocks(file, kExactTargets[i].blocks, kExactTargets[i].format, i == 5 ? 8 : 0);
        CAPTURE(std::string(kExactTargets[i].name));
        CHECK(etc.different == 0);
        gEtcBlocks[i].blocks += etc.blocks;
        gEtcBlocks[i].different += etc.different;
        gEtcBlocks[i].ours += etc.ours;
        gEtcBlocks[i].reference += etc.reference;
    }
    if (channelsOf(file).etc1s)
        addQualities(file, gQuality);
}

// Checks `file` again with the channel ids of its descriptor replaced by `first` and `second`, as a writer that fills
// them differently would set them.
void checkChannels(Bytes file, uint32_t first, uint32_t second, uint64_t* texels) {
    uint8_t* dfd = file.data() + prMemReadLeU32(file.data() + 48);
    dfd[31] = uint8_t((dfd[31] & 0xF0) | first);
    if (channelsOf(file).samples > 1)
        dfd[47] = uint8_t((dfd[47] & 0xF0) | second);
    checkFile(file, texels);
}

} // namespace

TEST_CASE("the basis decoders match the reference transcoder on textures that the reference encoder writes") {
    basist::basisu_transcoder_init();
    basisu::basisu_encoder_init();
    uint64_t texels = 0;
    uint32_t files = 0;
    // ETC1S: qualities, alpha, mips, a normal map, and sizes that are not whole blocks.
    for (int quality : std::vector<int>{1, 32, 128, 255}) {
        for (int variant = 0; variant < 4; ++variant) {
            CAPTURE(quality);
            CAPTURE(variant);
            Options options;
            options.quality = quality;
            options.alpha = (variant & 1) != 0;
            options.mips = (variant & 2) != 0;
            options.normalMap = quality == 32;
            options.width = variant == 3 ? 70 : 64;
            options.height = variant == 3 ? 46 : 64;
            Bytes file = encode(options);
            checkFile(file, &texels);
            // The same file with the channel ids of a texture of one value (3), and of two values (3 and 4). Those are
            // the ids that ETC1S gives its gray slices.
            if (!options.alpha)
                checkChannels(file, 3, 0, &texels);
            options.rg = true;
            checkChannels(encode(options), 3, 4, &texels);
            files += options.alpha ? 2 : 3;
        }
    }
    // UASTC: levels, rate-distortion optimization, Zstandard, alpha, mips, and each channel id.
    for (uint32_t level = 0; level <= 3; ++level) {
        for (int variant = 0; variant < 8; ++variant) {
            CAPTURE(level);
            CAPTURE(variant);
            Options options;
            options.uastc = true;
            options.level = level;
            options.rdo = (variant & 1) != 0;
            options.zstd = (variant & 2) != 0;
            options.alpha = (variant & 4) != 0;
            options.mips = variant == 3 || variant == 6;
            options.normalMap = variant == 5;
            options.width = variant == 7 ? 70 : 64;
            options.height = variant == 7 ? 46 : 64;
            Bytes file = encode(options);
            checkFile(file, &texels);
            for (uint32_t id : std::vector<uint32_t>{0, 3, 4, 5, 6})
                checkChannels(file, id, 0, &texels);
            files += 6;
        }
    }
    // Cubes and arrays, of both formats.
    for (int variant = 0; variant < 8; ++variant) {
        CAPTURE(variant);
        Options options;
        options.uastc = (variant & 1) != 0;
        options.cube = (variant & 2) != 0;
        options.images = options.cube ? 6 : 3;
        options.mips = (variant & 4) != 0;
        options.alpha = variant == 5;
        options.zstd = options.uastc && options.mips;
        checkFile(encode(options), &texels);
        ++files;
    }
    MESSAGE("textures: " << files << ", texels: " << texels << ", blocks of BC7: " << gBc7Blocks <<
        ", blocks of ASTC 4x4: " << gAstcBlocks);
    CHECK(gAstcBlocks != 0);
    for (size_t i = 0; i < 6; ++i) {
        const BlockComparison& etc = gEtcBlocks[i];
        MESSAGE("ETC1S to " << std::string(kExactTargets[i].name) << ", blocks: " << etc.blocks << ", different: " <<
            etc.different << ", megatexels a second: " << double(etc.blocks) * 16 / 1e6 / etc.ours <<
            ", the reference's: " << double(etc.blocks) * 16 / 1e6 / etc.reference);
        CHECK(etc.blocks != 0);
    }
    checkQualities(gQuality);
}

TEST_CASE("the basis decoders match the reference transcoder on the textures of the sample assets") {
    basist::basisu_transcoder_init();
    std::filesystem::path models = std::filesystem::path(PR_ROOT_DIR) / "third_party" / "glTF-Sample-Assets" / "Models";
    std::error_code error;
    std::vector<std::filesystem::path> paths;
    for (std::filesystem::recursive_directory_iterator at(models, error), end; !error && at != end; at.increment(error)) {
        if (at->path().extension() == ".ktx2")
            paths.push_back(at->path());
    }
    if (paths.empty()) {
        MESSAGE("the sample assets are missing; run ./dev.sh fetch gltf_sample_assets");
        return;
    }
    std::sort(paths.begin(), paths.end());
    Comparison total;
    Comparison etc1s;
    Comparison uastc;
    uint32_t exact = 0;
    BlockComparison bc7;
    BlockComparison astc;
    BlockComparison exactBlocks[6];
    BlockQuality quality[4];
    for (const std::filesystem::path& path : paths) {
        std::ifstream stream(path, std::ios::binary);
        Bytes file((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        Comparison comparison = compare(file);
        CAPTURE(path.string());
        CHECK(comparison.decoded);
        CHECK(comparison.different == 0);
        exact += comparison.decoded && comparison.different == 0 ? 1 : 0;
        BlockComparison blocks = compareBlocks(file, PrBasisBlocks_Bc7, basist::transcoder_texture_format::cTFBC7_RGBA);
        CHECK(blocks.different == 0);
        bc7.blocks += blocks.blocks;
        bc7.different += blocks.different;
        bc7.ours += blocks.ours;
        bc7.reference += blocks.reference;
        blocks = compareBlocks(file, PrBasisBlocks_Astc4x4, basist::transcoder_texture_format::cTFASTC_4x4_RGBA);
        CHECK(blocks.different == 0);
        astc.blocks += blocks.blocks;
        astc.different += blocks.different;
        astc.ours += blocks.ours;
        astc.reference += blocks.reference;
        for (size_t i = 0; i < 6; ++i) {
            blocks = compareBlocks(file, kExactTargets[i].blocks, kExactTargets[i].format, i == 5 ? 8 : 0);
            CAPTURE(std::string(kExactTargets[i].name));
            CHECK(blocks.different == 0);
            exactBlocks[i].blocks += blocks.blocks;
            exactBlocks[i].ours += blocks.ours;
            exactBlocks[i].reference += blocks.reference;
        }
        if (channelsOf(file).etc1s)
            addQualities(file, quality);
        Comparison& kind = channelsOf(file).etc1s ? etc1s : uastc;
        for (Comparison* sum : {&total, &kind}) {
            sum->texels += comparison.texels;
            sum->different += comparison.different;
            sum->ours += comparison.ours;
            sum->reference += comparison.reference;
            sum->oursInstructions += comparison.oursInstructions;
            sum->referenceInstructions += comparison.referenceInstructions;
        }
    }
    MESSAGE("textures: " << paths.size() << ", equal to the reference: " << exact << ", texels: " << total.texels <<
        ", different: " << total.different);
    MESSAGE("UASTC to BC7, blocks: " << bc7.blocks << ", different: " << bc7.different << ", megatexels a second: " <<
        double(bc7.blocks) * 16 / 1e6 / bc7.ours << ", the reference's: " <<
        double(bc7.blocks) * 16 / 1e6 / bc7.reference);
    MESSAGE("UASTC to ASTC 4x4, blocks: " << astc.blocks << ", different: " << astc.different <<
        ", megatexels a second: " << double(astc.blocks) * 16 / 1e6 / astc.ours << ", the reference's: " <<
        double(astc.blocks) * 16 / 1e6 / astc.reference);
    for (size_t i = 0; i < 6; ++i) {
        const BlockComparison& same = exactBlocks[i];
        if (same.blocks != 0) {
            MESSAGE("ETC1S to " << std::string(kExactTargets[i].name) << ", blocks: " << same.blocks <<
                ", megatexels a second: " << double(same.blocks) * 16 / 1e6 / same.ours << ", the reference's: " <<
                double(same.blocks) * 16 / 1e6 / same.reference);
        }
    }
    checkQualities(quality);
    MESSAGE("ETC1S megatexels a second: " << double(etc1s.texels) / 1e6 / etc1s.ours << ", the reference's: " <<
        double(etc1s.texels) / 1e6 / etc1s.reference);
    MESSAGE("UASTC megatexels a second: " << double(uastc.texels) / 1e6 / uastc.ours << ", the reference's: " <<
        double(uastc.texels) / 1e6 / uastc.reference);
    MESSAGE("instructions a texel, ETC1S: " << etc1s.oursInstructions / double(etc1s.texels) << ", the reference's: " <<
        etc1s.referenceInstructions / double(etc1s.texels) << "; UASTC: " <<
        uastc.oursInstructions / double(uastc.texels) << ", the reference's: " <<
        uastc.referenceInstructions / double(uastc.texels));
}

TEST_CASE("the block decoders decode blocks of random bits to the same texels as the reference unpackers") {
    // Each block format, the reference's name for it, and the number of channels that it has. The two decoders are
    // compared in those channels.
    struct Format {
        uint32_t vkFormat;
        PrBasisBlocks blocks;
        uint32_t blockBytes;
        basisu::texture_format reference;
        uint32_t channels;
        const char* name;
    };
    const Format kFormats[] = {
        {133, PrBasisBlocks_Bc1, 8, basisu::texture_format::cBC1, 4, "BC1"},
        {137, PrBasisBlocks_Bc3, 16, basisu::texture_format::cBC3, 4, "BC3"},
        {139, PrBasisBlocks_Bc4, 8, basisu::texture_format::cBC4, 1, "BC4"},
        {141, PrBasisBlocks_Bc5, 16, basisu::texture_format::cBC5, 2, "BC5"},
        {145, PrBasisBlocks_Bc7, 16, basisu::texture_format::cBC7, 4, "BC7"},
        {147, PrBasisBlocks_Etc2Rgb8, 8, basisu::texture_format::cETC2_RGB, 3, "ETC2 RGB8"},
        {151, PrBasisBlocks_Etc2Rgba8, 16, basisu::texture_format::cETC2_RGBA, 4, "ETC2 RGBA8"},
        {153, PrBasisBlocks_EacR11, 8, basisu::texture_format::cETC2_R11_EAC, 1, "EAC R11"},
        {155, PrBasisBlocks_EacRg11, 16, basisu::texture_format::cETC2_RG11_EAC, 2, "EAC RG11"},
    };
    const uint32_t kBlocks = 200000;
    uint64_t state = 0x9E3779B97F4A7C15ull;
    for (const Format& format : kFormats) {
        Bytes blocks(size_t(kBlocks) * format.blockBytes);
        for (uint8_t& byte : blocks) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            byte = uint8_t(state >> 56);
        }
        // The reference reads the colors of BC3 as it reads the colors of BC1. A BC1 block has four colors only if its
        // first endpoint is the greater. A BC3 block always has four.
        for (uint32_t i = 0; format.vkFormat == 137 && i < kBlocks; ++i) {
            uint8_t* block = &blocks[size_t(i) * 16];
            if ((block[8] | block[9] << 8) <= (block[10] | block[11] << 8)) {
                std::swap(block[8], block[10]);
                std::swap(block[9], block[11]);
            }
        }
        REQUIRE(prBasisBlocksDecodes(format.blocks));
        uint32_t compared = 0;
        uint32_t different = 0;
        for (uint32_t i = 0; i < kBlocks; ++i) {
            basisu::color_rgba reference[16];
            for (basisu::color_rgba& texel : reference)
                texel = basisu::color_rgba(0, 0, 0, 255);
            // The reference has no decoder of the modes that ETC2 adds to ETC1, nor of a BC7 block of no mode.
            if (!basisu::unpack_block(format.reference, &blocks[size_t(i) * format.blockBytes], reference))
                continue;
            ++compared;
            uint8_t texels[64];
            prBasisBlockDecode(format.blocks, &blocks[size_t(i) * format.blockBytes], texels);
            bool same = true;
            for (uint32_t t = 0; t < 16; ++t) {
                const uint8_t* pixel = &texels[t * 4];
                for (uint32_t c = 0; c < format.channels; ++c)
                    same = same && pixel[c] == reference[t][c];
            }
            different += same ? 0 : 1;
        }
        printf("%-10s  %6u blocks compared, %u differ\n", format.name, compared, different);
        CHECK(compared > 1000);
        CHECK(different == 0);
    }
}
