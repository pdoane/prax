// The built-in compression backend.

#include "pr/compress_builtin.h"

#include "pr/alloc.h"

#if defined(PR_COMPRESS_HAS_BROTLI)
#include "compress/backends/builtin/src/brotli.h"
#endif
#include "compress/backends/builtin/src/deflate.h"
#include "compress/backends/builtin/src/inflate.h"

namespace pr_impl {

namespace {

// A decoder of a Deflate format, or of Brotli.
struct BuiltinDecoder : PrCompressDecoder {
    Inflate* PR_NULLABLE inflate;
#if defined(PR_COMPRESS_HAS_BROTLI)
    Brotli* PR_NULLABLE brotli;
#endif
};

// The framing of each Deflate format, by PrCompressFormat.
constexpr InflateFormat kInflateFormat[] = {
    InflateFormat_Raw,  // PrCompressFormat_Deflate
    InflateFormat_Zlib, // PrCompressFormat_Zlib
    InflateFormat_Gzip, // PrCompressFormat_Gzip
};

// Returns true if the build decodes `format`: every Deflate format, and Brotli if the build has it.
bool decodes(PrCompressCodec*, PrCompressFormat format) {
#if defined(PR_COMPRESS_HAS_BROTLI)
    (void)format;
    return true;
#else
    return format != PrCompressFormat_Brotli;
#endif
}

PrStatus createDecoder(PrCompressCodec* codec, PrCompressFormat format, PrCompressDecoder* PR_NULLABLE* PR_NONNULL out) {
    if (!decodes(codec, format))
        return PrStatus_NotSupported;
    BuiltinDecoder* decoder = alloc<BuiltinDecoder>();
    decoder->codec = codec;
    decoder->inflate = nullptr;
#if defined(PR_COMPRESS_HAS_BROTLI)
    decoder->brotli = nullptr;
    if (format == PrCompressFormat_Brotli) {
        decoder->brotli = brotliCreate();
        *out = decoder;
        return PrStatus_Ok;
    }
#endif
    decoder->inflate = inflateCreate(kInflateFormat[format]);
    *out = decoder;
    return PrStatus_Ok;
}

void destroyDecoder(PrCompressCodec*, PrCompressDecoder* decoder) {
    BuiltinDecoder* builtin = static_cast<BuiltinDecoder*>(decoder);
    inflateDestroy(builtin->inflate);
#if defined(PR_COMPRESS_HAS_BROTLI)
    brotliDestroy(builtin->brotli);
#endif
    prMemoryFree(builtin);
}

PrStatus read(PrCompressCodec*, PrCompressDecoder* decoder, PrByteView input, bool final,
        uint8_t* PR_NULLABLE output, size_t capacity, PrCompressRead* out) {
    BuiltinDecoder* builtin = static_cast<BuiltinDecoder*>(decoder);
#if defined(PR_COMPRESS_HAS_BROTLI)
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
#endif
    InflateResult result = inflateRead(builtin->inflate, input, final, &out->consumed, output, capacity, &out->written);
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
PrCompressCodec gBackend = {
    .decodes = decodes,
    .decoderCreate = createDecoder,
    .decoderDestroy = destroyDecoder,
    .decoderRead = read,
    .encodeBound = encodeBound,
    .encode = encode,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrCompressCodec* prCompressBuiltinCreate(void) {
    return &gBackend;
}
