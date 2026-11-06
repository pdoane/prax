// Compression backends, which decode compressed byte streams and may encode them.

#include "pr/compress.h"

#include "pr/ascii.h"
#include "pr/assert.h"

#if defined(PR_COMPRESS_HAS_BUILTIN)
#include "pr/compress_builtin.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrCompressCodecBackend_Count] = {"default", "builtin"};

// The backends that the build has, which Default creates the first of, and then Default, so that a build without
// backends creates none.
constexpr PrCompressCodecBackend kBuilt[] = {
#if defined(PR_COMPRESS_HAS_BUILTIN)
    PrCompressCodecBackend_Builtin,
#endif
    PrCompressCodecBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrCompressCodecBackend prCompressCodecBackendFromName(PrStringView name) {
    for (PrCompressCodecBackend backend = 0; backend < PrCompressCodecBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrCompressCodecBackend_Count;
}

PrStringView prCompressCodecBackendName(PrCompressCodecBackend backend) {
    return backend < PrCompressCodecBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prCompressCodecBackendIsBuilt(PrCompressCodecBackend backend) {
    for (PrCompressCodecBackend built : kBuilt) {
        if (built != PrCompressCodecBackend_Default && (built == backend || backend == PrCompressCodecBackend_Default))
            return true;
    }
    return false;
}

PrCompressCodec* PR_NULLABLE prCompressCodecCreate(PrCompressCodecBackend backend) {
    switch (backend == PrCompressCodecBackend_Default ? kBuilt[0] : backend) {
#if defined(PR_COMPRESS_HAS_BUILTIN)
        case PrCompressCodecBackend_Builtin: return prCompressBuiltinCreate();
#endif
        default: return nullptr;
    }
}

void prCompressCodecDestroy(PrCompressCodec* PR_NULLABLE codec) {
    if (codec != nullptr && codec->destroy != nullptr)
        codec->destroy(codec);
}

bool prCompressCodecDecodes(PrCompressCodec* codec, PrCompressFormat format) {
    PR_ASSERT(format <= PrCompressFormat_Brotli);
    return codec->decodes == nullptr || codec->decodes(codec, format);
}

PrStatus prCompressDecoderCreate(PrCompressCodec* codec, PrCompressFormat format,
        PrCompressDecoder* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(format <= PrCompressFormat_Brotli);
    *out = nullptr;
    PrStatus status = codec->decoderCreate(codec, format, out);
    PR_ASSERT(status != PrStatus_Ok || ((*out) != nullptr && (*out)->codec == codec));
    return status;
}

void prCompressDecoderDestroy(PrCompressDecoder* PR_NULLABLE decoder) {
    if (decoder != nullptr)
        decoder->codec->decoderDestroy(decoder->codec, decoder);
}

PrStatus prCompressDecoderRead(PrCompressDecoder* decoder, PrByteView input, bool final,
        uint8_t* PR_NULLABLE output, size_t capacity, PrCompressRead* out) {
    PR_ASSERT(input.data != nullptr || input.size == 0);
    PR_ASSERT(output != nullptr || capacity == 0);
    *out = PrCompressRead{0, 0, PrCompressProgress_Input};
    return decoder->codec->decoderRead(decoder->codec, decoder, input, final, output, capacity, out);
}

PrStatus prCompressCodecDecode(PrCompressCodec* codec, PrCompressFormat format, PrByteView input,
        uint8_t* PR_NULLABLE output, size_t capacity, PrCompressRead* out) {
    *out = PrCompressRead{0, 0, PrCompressProgress_Input};
    PrCompressDecoder* decoder = nullptr;
    PrStatus status = prCompressDecoderCreate(codec, format, &decoder);
    if (status != PrStatus_Ok)
        return status;
    status = prCompressDecoderRead(decoder, input, true, output, capacity, out);
    prCompressDecoderDestroy(decoder);
    return status;
}

size_t prCompressCodecEncodeBound(PrCompressCodec* codec, PrCompressFormat format, size_t size) {
    PR_ASSERT(format <= PrCompressFormat_Brotli);
    return codec->encodeBound != nullptr ? codec->encodeBound(codec, format, size) : 0;
}

PrStatus prCompressCodecEncode(PrCompressCodec* codec, PrCompressFormat format, PrCompressLevel level,
        PrByteView input, uint8_t* output, size_t* written) {
    PR_ASSERT(format <= PrCompressFormat_Brotli && level <= PrCompressLevel_Small);
    PR_ASSERT(input.data != nullptr || input.size == 0);
    *written = 0;
    if (codec->encode == nullptr)
        return PrStatus_NotSupported;
    return codec->encode(codec, format, level, input, output, written);
}
