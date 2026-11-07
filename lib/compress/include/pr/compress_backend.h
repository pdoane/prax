// The interface that compression backends implement. Callers use the functions of pr/compress.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a decoder's backend, at the start of its state. Each implements the function of pr/compress.h that
// it is named for.
struct PrCompressDecoder {
    void (*destroyFn)(PrCompressDecoder* decoder);
    PrStatus (*readFn)(PrCompressDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output,
        size_t capacity, PrCompressRead* out);
};

// The functions of a codec's backend, at the start of its state. Each implements the function of pr/compress.h that it
// is named for. A null `destroyFn` means that there is nothing to free, and a null `decodesFn` that the codec decodes
// every format. Null `encodeBoundFn` and `encodeFn` mean that the codec encodes nothing.
struct PrCompressCodec {
    void (*PR_NULLABLE destroyFn)(PrCompressCodec* codec);
    bool (*PR_NULLABLE decodesFn)(PrCompressCodec* codec, PrCompressFormat format);
    PrStatus (*decoderCreateFn)(PrCompressCodec* codec, PrCompressFormat format,
        PrCompressDecoder* PR_NULLABLE* PR_NONNULL out);
    size_t (*PR_NULLABLE encodeBoundFn)(PrCompressCodec* codec, PrCompressFormat format, size_t size);
    PrStatus (*PR_NULLABLE encodeFn)(PrCompressCodec* codec, PrCompressFormat format, PrCompressLevel level,
        PrByteView input, uint8_t* output, size_t* written);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
