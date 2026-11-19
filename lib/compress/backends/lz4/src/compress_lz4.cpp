// The LZ4 codec: lib/lz4 as a member of a compression codec set.

#include "pr/compress_lz4.h"

#include "pr/alloc.h"
#include "pr/compress_backend.h"
#include "pr/lz4.h"

namespace pr_impl {

namespace {

struct Lz4CodecDecoder : PrCompressDecoder {
    PrLz4Decoder* decoder;
};

bool decodes(const PrCompressCodec*, PrCompressFormat format) {
    return format == PrCompressFormat_Lz4;
}

void destroyDecoder(PrCompressDecoder* decoder) {
    Lz4CodecDecoder* lz4 = static_cast<Lz4CodecDecoder*>(decoder);
    prLz4DecoderDestroy(lz4->decoder);
    prMemoryFree(lz4);
}

void reset(PrCompressDecoder* decoder) {
    prLz4DecoderReset(static_cast<Lz4CodecDecoder*>(decoder)->decoder);
}

PrStatus read(PrCompressDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output, size_t capacity,
        PrCompressRead* out) {
    PrLz4Read read{};
    PrStatus status = prLz4DecoderRead(static_cast<Lz4CodecDecoder*>(decoder)->decoder, input, final, output, capacity, &read);
    // The progresses are the compression interface's, in the same order.
    *out = PrCompressRead{read.consumed, read.written, PrCompressProgress(read.progress)};
    return status;
}

PrStatus createDecoder(const PrCompressCodec*, PrCompressFormat, PrCompressDecoder* PR_NULLABLE* PR_NONNULL out) {
    Lz4CodecDecoder* decoder = allocZeroed<Lz4CodecDecoder>();
    decoder->destroyFn = destroyDecoder;
    decoder->resetFn = reset;
    decoder->readFn = read;
    decoder->decoder = prLz4DecoderCreate();
    *out = decoder;
    return PrStatus_Ok;
}

// The codec has no state, so one serves every call and nothing is destroyed.
PrCompressCodec gCodec = {.decodesFn = decodes, .decoderCreateFn = createDecoder};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrCompressCodec* prCompressLz4Create(void) {
    return &gCodec;
}
