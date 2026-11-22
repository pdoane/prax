// The interface that image codec backends implement. Callers use the functions of pr/image.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a reader's backend, at the start of its state: an open image, which the codec's `decoderOpenFn`
// creates. `closeFn` implements prImageDecoderClose and frees the reader, `frameInfoFn` implements
// prImageDecoderFrameInfo, and `readFn` implements prImageDecoderReadLevel, decoding into pixels of the format that the
// reader reported and of the extent of the level. A reader may keep the source and the limits that it was opened with
// until it is closed, and it enforces the limits. A reader that reports no levels or faces has one of each.
//
// `levelExtentFn` gives the extent of each level of a reader whose levels do not each halve the one before, rounded
// down, and is null for any other.
struct PrImageReader {
    void (*closeFn)(PrImageReader* reader);
    PrStatus (*frameInfoFn)(PrImageReader* reader, uint32_t frame, PrImageFrameInfo* info);
    PrStatus (*readFn)(PrImageReader* reader, uint32_t frame, uint32_t level, PrPixels output);
    PrUVec2 (*PR_NULLABLE levelExtentFn)(const PrImageReader* reader, uint32_t level);

    // The extent of a preview and the read of a preview. A reader sets them if its image has previews
    // (PrImageInfo::previewCount).
    PrUVec2 (*PR_NULLABLE previewExtentFn)(const PrImageReader* reader, uint32_t preview);
    PrStatus (*PR_NULLABLE readPreviewFn)(PrImageReader* reader, uint32_t preview, PrPixels output);
};

// The functions of a codec's backend, at the start of its state. A null `destroyFn` means that there is nothing to
// free, and a null `encodeFn` that the codec encodes nothing.
//
// prImageDecoderOpen reads as many bytes from the start of the image as the largest `headerBytesFn` of the set returns,
// and asks each codec's `recognizeFn` in turn whether it decodes them. It then calls the first such codec's
// `decoderOpenFn`, which creates a reader if its status is Ok or Truncated. `set` is the set that the image opens
// through, which a container uses to open the images that it embeds. `supportsTypeFn` implements
// prImageCodecSetSupportsType for the codec, and `encodeFn` prImageCodecSetEncode, returning NotSupported for a type
// that the codec does not encode.
struct PrImageCodec {
    void (*PR_NULLABLE destroyFn)(PrImageCodec* codec);
    uint32_t (*headerBytesFn)(const PrImageCodec* codec);
    bool (*supportsTypeFn)(const PrImageCodec* codec, PrStringView type);
    bool (*recognizeFn)(const PrImageCodec* codec, PrByteView prefix);
    PrStatus (*decoderOpenFn)(const PrImageCodec* codec, const PrImageCodecSet* set, const PrImageSource* source,
        const PrImageLimits* limits, PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info);
    PrStatus (*PR_NULLABLE encodeFn)(const PrImageCodec* codec, PrStringView type, PrPixelView pixels,
        uint8_t* PR_NULLABLE out, size_t capacity, size_t* size);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
