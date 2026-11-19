// The Apple codec: Apple's Compression library as a member of a compression codec set. The library's "zlib" is bare
// Deflate, without zlib's frame.

#include "pr/compress_apple.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/compress_backend.h"

#include <compression.h>

namespace pr_impl {

namespace {

struct AppleCodec : PrCompressCodec {
    PrCompressFormat format;
    compression_algorithm algorithm;
};

struct AppleDecoder : PrCompressDecoder {
    compression_stream stream;
    compression_algorithm algorithm;
    bool done;
    bool failed;
};

// Returns the library's algorithm of the bare `format`, which the library codes. Its "zlib" is bare Deflate.
compression_algorithm algorithmOf(PrCompressFormat format) {
    switch (format) {
        case PrCompressFormat_Deflate: return COMPRESSION_ZLIB;
        case PrCompressFormat_Brotli: return COMPRESSION_BROTLI;
        case PrCompressFormat_Lzfse: return COMPRESSION_LZFSE;
        default: break;
    }
    PR_ASSERT(false && "a format that the library codes");
    return COMPRESSION_LZFSE;
}

void destroyCodec(PrCompressCodec* codec) {
    prMemoryFree(static_cast<AppleCodec*>(codec));
}

bool codes(const PrCompressCodec* codec, PrCompressFormat format) {
    return format == static_cast<const AppleCodec*>(codec)->format;
}

void destroyDecoder(PrCompressDecoder* decoder) {
    AppleDecoder* apple = static_cast<AppleDecoder*>(decoder);
    compression_stream_destroy(&apple->stream);
    prMemoryFree(apple);
}

// Starts a new stream with a new stream of the library's, which has no reset of its own.
void reset(PrCompressDecoder* decoder) {
    AppleDecoder* apple = static_cast<AppleDecoder*>(decoder);
    compression_stream_destroy(&apple->stream);
    apple->failed = compression_stream_init(&apple->stream, COMPRESSION_STREAM_DECODE, apple->algorithm) !=
        COMPRESSION_STATUS_OK;
    apple->done = false;
}

PrStatus read(PrCompressDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output, size_t capacity,
        PrCompressRead* out) {
    AppleDecoder* apple = static_cast<AppleDecoder*>(decoder);
    if (apple->done) {
        out->progress = PrCompressProgress_Done;
        return PrStatus_Ok;
    }
    if (apple->failed)
        return PrStatus_InvalidData;
    // The library takes no null pointers, even for nothing.
    uint8_t none = 0;
    apple->stream.src_ptr = input.data != nullptr ? input.data : &none;
    apple->stream.src_size = input.size;
    apple->stream.dst_ptr = output != nullptr ? output : &none;
    apple->stream.dst_size = capacity;
    // The library reports the end of a stream once it reads it, so the decoder never finalizes, which would make a
    // stream that ends early corrupt rather than truncated. A call may stop short of the output that it has, so the
    // decoder calls again while calls make progress.
    compression_status status = COMPRESSION_STATUS_OK;
    for (;;) {
        const size_t before = apple->stream.src_size + apple->stream.dst_size;
        status = compression_stream_process(&apple->stream, 0);
        if (status != COMPRESSION_STATUS_OK || apple->stream.dst_size == 0 ||
                apple->stream.src_size + apple->stream.dst_size == before)
            break;
    }
    out->consumed = input.size - apple->stream.src_size;
    out->written = capacity - apple->stream.dst_size;
    if (status == COMPRESSION_STATUS_END) {
        apple->done = true;
        out->progress = PrCompressProgress_Done;
        return PrStatus_Ok;
    }
    if (status != COMPRESSION_STATUS_OK) {
        apple->failed = true;
        return PrStatus_InvalidData;
    }
    if (apple->stream.dst_size == 0) {
        out->progress = PrCompressProgress_Output;
        return PrStatus_Ok;
    }
    // With room for output left, the library read all of the input.
    if (final) {
        apple->failed = true;
        return PrStatus_Truncated;
    }
    out->progress = PrCompressProgress_Input;
    return PrStatus_Ok;
}

PrStatus createDecoder(const PrCompressCodec* codec, PrCompressFormat, PrCompressDecoder* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    AppleDecoder* decoder = allocZeroed<AppleDecoder>();
    if (compression_stream_init(&decoder->stream, COMPRESSION_STREAM_DECODE,
            static_cast<const AppleCodec*>(codec)->algorithm) != COMPRESSION_STATUS_OK) {
        prMemoryFree(decoder);
        return PrStatus_NotSupported;
    }
    decoder->destroyFn = destroyDecoder;
    decoder->resetFn = reset;
    decoder->readFn = read;
    decoder->algorithm = static_cast<const AppleCodec*>(codec)->algorithm;
    // Given more input than its stream holds, the library reads past the stream's end.
    decoder->readsPastEnd = true;
    *out = decoder;
    return PrStatus_Ok;
}

// The library states no bound. Each of its formats grows input that it cannot shrink by a few bytes per block, far
// below this.
size_t encodeBound(const PrCompressCodec*, PrCompressFormat, size_t size) {
    return size + size / 16 + 1024;
}

PrStatus encode(const PrCompressCodec* codec, PrCompressFormat format, PrCompressLevel, PrByteView input,
        uint8_t* output, size_t* written) {
    *written = 0;
    compression_stream stream;
    if (compression_stream_init(&stream, COMPRESSION_STREAM_ENCODE, static_cast<const AppleCodec*>(codec)->algorithm) !=
            COMPRESSION_STATUS_OK)
        return PrStatus_NotSupported;
    uint8_t none = 0;
    stream.src_ptr = input.data != nullptr ? input.data : &none;
    stream.src_size = input.size;
    stream.dst_ptr = output;
    const size_t capacity = encodeBound(codec, format, input.size);
    stream.dst_size = capacity;
    compression_status status = COMPRESSION_STATUS_OK;
    while (status == COMPRESSION_STATUS_OK && stream.dst_size != 0)
        status = compression_stream_process(&stream, COMPRESSION_STREAM_FINALIZE);
    compression_stream_destroy(&stream);
    // Output that fills the bound means that the bound was wrong.
    PR_ASSERT(status != COMPRESSION_STATUS_OK);
    if (status != COMPRESSION_STATUS_END)
        return PrStatus_InvalidData;
    *written = capacity - stream.dst_size;
    return PrStatus_Ok;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrCompressCodec* prCompressAppleCreate(PrCompressFormat format) {
    PR_ASSERT(format == PrCompressFormat_Deflate || format == PrCompressFormat_Brotli ||
        format == PrCompressFormat_Lzfse);
    AppleCodec* codec = allocZeroed<AppleCodec>();
    codec->destroyFn = destroyCodec;
    codec->decodesFn = codes;
    codec->decoderCreateFn = createDecoder;
    codec->encodesFn = codes;
    codec->encodeBoundFn = encodeBound;
    codec->encodeFn = encode;
    codec->format = format;
    codec->algorithm = algorithmOf(format);
    return codec;
}
