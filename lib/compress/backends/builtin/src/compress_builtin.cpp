// The built-in compression backend.

#include "pr/compress_builtin.h"

#include "pr/alloc.h"

#include "compress/backends/builtin/src/brotli.h"
#include "compress/backends/builtin/src/deflate.h"
#include "compress/backends/builtin/src/inflate.h"

namespace pr_impl {

namespace {

// A decoder of a Deflate format, or of Brotli.
struct BuiltinDecoder : PrCompressDecoder {
    Inflate* PR_NULLABLE inflate;
    Brotli* PR_NULLABLE brotli;
};

// The framing of each Deflate format, by PrCompressFormat.
constexpr InflateFormat kInflateFormat[] = {
    InflateFormat_Raw,  // PrCompressFormat_Deflate
    InflateFormat_Zlib, // PrCompressFormat_Zlib
    InflateFormat_Gzip, // PrCompressFormat_Gzip
};

PrStatus createDecoder(PrCompressCodec* codec, PrCompressFormat format,
                       PrCompressDecoder* PR_NULLABLE* PR_NONNULL out) {
    BuiltinDecoder* decoder = alloc<BuiltinDecoder>();
    decoder->codec = codec;
    decoder->inflate = nullptr;
    decoder->brotli = nullptr;
    if (format == PrCompressFormat_Brotli)
        decoder->brotli = brotliCreate();
    else
        decoder->inflate = inflateCreate(kInflateFormat[format]);
    *out = decoder;
    return PrStatus_Ok;
}

void destroyDecoder(PrCompressCodec*, PrCompressDecoder* decoder) {
    BuiltinDecoder* builtin = static_cast<BuiltinDecoder*>(decoder);
    inflateDestroy(builtin->inflate);
    brotliDestroy(builtin->brotli);
    prMemoryFree(builtin);
}

PrStatus read(PrCompressCodec*, PrCompressDecoder* decoder, PrByteView input, bool final,
              uint8_t* PR_NULLABLE output, size_t capacity, PrCompressRead* out) {
    BuiltinDecoder* builtin = static_cast<BuiltinDecoder*>(decoder);
    if (builtin->brotli != nullptr) {
        switch (brotliRead(builtin->brotli, input, final, &out->consumed, output, capacity, &out->written)) {
            case BrotliResult_Input:
                out->progress = PrCompressProgress_Input;
                return PrStatus_Ok;
            case BrotliResult_Output:
                out->progress = PrCompressProgress_Output;
                return PrStatus_Ok;
            case BrotliResult_Done:
                out->progress = PrCompressProgress_Done;
                return PrStatus_Ok;
            case BrotliResult_Truncated:
                return PrStatus_Truncated;
            default:
                return PrStatus_InvalidData;
        }
    }
    InflateResult result = inflateRead(builtin->inflate, input, final, &out->consumed, output, capacity,
                                       &out->written);
    switch (result) {
        case InflateResult_Input:
            out->progress = PrCompressProgress_Input;
            return PrStatus_Ok;
        case InflateResult_Output:
            out->progress = PrCompressProgress_Output;
            return PrStatus_Ok;
        case InflateResult_Done:
            out->progress = PrCompressProgress_Done;
            return PrStatus_Ok;
        case InflateResult_Dictionary:
            return PrStatus_NotSupported;
        case InflateResult_Truncated:
            return PrStatus_Truncated;
        default:
            return PrStatus_InvalidData;
    }
}

size_t encodeBound(PrCompressCodec*, PrCompressFormat format, size_t size) {
    return format == PrCompressFormat_Brotli ? 0 : deflateBound(kInflateFormat[format], size);
}

PrStatus encode(PrCompressCodec*, PrCompressFormat format, PrCompressLevel level, PrByteView input, uint8_t* output,
                size_t* written) {
    if (format == PrCompressFormat_Brotli)
        return PrStatus_NotSupported;
    *written = deflateEncode(kInflateFormat[format], level, input, output);
    return PrStatus_Ok;
}

// The single backend. It has no state, so nothing is created per call and nothing is destroyed.
PrCompressCodec gBackend = {nullptr, createDecoder, destroyDecoder, read, encodeBound, encode};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrCompressCodec* prCompressBuiltinCreate(void) {
    return &gBackend;
}
