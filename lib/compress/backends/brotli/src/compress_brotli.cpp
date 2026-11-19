// The Brotli codec: lib/brotli as a member of a compression codec set.

#include "pr/compress_brotli.h"

#include "pr/alloc.h"
#include "pr/brotli.h"
#include "pr/compress_backend.h"

namespace pr_impl {

namespace {

struct BrotliCodecDecoder : PrCompressDecoder {
    PrBrotliDecoder* decoder;
};

bool decodes(const PrCompressCodec*, PrCompressFormat format) {
    return format == PrCompressFormat_Brotli;
}

void destroyDecoder(PrCompressDecoder* decoder) {
    BrotliCodecDecoder* brotli = static_cast<BrotliCodecDecoder*>(decoder);
    prBrotliDecoderDestroy(brotli->decoder);
    prMemoryFree(brotli);
}

void reset(PrCompressDecoder* decoder) {
    prBrotliDecoderReset(static_cast<BrotliCodecDecoder*>(decoder)->decoder);
}

PrStatus read(PrCompressDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output, size_t capacity,
        PrCompressRead* out) {
    BrotliCodecDecoder* brotli = static_cast<BrotliCodecDecoder*>(decoder);
    PrBrotliRead read{};
    PrStatus status = prBrotliDecoderRead(brotli->decoder, input, final, output, capacity, &read);
    // The progresses are the compression interface's, in the same order.
    *out = PrCompressRead{read.consumed, read.written, PrCompressProgress(read.progress)};
    return status;
}

PrStatus createDecoder(const PrCompressCodec*, PrCompressFormat, PrCompressDecoder* PR_NULLABLE* PR_NONNULL out) {
    BrotliCodecDecoder* decoder = allocZeroed<BrotliCodecDecoder>();
    decoder->destroyFn = destroyDecoder;
    decoder->resetFn = reset;
    decoder->readFn = read;
    decoder->decoder = prBrotliDecoderCreate();
    *out = decoder;
    return PrStatus_Ok;
}

// The codec has no state, so one serves every call and nothing is destroyed.
PrCompressCodec gCodec = {.decodesFn = decodes, .decoderCreateFn = createDecoder};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrCompressCodec* prCompressBrotliCreate(void) {
    return &gCodec;
}
