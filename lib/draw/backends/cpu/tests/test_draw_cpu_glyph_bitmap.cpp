#include "pr/compress_builtin.h"
#include "pr/draw_cpu.h"
#include "pr/draw_glyph.h"
#include "pr/image_builtin.h"
#include "pr/vec.h"

#include "tools/harness/test.h"

#include <cstdio>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

std::vector<uint8_t> readFile(const char* name) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + name;
    std::vector<uint8_t> bytes;
    FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    uint8_t buffer[65536];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        bytes.insert(bytes.end(), buffer, buffer + read);
    std::fclose(file);
    return bytes;
}

void put16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(uint8_t(value >> 8));
    out.push_back(uint8_t(value));
}

void put32(std::vector<uint8_t>& out, uint32_t value) {
    put16(out, value >> 16);
    put16(out, value & 0xffff);
}

// Returns a big-endian TIFF of one strip of opaque RGB pixels, `size` on a side, all of one color.
std::vector<uint8_t> solidTiff(uint32_t size, uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> out = {'M', 'M', 0, 42};
    uint32_t pixels = size * size * 3;
    put32(out, 8 + pixels);
    for (uint32_t i = 0; i < size * size; ++i) {
        out.push_back(r);
        out.push_back(g);
        out.push_back(b);
    }
    const uint32_t entries[][3] = {
        {256, 4, size}, {257, 4, size}, {259, 3, 1}, {262, 3, 2},
        {273, 4, 8}, {277, 3, 3}, {278, 4, size}, {279, 4, pixels},
    };
    put16(out, 8);
    for (const uint32_t* entry : entries) {
        put16(out, entry[0]);
        put16(out, entry[1]);
        put32(out, 1);
        if (entry[1] == 3) {
            put16(out, entry[2]);
            put16(out, 0);
        } else {
            put32(out, entry[2]);
        }
    }
    put32(out, 0);
    return out;
}

// Returns the font file with its sbix table replaced by one strike of `ppem` pixels per em. In that strike, glyph
// `glyph` is `image`, of the sbix graphic type `type`, with its bottom left corner at the origin.
std::vector<uint8_t> withSbixImage(std::vector<uint8_t> font, uint16_t glyph, uint16_t glyphCount, uint32_t type,
        const std::vector<uint8_t>& image, uint16_t ppem) {
    std::vector<uint8_t> sbix;
    put16(sbix, 1);
    put16(sbix, 1);
    put32(sbix, 1);
    put32(sbix, 12);
    uint32_t data = 4 + (uint32_t(glyphCount) + 1) * 4;
    put16(sbix, ppem);
    put16(sbix, 72);
    for (uint32_t g = 0; g <= glyphCount; ++g)
        put32(sbix, g <= glyph ? data : data + 8 + uint32_t(image.size()));
    put16(sbix, 0);
    put16(sbix, 0);
    put32(sbix, type);
    sbix.insert(sbix.end(), image.begin(), image.end());
    while (font.size() % 4 != 0)
        font.push_back(0);
    uint32_t offset = uint32_t(font.size());
    font.insert(font.end(), sbix.begin(), sbix.end());
    uint32_t tables = uint32_t(font[4]) << 8 | font[5];
    for (uint32_t i = 0; i < tables; ++i) {
        uint8_t* record = font.data() + 12 + i * 16;
        if (std::string(reinterpret_cast<const char*>(record), 4) != "sbix")
            continue;
        for (uint32_t k = 0; k < 4; ++k) {
            record[8 + k] = uint8_t(offset >> (24 - 8 * k));
            record[12 + k] = uint8_t(sbix.size() >> (24 - 8 * k));
        }
    }
    return font;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a TIFF image in an sbix strike draws through the image codec") {
    std::vector<uint8_t> file = readFile("TwemojiSmileySbix.ttf");
    const PrFont* original = nullptr;
    REQUIRE(prFontCreate(file, 0, nullptr, &original) == PrStatus_Ok);
    uint16_t glyphs = uint16_t(prFontGlyphCount(original));
    prFontRelease(original);
    std::vector<uint8_t> bytes = withSbixImage(file, 2, glyphs, 0x74696666, solidTiff(32, 0, 0, 255), 32);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);

    PrCompressCodec* compressCodec = prCompressBuiltinCreate();
    PrImageCodec* imageCodec = prImageBuiltinCreate(compressCodec);
    PrDrawGlyphDecoder* decoder = prDrawGlyphBitmapCreate(imageCodec);
    PrDrawCpuOptions options = prDrawCpuOptionsDefault();
    options.glyphDecoder = decoder;
    PrDrawRenderer* renderer = prDrawCpuCreate(&options);
    // The image's 32 pixels at 32 pixels per em fill an em above the baseline: 40 to 72 across, 8 to 40 down.
    PrFontGlyph glyph{2, PrVec2{40, 40}};
    PrFontGlyphRun run{};
    run.font = font;
    run.size = 32;
    run.glyphs = &glyph;
    run.glyphCount = 1;
    PrDrawList* list = prDrawListCreate();
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 1}});
    std::vector<uint8_t> pixels(80 * 80 * 4, 0);
    REQUIRE(prDrawRendererRenderPixels(renderer, list, PrPixels{pixels.data(), 80 * 4, {80, 80}},
        PrPixelFormat_Rgba8) == PrStatus_Ok);
    const uint8_t* inside = pixels.data() + (size_t(24) * 80 + 56) * 4;
    CHECK(inside[2] == 255);
    CHECK(inside[3] == 255);
    const uint8_t* outside = pixels.data() + (size_t(60) * 80 + 20) * 4;
    CHECK(outside[3] == 0);
    prDrawListRelease(list);
    prDrawRendererDestroy(renderer);
    prDrawGlyphDecoderDestroy(decoder);
    prImageCodecDestroy(imageCodec);
    prCompressCodecDestroy(compressCodec);
    prFontRelease(font);
}
