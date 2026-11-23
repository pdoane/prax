// DDS files written by the tests: the headers of a legacy pixel format or of the DX10 extension, and the texels.

#pragma once

#include "pr/array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What a test file's headers hold. A file has the DX10 header if `dxgi` is not zero.
struct DdsLayout {
    uint32_t width = 4;
    uint32_t height = 4;
    uint32_t depth = 0;
    uint32_t levels = 0;            // the mip count, written with its flag if not zero
    uint32_t pixelFlags = 0;
    uint32_t fourCc = 0;
    uint32_t bits = 0;
    uint32_t masks[4] = {0, 0, 0, 0};
    uint32_t caps2 = 0;
    uint32_t dxgi = 0;
    uint32_t dimension = 3;
    uint32_t misc = 0;
    uint32_t arraySize = 1;
    uint32_t alphaMode = 0;
};

// Returns the FourCC of four characters, as a header stores it.
inline constexpr uint32_t ddsFourCc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

// Returns a file of `layout` and then `texels`.
inline Array<uint8_t> ddsFile(const DdsLayout& layout, const Array<uint8_t>& texels) {
    Array<uint8_t> out;
    auto word = [&](uint32_t value) {
        for (uint32_t k = 0; k < 4; ++k)
            out.push(uint8_t(value >> (8 * k)));
    };
    word(ddsFourCc('D', 'D', 'S', ' '));
    word(124);
    word(0x1007 | (layout.levels != 0 ? 0x20000u : 0u) | (layout.depth != 0 ? 0x800000u : 0u));
    word(layout.height);
    word(layout.width);
    word(0);
    word(layout.depth);
    word(layout.levels);
    for (uint32_t i = 0; i < 11; ++i)
        word(0);
    word(32);
    if (layout.dxgi != 0) {
        word(0x4);
        word(ddsFourCc('D', 'X', '1', '0'));
    } else {
        word(layout.pixelFlags);
        word(layout.fourCc);
    }
    word(layout.bits);
    for (uint32_t mask : layout.masks)
        word(mask);
    word(0x1000);
    word(layout.caps2);
    word(0);
    word(0);
    word(0);
    if (layout.dxgi != 0) {
        word(layout.dxgi);
        word(layout.dimension);
        word(layout.misc);
        word(layout.arraySize);
        word(layout.alphaMode);
    }
    out.append(texels.span());
    return out;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
