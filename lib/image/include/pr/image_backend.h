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
// prImageDecoderFrameInfo, and `readFn` implements prImageDecoderRead, decoding into pixels of the format and extent
// that the reader reported. A reader may keep the source and the limits that it was opened with until it is closed, and
// it enforces the limits.
struct PrImageReader {
    void (*closeFn)(PrImageReader* reader);
    PrStatus (*frameInfoFn)(PrImageReader* reader, uint32_t frame, PrImageFrameInfo* info);
    PrStatus (*readFn)(PrImageReader* reader, uint32_t frame, PrPixels output);
};

// The functions of a codec's backend, at the start of its state. Each implements the function of pr/image.h that it is
// named for. A null `destroyFn` means that there is nothing to free, and a null `encodeFn` that the codec encodes
// nothing.
//
// prImageDecoderOpen reads as many bytes from the start of the image as `headerBytesFn` returns, and asks `recognizeFn`
// whether the codec decodes them. It then calls `decoderOpenFn`, which creates a reader if its status is Ok or
// Truncated.
struct PrImageCodec {
    void (*PR_NULLABLE destroyFn)(PrImageCodec* codec);
    uint32_t (*headerBytesFn)(const PrImageCodec* codec);
    bool (*supportsTypeFn)(PrImageCodec* codec, PrStringView type);
    bool (*recognizeFn)(PrImageCodec* codec, PrByteView prefix);
    PrStatus (*decoderOpenFn)(PrImageCodec* codec, const PrImageSource* source, const PrImageLimits* limits,
        PrImageReader* PR_NULLABLE* PR_NONNULL out, PrImageInfo* info);
    PrStatus (*PR_NULLABLE encodeFn)(PrImageCodec* codec, PrStringView type, PrPixelView pixels,
        uint8_t* PR_NULLABLE out, size_t capacity, size_t* size);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
