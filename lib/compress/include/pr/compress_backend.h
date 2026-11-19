// The interface that compression backends implement. Callers use the functions of pr/compress.h, which call these. Only
// backends include this header.
//
// A codec codes the bare formats: Deflate, Brotli, LZFSE, LZ4 and Zstandard. A set frames zlib and gzip itself, around
// the Deflate of its first codec that codes Deflate, so a set never asks a codec for them.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a decoder's backend, at the start of its state. Each implements the function of pr/compress.h that
// it is named for. A null `setWindowLimitFn` means that the decoder's format bounds its own window.
struct PrCompressDecoder {
    void (*destroyFn)(PrCompressDecoder* decoder);
    void (*resetFn)(PrCompressDecoder* decoder);
    void (*PR_NULLABLE setWindowLimitFn)(PrCompressDecoder* decoder, size_t limit);
    PrStatus (*readFn)(PrCompressDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output,
        size_t capacity, PrCompressRead* out);

    // True if the decoder may read input past the end of its stream. A frame around such a stream ends where the input
    // does, because the frame's trailer is then the end of the input.
    bool readsPastEnd;
};

// The functions of a codec's backend, at the start of its state. A set calls them for the bare formats that the codec
// reports: `decoderCreateFn` for a format that `decodesFn` accepts, and `encodeBoundFn` and `encodeFn` for one that
// `encodesFn` accepts. A null `destroyFn` means that there is nothing to free, and a null `encodesFn` that the codec
// encodes nothing.
struct PrCompressCodec {
    void (*PR_NULLABLE destroyFn)(PrCompressCodec* codec);
    bool (*decodesFn)(const PrCompressCodec* codec, PrCompressFormat format);
    PrStatus (*decoderCreateFn)(const PrCompressCodec* codec, PrCompressFormat format,
        PrCompressDecoder* PR_NULLABLE* PR_NONNULL out);
    bool (*PR_NULLABLE encodesFn)(const PrCompressCodec* codec, PrCompressFormat format);
    size_t (*PR_NULLABLE encodeBoundFn)(const PrCompressCodec* codec, PrCompressFormat format, size_t size);
    PrStatus (*PR_NULLABLE encodeFn)(const PrCompressCodec* codec, PrCompressFormat format, PrCompressLevel level,
        PrByteView input, uint8_t* output, size_t* written);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
