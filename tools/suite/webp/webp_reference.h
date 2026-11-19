// What compares lib/webp's decoder with libwebp's: each decoder's reading of a file, as Rgba8 pixels and, for a lossy
// bitstream, as the planes of its frame; and libwebp's encoders, which write the files of each feature.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

namespace webp {

using namespace pr;

// The pixels of each frame of a file, as straight-alpha Rgba8, packed, one frame after another, and how long each
// shows.
struct Frames {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t playCount = 0;     // zero for forever
    bool alpha = true;          // whether the image has alpha; one without it is opaque
    Array<uint8_t> rgba;
    Array<uint32_t> durations;
};

// A lossy frame's planes: luma at its width and height, and each chroma plane at half of each, rounded up.
struct Planes {
    uint32_t width = 0;
    uint32_t height = 0;
    Array<uint8_t> y;
    Array<uint8_t> u;
    Array<uint8_t> v;
};

// libwebp's decode of the file: WebPDecodeRGBA for a still image, and WebPAnimDecoder for an animation, whose canvas
// starts transparent. Returns false if libwebp refuses the file.
bool referenceFrames(PrByteView file, Frames* out);

// libwebp's planes of the file's lossy bitstream (WebPDecodeYUV). Returns false if libwebp refuses it.
bool referencePlanes(PrByteView file, Planes* out);

// lib/webp's decode of the file, in its own format and expanded to Rgba8. Returns the status of the open or of the
// first read that fails.
PrStatus praxFrames(PrByteView file, Frames* out);

// Decodes every frame of the file into `scratch`, which keeps its storage from one call to the next, with each decoder
// as a program would: lib/webp's in the image's own format, and libwebp's as Rgba8, each without the copies that the
// comparison makes. What a timing measures.
PrStatus praxDecode(PrByteView file, Array<uint8_t>* scratch);
bool referenceDecode(PrByteView file, Array<uint8_t>* scratch);

// The planes of the lossy bitstream of the file's first frame, as lib/webp's VP8 decoder decodes them.
PrStatus praxPlanes(PrByteView file, Planes* out);

// The encoder's choices, as libwebp's WebPConfig names them. The defaults are WebPConfigInit's, lossy at 75.
struct Options {
    bool lossless = false;
    float quality = 75;
    int method = 4;
    bool exact = false;
    int nearLossless = 100;
    int segments = 4;
    int sns = 50;
    int filterStrength = 60;
    int filterSharpness = 0;
    int filterType = 1;
    bool autofilter = false;
    int partitions = 0;
    int alphaCompression = 1;
    int alphaFiltering = 1;
    int alphaQuality = 100;
    bool sharpYuv = false;
};

// Encodes `width` by `height` straight-alpha Rgba8 pixels as a still WebP file.
Array<uint8_t> encode(const uint8_t* rgba, uint32_t width, uint32_t height, const Options& options);

// What WebPAnimEncoder chooses for an animation, as WebPAnimEncoderOptions names it.
struct AnimationOptions {
    int kmin = 0;   // zero for libwebp's default
    int kmax = 0;
    bool allowMixed = false;
    bool minimizeSize = false;
    int loopCount = 0;
};

// Encodes `count` frames of `width` by `height` straight-alpha Rgba8 pixels, one after another in `rgba`, each shown
// for its duration, as an animated WebP file.
Array<uint8_t> encodeAnimation(const uint8_t* rgba, const uint32_t* durations, uint32_t count, uint32_t width,
    uint32_t height, const Options& options, const AnimationOptions& animation);

// Returns `file` with its chunk of the FourCC `fourCc`, such as "ICCP", "EXIF" or "XMP ", set to `payload`, as WebPMux
// writes it.
Array<uint8_t> withChunk(PrByteView file, const char* fourCc, PrByteView payload);

} // namespace webp

PR_ASSUME_NONNULL_END
