// The interface that media codec backends implement. Callers use the functions of pr/media.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/media.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a decoder's backend, at the start of its state. Each implements the function of pr/media.h that it
// is named for. A null `frameFn` means that the decoder decodes no frames, and a null `audioStartFn` that it decodes no
// audio, and then `audioSeekFn`, `audioReadFn` and `audioWaitFn` are null too. A null `audioWaitFn` means that its
// audio is read without waiting.
struct PrMediaDecoder {
    void (*destroyFn)(PrMediaDecoder* decoder);
    bool (*PR_NULLABLE frameFn)(PrMediaDecoder* decoder, double seconds, PrMediaFrame* out);
    bool (*PR_NULLABLE audioStartFn)(PrMediaDecoder* decoder, uint32_t sampleRate, uint32_t channels, double seconds);
    void (*PR_NULLABLE audioSeekFn)(PrMediaDecoder* decoder, double seconds);
    PrMediaAudioChunk (*PR_NULLABLE audioReadFn)(PrMediaDecoder* decoder, float* samples, uint32_t frames);
    void (*PR_NULLABLE audioWaitFn)(PrMediaDecoder* decoder, uint32_t frames);
};

// A decoder of one track's coded frames, one frame at a time, which a codec creates for a media source. It is used by
// one thread at a time.
typedef struct PrMediaSampleDecoder PrMediaSampleDecoder;

// What a sample decoder produces as it decodes, during the call that decodes. A video decoder gives images, each with
// one reference that passes to the sink, which the codec's imageRelease releases. An audio decoder gives interleaved
// samples in the format that it was created for, which are valid only during the call, and `time` says where the first
// lies on the track's timeline.
typedef struct PrMediaSampleSink {
    void (*PR_NULLABLE image)(void* PR_NULLABLE context, void* image, double presentation);
    void (*PR_NULLABLE samples)(void* PR_NULLABLE context, const float* samples, uint32_t frames, double time);
    void* PR_NULLABLE context;
} PrMediaSampleSink;

// The functions of a sample decoder's backend, at the start of its state. `decodeFn` returns false if the frame does
// not decode, and `flushFn` drops what the decoder holds of the frames before, for a jump on the timeline.
struct PrMediaSampleDecoder {
    void (*destroyFn)(PrMediaSampleDecoder* decoder);
    bool (*decodeFn)(PrMediaSampleDecoder* decoder, const PrMediaBufferedFrame* frame, const PrMediaSampleSink* sink);
    void (*flushFn)(PrMediaSampleDecoder* decoder);
};

// The functions of a codec's backend, at the start of its state. Each implements the function of pr/media.h that it is
// named for. A null `destroyFn` means that there is nothing to free.
//
// The sample functions decode the coded frames of media sources. A null `sampleDecoderCreateFn` means that the codec
// decodes none, and then `imageImportFn` and `imageReleaseFn` are null too. A null `supportsSourceTypeFn` means that
// the codec's answer for a media source is its answer for a resource. An image is a decoded video frame:
// `imageImportFn` writes it as an image to import, which stays valid until `imageReleaseFn`.
struct PrMediaCodec {
    void (*PR_NULLABLE destroyFn)(PrMediaCodec* codec);
    PrMediaSupport (*supportsTypeFn)(PrMediaCodec* codec, PrStringView type);
    PrStatus (*decoderCreateFn)(PrMediaCodec* codec, PrByteView bytes, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out,
        PrMediaInfo* info);
    PrMediaSupport (*PR_NULLABLE supportsSourceTypeFn)(PrMediaCodec* codec, PrStringView type);
    PrStatus (*PR_NULLABLE sampleDecoderCreateFn)(PrMediaCodec* codec, const PrMediaTrackInfo* track,
        uint32_t sampleRate, uint32_t channels, PrMediaSampleDecoder* PR_NULLABLE* PR_NONNULL out);
    void (*PR_NULLABLE imageImportFn)(PrMediaCodec* codec, void* image, PrGpuImport* out);
    void (*PR_NULLABLE imageReleaseFn)(PrMediaCodec* codec, void* image);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
