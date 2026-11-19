// The Deflate codec: lib/deflate as a member of a compression codec set.

#include "pr/compress_deflate.h"

#include "pr/alloc.h"
#include "pr/compress_backend.h"
#include "pr/deflate.h"

namespace pr_impl {

namespace {

struct DeflateCodecDecoder : PrCompressDecoder {
    PrDeflateDecoder* decoder;
};

// The level of the encoder, by PrCompressLevel.
constexpr PrDeflateLevel kLevels[] = {PrDeflateLevel_Fast, PrDeflateLevel_Default, PrDeflateLevel_Small};

bool codes(const PrCompressCodec*, PrCompressFormat format) {
    return format == PrCompressFormat_Deflate;
}

void destroyDecoder(PrCompressDecoder* decoder) {
    DeflateCodecDecoder* deflate = static_cast<DeflateCodecDecoder*>(decoder);
    prDeflateDecoderDestroy(deflate->decoder);
    prMemoryFree(deflate);
}

void reset(PrCompressDecoder* decoder) {
    prDeflateDecoderReset(static_cast<DeflateCodecDecoder*>(decoder)->decoder);
}

PrStatus read(PrCompressDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output, size_t capacity,
        PrCompressRead* out) {
    DeflateCodecDecoder* deflate = static_cast<DeflateCodecDecoder*>(decoder);
    PrDeflateRead read{};
    PrStatus status = prDeflateDecoderRead(deflate->decoder, input, final, output, capacity, &read);
    out->consumed = read.consumed;
    out->written = read.written;
    if (status != PrStatus_Ok)
        return status;
    switch (read.progress) {
        case PrDeflateProgress_Input: out->progress = PrCompressProgress_Input; break;
        case PrDeflateProgress_Output: out->progress = PrCompressProgress_Output; break;
        default: out->progress = PrCompressProgress_Done; break;
    }
    return PrStatus_Ok;
}

PrStatus createDecoder(const PrCompressCodec*, PrCompressFormat, PrCompressDecoder* PR_NULLABLE* PR_NONNULL out) {
    DeflateCodecDecoder* decoder = allocZeroed<DeflateCodecDecoder>();
    decoder->destroyFn = destroyDecoder;
    decoder->resetFn = reset;
    decoder->readFn = read;
    decoder->decoder = prDeflateDecoderCreate();
    *out = decoder;
    return PrStatus_Ok;
}

size_t encodeBound(const PrCompressCodec*, PrCompressFormat, size_t size) {
    return prDeflateEncodeBound(size);
}

PrStatus encode(const PrCompressCodec*, PrCompressFormat, PrCompressLevel level, PrByteView input, uint8_t* output,
        size_t* written) {
    *written = prDeflateEncode(kLevels[level], input, output);
    return PrStatus_Ok;
}

// The codec has no state, so one serves every call and nothing is destroyed.
PrCompressCodec gCodec = {
    .decodesFn = codes,
    .decoderCreateFn = createDecoder,
    .encodesFn = codes,
    .encodeBoundFn = encodeBound,
    .encodeFn = encode,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrCompressCodec* prCompressDeflateCreate(void) {
    return &gCodec;
}
