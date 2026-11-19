// The Zstandard codec: lib/zstd as a member of a compression codec set.

#include "pr/compress_zstd.h"

#include "pr/alloc.h"
#include "pr/compress_backend.h"
#include "pr/zstd.h"

namespace pr_impl {

namespace {

struct ZstdCodecDecoder : PrCompressDecoder {
    PrZstdDecoder* decoder;
};

// The level of the encoder, by PrCompressLevel.
constexpr PrZstdLevel kLevels[] = {PrZstdLevel_Fast, PrZstdLevel_Default, PrZstdLevel_Small};

bool codes(const PrCompressCodec*, PrCompressFormat format) {
    return format == PrCompressFormat_Zstd;
}

void destroyDecoder(PrCompressDecoder* decoder) {
    ZstdCodecDecoder* zstd = static_cast<ZstdCodecDecoder*>(decoder);
    prZstdDecoderDestroy(zstd->decoder);
    prMemoryFree(zstd);
}

void reset(PrCompressDecoder* decoder) {
    prZstdDecoderReset(static_cast<ZstdCodecDecoder*>(decoder)->decoder);
}

void setWindowLimit(PrCompressDecoder* decoder, size_t limit) {
    prZstdDecoderSetWindowLimit(static_cast<ZstdCodecDecoder*>(decoder)->decoder, limit);
}

PrStatus read(PrCompressDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output, size_t capacity,
        PrCompressRead* out) {
    PrZstdRead read{};
    PrStatus status = prZstdDecoderRead(static_cast<ZstdCodecDecoder*>(decoder)->decoder, input, final, output,
        capacity, &read);
    // The progresses are the compression interface's, in the same order.
    *out = PrCompressRead{read.consumed, read.written, PrCompressProgress(read.progress)};
    return status;
}

PrStatus createDecoder(const PrCompressCodec*, PrCompressFormat, PrCompressDecoder* PR_NULLABLE* PR_NONNULL out) {
    ZstdCodecDecoder* decoder = allocZeroed<ZstdCodecDecoder>();
    decoder->destroyFn = destroyDecoder;
    decoder->resetFn = reset;
    decoder->setWindowLimitFn = setWindowLimit;
    decoder->readFn = read;
    decoder->decoder = prZstdDecoderCreate(kPrZstdDefaultWindowLimit);
    *out = decoder;
    return PrStatus_Ok;
}

size_t encodeBound(const PrCompressCodec*, PrCompressFormat, size_t size) {
    return prZstdEncodeBound(size);
}

PrStatus encode(const PrCompressCodec*, PrCompressFormat, PrCompressLevel level, PrByteView input, uint8_t* output,
        size_t* written) {
    *written = prZstdEncode(kLevels[level], input, output);
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

PrCompressCodec* prCompressZstdCreate(void) {
    return &gCodec;
}
