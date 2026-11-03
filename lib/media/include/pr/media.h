// Media: reading audio and video resources, and decoding their audio and the frames of their video.
//
// A codec may be used from any number of threads at once. A decoder is used by one thread at a time, and decodes ahead
// on threads of its own. The exception is prMediaDecoderAudioRead, which one other thread may call at any time.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/media_segment.h"
#include "pr/media_source.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A source of decoders. Each backend provides its own constructor.
typedef struct PrMediaCodec PrMediaCodec;

// The state of one resource that a codec opened: what it holds, and the frames of its video.
typedef struct PrMediaDecoder PrMediaDecoder;

// How likely a codec is to read a resource of a type (html/#canplaytyperesult).
enum {
    PrMediaSupport_No,
    PrMediaSupport_Maybe,
    PrMediaSupport_Probably,
};
typedef uint8_t PrMediaSupport;

// What a resource holds, as its container gives it.
typedef struct PrMediaInfo {
    double duration;        // in seconds, or NaN if the container does not give it
    PrUVec2 videoExtent;    // the extent of the first video track as it displays, or zero if the resource has none
    bool hasVideo;
    bool hasAudio;
} PrMediaInfo;

// A decoded frame of a resource's video: opaque sRGB in Bgra8.
typedef struct PrMediaFrame {
    PrPixelView pixels;
    double time;            // when the frame starts to show, in seconds on the resource's timeline
    uint64_t serial;        // distinct for each frame that a decoder returns

    // The frame is the one that shows at the time that was asked for, rather than the last that the decoder reached on
    // its way there.
    bool current;
} PrMediaFrame;

// Samples of a resource's audio that one read gave.
typedef struct PrMediaAudioChunk {
    uint32_t frames;
    double time;            // where the first frame lies on the resource's timeline, in seconds
    bool ended;             // no frames follow until the decoder seeks
} PrMediaAudioChunk;

// The start of every decoder's state. `codec` is the codec that created the decoder.
struct PrMediaDecoder {
    PrMediaCodec* codec;
};

// A decoder of one track's coded frames, one frame at a time, which a codec creates for a media source. It is used by
// one thread at a time. `kind` is the kind of its track.
typedef struct PrMediaSampleDecoder PrMediaSampleDecoder;
struct PrMediaSampleDecoder {
    PrMediaCodec* codec;
    PrMediaTrackKind kind;
};

// What a sample decoder produces as it decodes, during the call that decodes. A video decoder gives images, each with
// one reference that passes to the sink, which the codec's imageRelease releases. An audio decoder gives interleaved
// samples in the format that it was created for, which are valid only during the call, and `time` says where the first
// lies on the track's timeline.
typedef struct PrMediaSampleSink {
    void (*PR_NULLABLE image)(void* PR_NULLABLE context, void* image, double presentation);
    void (*PR_NULLABLE samples)(void* PR_NULLABLE context, const float* samples, uint32_t frames, double time);
    void* PR_NULLABLE context;
} PrMediaSampleSink;

// The function table of a codec, placed at the start of the backend's state. Each function member implements the public
// function of the same name. A null `destroy` means there is nothing to free, a null `decoderFrame` that the codec
// decodes no frames, and a null `decoderAudioStart` that it decodes no audio, and then `decoderAudioSeek` and
// `decoderAudioRead` are null too. Callers use the functions below, never these members.
//
// The sample members decode the coded frames of media sources. A null `sampleDecoderCreate` means that the codec
// decodes none, and then the other sample members are null too, and a null `supportsSourceType` means that the codec's
// answer for a media source is its answer for a resource. `sampleDecode` returns false if the frame does not decode,
// and `sampleDecoderFlush` drops what the decoder holds of the frames before, for a jump on the timeline. An image is a
// decoded video frame: `imageLock` makes its pixels readable, which stay so until `imageUnlock`.
struct PrMediaCodec {
    void (*PR_NULLABLE destroy)(PrMediaCodec* codec);
    PrMediaSupport (*supportsType)(PrMediaCodec* codec, PrStringView type);
    PrStatus (*decoderCreate)(PrMediaCodec* codec, PrByteView bytes, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out,
        PrMediaInfo* info);
    void (*decoderDestroy)(PrMediaCodec* codec, PrMediaDecoder* decoder);
    bool (*PR_NULLABLE decoderFrame)(PrMediaCodec* codec, PrMediaDecoder* decoder, double seconds, PrMediaFrame* out);
    bool (*PR_NULLABLE decoderAudioStart)(PrMediaCodec* codec, PrMediaDecoder* decoder, uint32_t sampleRate,
        uint32_t channels, double seconds);
    void (*PR_NULLABLE decoderAudioSeek)(PrMediaCodec* codec, PrMediaDecoder* decoder, double seconds);
    PrMediaAudioChunk (*PR_NULLABLE decoderAudioRead)(PrMediaCodec* codec, PrMediaDecoder* decoder, float* samples,
        uint32_t frames);
    PrMediaSupport (*PR_NULLABLE supportsSourceType)(PrMediaCodec* codec, PrStringView type);
    PrStatus (*PR_NULLABLE sampleDecoderCreate)(PrMediaCodec* codec, const PrMediaTrackInfo* track, uint32_t sampleRate,
        uint32_t channels, PrMediaSampleDecoder* PR_NULLABLE* PR_NONNULL out);
    void (*PR_NULLABLE sampleDecoderDestroy)(PrMediaCodec* codec, PrMediaSampleDecoder* decoder);
    bool (*PR_NULLABLE sampleDecode)(PrMediaCodec* codec, PrMediaSampleDecoder* decoder,
        const PrMediaBufferedFrame* frame, const PrMediaSampleSink* sink);
    void (*PR_NULLABLE sampleDecoderFlush)(PrMediaCodec* codec, PrMediaSampleDecoder* decoder);
    bool (*PR_NULLABLE imageLock)(PrMediaCodec* codec, void* image, PrPixelView* out);
    void (*PR_NULLABLE imageUnlock)(PrMediaCodec* codec, void* image);
    void (*PR_NULLABLE imageRelease)(PrMediaCodec* codec, void* image);
};

