// The LZFSE codec: lib/lzfse as a member of a compression codec set.

#include "pr/compress_lzfse.h"

#include "pr/alloc.h"
#include "pr/compress_backend.h"
#include "pr/lzfse.h"

namespace pr_impl {

namespace {

struct LzfseCodecDecoder : PrCompressDecoder {
    PrLzfseDecoder* decoder;
};

bool decodes(const PrCompressCodec*, PrCompressFormat format) {
    return format == PrCompressFormat_Lzfse;
}

void destroyDecoder(PrCompressDecoder* decoder) {
    LzfseCodecDecoder* lzfse = static_cast<LzfseCodecDecoder*>(decoder);
    prLzfseDecoderDestroy(lzfse->decoder);
    prMemoryFree(lzfse);
}

void reset(PrCompressDecoder* decoder) {
    prLzfseDecoderReset(static_cast<LzfseCodecDecoder*>(decoder)->decoder);
}

PrStatus read(PrCompressDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output, size_t capacity,
        PrCompressRead* out) {
    LzfseCodecDecoder* lzfse = static_cast<LzfseCodecDecoder*>(decoder);
    PrLzfseRead read{};
    PrStatus status = prLzfseDecoderRead(lzfse->decoder, input, final, output, capacity, &read);
    // The progresses are the compression interface's, in the same order.
    *out = PrCompressRead{read.consumed, read.written, PrCompressProgress(read.progress)};
    return status;
}

PrStatus createDecoder(const PrCompressCodec*, PrCompressFormat, PrCompressDecoder* PR_NULLABLE* PR_NONNULL out) {
    LzfseCodecDecoder* decoder = allocZeroed<LzfseCodecDecoder>();
    decoder->destroyFn = destroyDecoder;
    decoder->resetFn = reset;
    decoder->readFn = read;
    decoder->decoder = prLzfseDecoderCreate();
    *out = decoder;
    return PrStatus_Ok;
}

// The codec has no state, so one serves every call and nothing is destroyed.
PrCompressCodec gCodec = {.decodesFn = decodes, .decoderCreateFn = createDecoder};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrCompressCodec* prCompressLzfseCreate(void) {
    return &gCodec;
}
