// What compares lib/exr's decoder with OpenEXR's core library: each decoder's reading of a file, as the samples of
// every channel of every level of every part, and OpenEXR's encoders, which write the files of each compression.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"
#include "pr/string.h"

PR_ASSUME_NONNULL_BEGIN

namespace exr {

using namespace pr;

// The samples of one channel of a level, as the file stores them: `width` by `height` samples of `sampleBytes` each,
// row after row, from the level's first pixel. A sample covers `xSampling` by `ySampling` pixels. The channel's name,
// its pixel type as the channel list gives it, and its pLinear are those of the file.
struct Plane {
    String name;
    uint32_t pixel = 1;
    bool linear = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t sampleBytes = 0;
    uint32_t xSampling = 1;
    uint32_t ySampling = 1;
    Array<uint8_t> samples;
};

// A level of a part: where its first pixel is, which is the data window's first pixel, its extent, and its planes by
// channel, in the order of the channel list.
struct Level {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    Array<Plane> planes;
};

// A part of a file: its display window's first pixel and extent, its compression, and its levels, in the order that
// lib/exr numbers them: a mip chain's from the largest, and a rip map's by rows of widths for each height.
struct Part {
    int32_t displayX = 0;
    int32_t displayY = 0;
    uint32_t displayWidth = 0;
    uint32_t displayHeight = 0;
    uint32_t compression = 0;
    bool decoded = true;        // false for a part that the decoder refuses
    bool deep = false;          // the part holds deep data
    Array<Level> levels;
};

// A file's parts.
struct Image {
    Array<Part> parts;
};

// OpenEXR's decode of the file through its core library: every chunk of every level of every part that is not deep
// data. Returns false if it refuses the file. A part whose chunks it refuses is not decoded.
bool referenceImage(PrByteView file, Image* out);

// lib/exr's decode of the file, each channel read in the form that it is stored in. Its first level holds only the
// samples of the data window that lie in the display window, and the rest are zero. Returns the status of the open or
// of the first read that fails, other than Truncated.
PrStatus praxImage(PrByteView file, Image* out);

// The comparison of two decodes of a file: the samples that both hold, those that differ, and those that lib/exr's
// first level does not hold, outside the display window.
struct Comparison {
    uint64_t compared = 0;
    uint64_t differ = 0;
    uint64_t outside = 0;
    bool shapes = true;         // false if the decodes have different parts, levels or planes
};

// Compares lib/exr's decode of a file with OpenEXR's.
Comparison compare(const Image& prax, const Image& reference);

// Decodes every level of every part of the file with each decoder as a program would: lib/exr's into the image's own
// format, and OpenEXR's into planes of each channel. What a timing measures.
PrStatus praxDecode(PrByteView file, Array<uint8_t>* scratch);
bool referenceDecode(PrByteView file, Array<uint8_t>* scratch);

// The layouts that OpenEXR's encoder writes: scan lines, or tiles of 64 by 64 pixels, of one level or with a mip chain
// or a rip map rounded down.
enum {
    Layout_ScanLines,
    Layout_Tiles,
    Layout_MipTiles,
    Layout_RipTiles,
};
typedef uint8_t Layout;

// Encodes the first level of the first part of `image` with its planes' channels, as a file of one part of `layout` in
// `compression`, a value of the file format's compression attribute, whose windows start at the origin. Each further
// level of a tiled layout is the first level sampled at every second pixel across and down. Returns an empty array if
// OpenEXR refuses to write it.
Array<uint8_t> encode(const Image& image, uint32_t compression, Layout layout);

} // namespace exr

PR_ASSUME_NONNULL_END
