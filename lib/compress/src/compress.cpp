// Compression codecs and their sets: argument checks, the choice of the codec that codes each format, and the frames
// around Deflate.

#include "pr/compress.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/compress_backend.h"

#include "compress/src/compress_frame.h"

namespace pr_impl {

// The codecs of a set, in priority order, which the set owns.
struct CompressCodecSet {
    PrCompressCodec** codecs;
    uint32_t count;
};

namespace {

// Returns the bare format that codes `format`: Deflate for the frames around it, and the format itself otherwise.
PrCompressFormat bareOf(PrCompressFormat format) {
    return compressIsFramed(format) ? PrCompressFormat_Deflate : format;
}

// Returns the first codec of `set` that decodes `format`, or null.
const PrCompressCodec* PR_NULLABLE decoderOf(const CompressCodecSet& set, PrCompressFormat format) {
    format = bareOf(format);
    for (uint32_t i = 0; i < set.count; ++i) {
        if (set.codecs[i]->decodesFn(set.codecs[i], format))
            return set.codecs[i];
    }
    return nullptr;
}

// Returns the first codec of `set` that encodes `format`, or null.
const PrCompressCodec* PR_NULLABLE encoderOf(const CompressCodecSet& set, PrCompressFormat format) {
    format = bareOf(format);
    for (uint32_t i = 0; i < set.count; ++i) {
        const PrCompressCodec* codec = set.codecs[i];
        if (codec->encodesFn != nullptr && codec->encodesFn(codec, format))
            return codec;
    }
    return nullptr;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

struct PrCompressCodecSet : CompressCodecSet {};

void prCompressCodecDestroy(PrCompressCodec* PR_NULLABLE codec) {
    if (codec != nullptr && codec->destroyFn != nullptr)
        codec->destroyFn(codec);
}

PrCompressCodecSet* prCompressCodecSetCreate(PrCompressCodec* PR_NONNULL const* PR_NULLABLE codecs, uint32_t count) {
    PrCompressCodecSet* set = allocZeroed<PrCompressCodecSet>();
    set->codecs = count != 0 ? static_cast<PrCompressCodec**>(prMemoryAlloc(sizeof(PrCompressCodec*) * count)) : nullptr;
    for (uint32_t i = 0; i < count; ++i) {
        PR_ASSERT(codecs[i] != nullptr);
        set->codecs[i] = codecs[i];
    }
    set->count = count;
    return set;
}

void prCompressCodecSetDestroy(PrCompressCodecSet* PR_NULLABLE set) {
    if (set == nullptr)
        return;
    for (uint32_t i = 0; i < set->count; ++i)
        prCompressCodecDestroy(set->codecs[i]);
    prMemoryFree(set->codecs);
    prMemoryFree(set);
}

bool prCompressCodecSetDecodes(const PrCompressCodecSet* set, PrCompressFormat format) {
    PR_ASSERT(format < PrCompressFormat_Count);
    return decoderOf(*set, format) != nullptr;
}

bool prCompressCodecSetEncodes(const PrCompressCodecSet* set, PrCompressFormat format) {
    PR_ASSERT(format < PrCompressFormat_Count);
    return encoderOf(*set, format) != nullptr;
}

PrStatus prCompressDecoderCreate(const PrCompressCodecSet* set, PrCompressFormat format,
        PrCompressDecoder* PR_NULLABLE* PR_NONNULL out) {
    PR_ASSERT(format < PrCompressFormat_Count);
    *out = nullptr;
    const PrCompressCodec* codec = decoderOf(*set, format);
    if (codec == nullptr)
        return PrStatus_NotSupported;
    PrStatus status = PrStatus_Ok;
    if (compressIsFramed(format))
        status = compressFrameDecoderCreate(format, codec, out);
    else
        status = codec->decoderCreateFn(codec, format, out);
    PR_ASSERT(status != PrStatus_Ok || *out != nullptr);
    return status;
}

void prCompressDecoderDestroy(PrCompressDecoder* PR_NULLABLE decoder) {
    if (decoder != nullptr)
        decoder->destroyFn(decoder);
}

void prCompressDecoderReset(PrCompressDecoder* decoder) {
    decoder->resetFn(decoder);
}

void prCompressDecoderSetWindowLimit(PrCompressDecoder* decoder, size_t limit) {
    if (decoder->setWindowLimitFn != nullptr)
        decoder->setWindowLimitFn(decoder, limit);
}

PrStatus prCompressDecoderRead(PrCompressDecoder* decoder, PrByteView input, bool final,
        uint8_t* PR_NULLABLE output, size_t capacity, PrCompressRead* out) {
    PR_ASSERT(input.data != nullptr || input.size == 0);
    PR_ASSERT(output != nullptr || capacity == 0);
    *out = PrCompressRead{0, 0, PrCompressProgress_Input};
    return decoder->readFn(decoder, input, final, output, capacity, out);
}

PrStatus prCompressCodecSetDecode(const PrCompressCodecSet* set, PrCompressFormat format, PrByteView input,
        uint8_t* PR_NULLABLE output, size_t capacity, PrCompressRead* out) {
    *out = PrCompressRead{0, 0, PrCompressProgress_Input};
    PrCompressDecoder* decoder = nullptr;
    PrStatus status = prCompressDecoderCreate(set, format, &decoder);
    if (status != PrStatus_Ok)
        return status;
    status = prCompressDecoderRead(decoder, input, true, output, capacity, out);
    prCompressDecoderDestroy(decoder);
    return status;
}

size_t prCompressCodecSetEncodeBound(const PrCompressCodecSet* set, PrCompressFormat format, size_t size) {
    PR_ASSERT(format < PrCompressFormat_Count);
    const PrCompressCodec* codec = encoderOf(*set, format);
    if (codec == nullptr)
        return 0;
    if (compressIsFramed(format))
        return codec->encodeBoundFn(codec, PrCompressFormat_Deflate, size) + compressFrameSize(format);
    return codec->encodeBoundFn(codec, format, size);
}

PrStatus prCompressCodecSetEncode(const PrCompressCodecSet* set, PrCompressFormat format, PrCompressLevel level,
        PrByteView input, uint8_t* output, size_t* written) {
    PR_ASSERT(format < PrCompressFormat_Count && level < PrCompressLevel_Count);
    PR_ASSERT(input.data != nullptr || input.size == 0);
    *written = 0;
    const PrCompressCodec* codec = encoderOf(*set, format);
    if (codec == nullptr)
        return PrStatus_NotSupported;
    if (!compressIsFramed(format))
        return codec->encodeFn(codec, format, level, input, output, written);
    const size_t header = compressFrameWriteHeader(format, level, output);
    size_t data = 0;
    PrStatus status = codec->encodeFn(codec, PrCompressFormat_Deflate, level, input, output + header, &data);
    if (status != PrStatus_Ok)
        return status;
    *written = header + data + compressFrameWriteTrailer(format, input, output + header + data);
    return PrStatus_Ok;
}
