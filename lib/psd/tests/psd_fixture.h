// Photoshop documents written by the tests: the header, the color mode data, image resources, the layer count of the
// layer information, and the merged image, stored as it is or with PackBits.

#pragma once

#include "pr/array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What a test document holds.
struct PsdLayout {
    uint16_t version = 1;           // 2 for PSB
    uint16_t channels = 3;
    uint32_t width = 2;
    uint32_t height = 2;
    uint16_t depth = 8;
    uint16_t mode = 3;
    int16_t layerCount = 0;         // written in the layer information if it is not zero
    uint16_t compression = 0;
};

// Appends `v` as a big-endian integer of 16 bits.
inline void psdPut16(Array<uint8_t>* out, uint32_t v) {
    out->push(uint8_t(v >> 8));
    out->push(uint8_t(v));
}

// Appends `v` as a big-endian integer of 32 bits.
inline void psdPut32(Array<uint8_t>* out, uint32_t v) {
    psdPut16(out, v >> 16);
    psdPut16(out, v & 0xffff);
}

// Returns an image resource of `id` that holds `data`, with an empty name.
inline Array<uint8_t> psdResource(uint16_t id, const Array<uint8_t>& data) {
    Array<uint8_t> out({'8', 'B', 'I', 'M'});
    psdPut16(&out, id);
    psdPut16(&out, 0);
    psdPut32(&out, data.count());
    out.append(data.span());
    if (data.count() % 2 != 0)
        out.push(0);
    return out;
}

// Returns the PackBits encoding of each row of `planes`, the channels' rows in turn, of `rowBytes` bytes each, as
// literal runs, with the table of their byte counts first, of 2 bytes each for PSD and 4 for PSB.
inline Array<uint8_t> psdPackBits(const Array<uint8_t>& planes, uint32_t rowBytes, bool psb) {
    Array<uint8_t> counts;
    Array<uint8_t> rows;
    for (uint32_t at = 0; at < planes.count(); at += rowBytes) {
        Array<uint8_t> row;
        for (uint32_t x = 0; x < rowBytes; x += 128) {
            const uint32_t n = rowBytes - x < 128 ? rowBytes - x : 128;
            row.push(uint8_t(n - 1));
            row.append(PrByteView{planes.data() + at + x, n});
        }
        if (psb)
            psdPut32(&counts, row.count());
        else
            psdPut16(&counts, row.count());
        rows.append(row.span());
    }
    counts.append(rows.span());
    return counts;
}

// Returns a document of `layout`, `colorData`, `resources` and the merged image's `data`.
inline Array<uint8_t> psdFile(const PsdLayout& layout, const Array<uint8_t>& colorData, const Array<uint8_t>& resources,
        const Array<uint8_t>& data) {
    Array<uint8_t> out({'8', 'B', 'P', 'S'});
    psdPut16(&out, layout.version);
    for (uint32_t i = 0; i < 6; ++i)
        out.push(0);
    psdPut16(&out, layout.channels);
    psdPut32(&out, layout.height);
    psdPut32(&out, layout.width);
    psdPut16(&out, layout.depth);
    psdPut16(&out, layout.mode);
    psdPut32(&out, colorData.count());
    out.append(colorData.span());
    psdPut32(&out, resources.count());
    out.append(resources.span());
    // The layer and mask information: the layer information's length and its layer count, and nothing else.
    const bool psb = layout.version == 2;
    if (layout.layerCount != 0) {
        if (psb)
            psdPut32(&out, 0);
        psdPut32(&out, psb ? 12 : 8);
        if (psb)
            psdPut32(&out, 0);
        psdPut32(&out, 2);
        psdPut16(&out, uint16_t(layout.layerCount));
        psdPut16(&out, 0);
    } else {
        if (psb)
            psdPut32(&out, 0);
        psdPut32(&out, 0);
    }
    psdPut16(&out, layout.compression);
    out.append(data.span());
    return out;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