// The library's backends of codecs, which the build turns on (README.md), and Default, the platform's own or else the
// first that the build has.
enum {
    PrMediaCodecBackend_Default,
    PrMediaCodecBackend_Apple,      // AVFoundation
    PrMediaCodecBackend_Windows,    // Media Foundation
    PrMediaCodecBackend_Builtin,
    PrMediaCodecBackend_Count,
};
typedef uint8_t PrMediaCodecBackend;

// Returns the backend that `name` names, as prMediaCodecBackendName spells it, ignoring case, or Count if it names
// none.
PrMediaCodecBackend prMediaCodecBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prMediaCodecBackendName(PrMediaCodecBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prMediaCodecBackendIsBuilt(PrMediaCodecBackend backend);

// Creates a codec by the backend. A backend's own header documents what it reads. Returns null if the build does not
// have the backend, or the backend's constructor fails.
PrMediaCodec* PR_NULLABLE prMediaCodecCreate(PrMediaCodecBackend backend);

// Frees the codec. Its decoders must be destroyed first.
void prMediaCodecDestroy(PrMediaCodec* PR_NULLABLE codec);

// Returns how likely the codec is to read a resource of the MIME type `type`, which may have a codecs parameter
// (rfc6381).
PrMediaSupport prMediaCodecSupportsType(PrMediaCodec* codec, PrStringView type);

// Returns how likely the codec is to play a media source's byte stream of the MIME type `type`, whose codecs it decodes
// one coded frame at a time.
PrMediaSupport prMediaCodecSupportsSourceType(PrMediaCodec* codec, PrStringView type);

// Opens the resource in `bytes`, which the decoder copies, and writes what it holds to `info`. Returns NotSupported if
// the codec does not read the resource, and InvalidData if the resource is corrupt. On failure, `*out` is null.
PrStatus prMediaDecoderCreate(PrMediaCodec* codec, PrByteView bytes, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out,
    PrMediaInfo* info);

// Creates a decoder of the media source whose track buffers are `buffers`: the first video track among them, and the
// first audio track. The decoder reads their frames as script appends them, on threads of its own, and decodes them
// with the codec's sample decoders. A codec that decodes no samples gives a decoder that decodes no frames and no
// audio. The buffers must outlive the decoder. Writes what the tracks hold to `info`, with a duration of NaN.
PrStatus prMediaDecoderCreateFromSource(PrMediaCodec* codec, PrMediaSourceBuffer* PR_NONNULL const* PR_NONNULL buffers,
    uint32_t count, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out, PrMediaInfo* info);

// Tells a decoder of a media source whether the source has ended (media-source-2/#end-of-stream-algorithm). Once it
// has, the decoder's audio ends where the buffered audio does. Does nothing for any other decoder.
void prMediaDecoderSetSourceEnded(PrMediaDecoder* decoder, bool ended);

// Frees the decoder.
void prMediaDecoderDestroy(PrMediaDecoder* PR_NULLABLE decoder);

// Returns true if the decoder decodes the frames of its resource's video.
bool prMediaDecoderHasFrames(const PrMediaDecoder* decoder);

// Writes the frame of the video that shows at `seconds`, the last that starts at or before it, to `out`, or if that
// frame is not decoded yet, the last frame that the decoder reached on its way there. The pixels are valid until the
// next call on the decoder. Returns false if the decoder has no frame to write, or decodes no frames, and then sets
// `out->current` if no frame will come. The call does not wait: the decoder decodes toward `seconds` on its own
// threads, so that a later call returns the frame that shows there. A time before what the decoder kept, or far past
// what it reached, seeks.
bool prMediaDecoderFrame(PrMediaDecoder* decoder, double seconds, PrMediaFrame* out);

// Starts decoding the resource's first audio track from `seconds` on a thread of the decoder's own, converted to
// `sampleRate` and `channels`. A decoder starts at most once. Returns false if the codec decodes no audio, or the
// resource has none that it decodes.
bool prMediaDecoderAudioStart(PrMediaDecoder* decoder, uint32_t sampleRate, uint32_t channels, double seconds);

// Continues the audio from `seconds`. The audio must have started. Reads return nothing until samples from there are
// decoded.
void prMediaDecoderAudioSeek(PrMediaDecoder* decoder, double seconds);

// Writes up to `frames` frames of interleaved samples to `samples`. The audio must have started. The call does not wait
// or allocate, so a real-time thread may make it, while other calls on the decoder run. Destroying the decoder must not
// overlap it.
PrMediaAudioChunk prMediaDecoderAudioRead(PrMediaDecoder* decoder, float* samples, uint32_t frames);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
