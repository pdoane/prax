// Media: reading audio and video resources, and decoding the frames of their video.
//
// A codec may be used from any number of threads at once. A decoder is used by one thread at a time, and decodes ahead
// on threads of its own.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
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

// The start of every decoder's state. `codec` is the codec that created the decoder.
struct PrMediaDecoder {
    PrMediaCodec* codec;
};

// The function table of a codec, placed at the start of the backend's state. Each function member implements the public
// function of the same name. A null `destroy` means there is nothing to free, and a null `decoderFrame` that the codec
// decodes no frames. Callers use the functions below, never these members.
struct PrMediaCodec {
    void (*PR_NULLABLE destroy)(PrMediaCodec* codec);
    PrMediaSupport (*supportsType)(PrMediaCodec* codec, PrStringView type);
    PrStatus (*decoderCreate)(PrMediaCodec* codec, PrByteView bytes, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out,
        PrMediaInfo* info);
    void (*decoderDestroy)(PrMediaCodec* codec, PrMediaDecoder* decoder);
    bool (*PR_NULLABLE decoderFrame)(PrMediaCodec* codec, PrMediaDecoder* decoder, double seconds, PrMediaFrame* out);
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

// Opens the resource in `bytes`, which the decoder copies, and writes what it holds to `info`. Returns NotSupported if
// the codec does not read the resource, and InvalidData if the resource is corrupt. On failure, `*out` is null.
PrStatus prMediaDecoderCreate(PrMediaCodec* codec, PrByteView bytes, PrMediaDecoder* PR_NULLABLE* PR_NONNULL out,
    PrMediaInfo* info);

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

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
