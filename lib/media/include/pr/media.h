// Media: reading audio and video resources, and decoding their audio and the frames of their video.
//
// A codec may be used from any number of threads at once. A decoder is used by one thread at a time, and decodes ahead
// on threads of its own. The exceptions are prMediaDecoderAudioRead and prMediaDecoderAudioWait, which one other thread
// may call at any time.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/gpu.h"
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

// A decoded frame of a resource's video: opaque sRGB in Bgra8. A GPU device makes a texture of its image
// (prGpuTextureImport), in place if the device imports the image's kind. The image is the backend's own, such as a
// CVPixelBuffer, or pixels in memory (PrGpuImportKind_Pixels) for a backend that decodes into memory.
typedef struct PrMediaFrame {
    PrGpuImport image;
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

// The library's codec backends, which the build turns on (README.md), and Default. Default is the first backend in the
// build whose constructor succeeds, trying the platform's own first.
enum {
    PrMediaCodecBackend_Default,
    PrMediaCodecBackend_Apple,      // AVFoundation
    PrMediaCodecBackend_Windows,    // Media Foundation
    PrMediaCodecBackend_Web,        // a web page's video element
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
// frame is not decoded yet, the last frame that the decoder reached on its way there. The image is valid until the next
// call on the decoder. Returns false if the decoder has no frame to write, or decodes no frames. In that case, the call
// sets `out->current` if no frame will come. The call does not wait: the decoder decodes toward `seconds` on its own
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

// Waits until a read of `frames` frames would give them all, or the audio has ended, so that what a read gives does not
// depend on how far the decoder's thread has come. The audio must have started. The call may be made where a read may.
// A decoder of a media source's buffers does not wait, because its audio comes from the decoder's user.
void prMediaDecoderAudioWait(PrMediaDecoder* decoder, uint32_t frames);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
