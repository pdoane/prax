// The compressions of OpenEXR's pixel data, as lib/exr decodes them: each turns the bytes of one chunk back into the
// chunk's pixels.
//
// OpenEXR File Layout - https://openexr.com/en/latest/OpenEXRFileLayout.html
//
// A chunk's pixels, uncompressed, are its lines from the top down. Each line holds the samples of each channel that
// samples it, in the order of the channel list, and a channel's samples run from left to right, little-endian. The file
// layout leaves the compressed forms to the reference implementation, so each decompressor states the form that it
// reads.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/raw_array.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The compressions that a part's `compression` attribute names, by its value.
enum {
    ExrScheme_None,
    ExrScheme_Rle,
    ExrScheme_Zips,
    ExrScheme_Zip,
    ExrScheme_Piz,
    ExrScheme_Pxr24,
    ExrScheme_B44,
    ExrScheme_B44a,
    ExrScheme_Dwaa,
    ExrScheme_Dwab,
    ExrScheme_Count,
};
typedef uint8_t ExrScheme;

// How a channel stores a sample, by the value of its type in the channel list.
enum {
    ExrPixel_Uint,          // a 32-bit unsigned integer
    ExrPixel_Half,          // an IEEE binary16 float
    ExrPixel_Float,         // an IEEE binary32 float
    ExrPixel_Count,
};
typedef uint8_t ExrPixel;

// Returns the bytes of one sample of `pixel`.
inline uint32_t exrPixelBytes(ExrPixel pixel) {
    return pixel == ExrPixel_Half ? 2 : 4;
}

// Returns the lines of a chunk that the compression stores together: a scan line block's height.
uint32_t exrSchemeLines(ExrScheme compression);

// One channel of a chunk: how it stores its samples, and how many it has across the chunk and down it.
struct ExrBlockChannel {
    PrStringView name;
    ExrPixel pixel;
    bool linear;            // pLinear: the values are perceptually linear, which B44 takes into account
    uint32_t ySampling;
    uint32_t width;         // the samples of one of its lines
    uint32_t height;        // the lines of the chunk that hold its samples
};

// A chunk's region and channels. `y` is the first line in the part's coordinates, which decides the lines that a
// subsampled channel has.
struct ExrBlock {
    const ExrBlockChannel* channels;
    uint32_t channelCount;
    int32_t y;
    uint32_t width;
    uint32_t height;
};

// Returns true if the channel has samples on line `y` of the part.
inline bool exrChannelSamples(const ExrBlockChannel& channel, int32_t y) {
    return channel.ySampling == 1 || y % int32_t(channel.ySampling) == 0;
}

// The decoding tables of a Huffman coding.
struct ExrHuffman;

// What the decompressors of one reader share: the zlib decoder, scratch storage that grows to the largest chunk, and
// the tables of PIZ, B44 and DWA, made the first time a chunk needs them. One thread uses it at a time.
struct ExrCodec {
    const PrCompressCodecSet* PR_NULLABLE compress;
    PrCompressDecoder* PR_NULLABLE zlib;
    RawArray<uint8_t> scratch;
    RawArray<uint8_t> scratch2;
    RawArray<uint16_t> words;
    ExrHuffman* PR_NULLABLE huffman;
    uint16_t* PR_NULLABLE b44Log;       // B44's table of a pLinear channel's samples, by their bits
    uint16_t* PR_NULLABLE dwaToLinear;  // DWA's table from its nonlinear half samples to linear ones, by their bits
};

// Frees what the codec made.
void exrCodecReset(ExrCodec& codec);

// Inflates the zlib stream `packed` into exactly `size` bytes at `out`. Returns InvalidData for any other stream.
PrStatus exrInflate(ExrCodec& codec, PrByteView packed, uint8_t* out, size_t size);

// Reverses ZIP's and RLE's preparation of the bytes, in `count` bytes at `bytes` into `out`: their deltas, then their
// split into the bytes at even offsets and those at odd ones.
void exrUnpredict(uint8_t* bytes, uint8_t* out, size_t count);

// Decompresses the chunk `packed` of `block` by `compression` into `out`, which holds the chunk's `size` bytes of
// pixels. A chunk whose bytes are as many as its pixels' is stored uncompressed, which the caller handles. Returns
// InvalidData for bytes that do not decompress to exactly `size` bytes, and NotSupported for a compression that the
// codec does not decode, or a zlib stream if `codec.compress` does not decode zlib.
PrStatus exrDecompress(ExrCodec& codec, ExrScheme compression, const ExrBlock& block, PrByteView packed,
    uint8_t* out, size_t size);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
