// Media segments: the byte streams that Media Source Extensions append, parsed into initialization segments and coded
// frames (media-source-2/#byte-stream-formats).
//
// A parser reads one byte stream in pieces of any size. It reports each initialization segment, and each coded frame
// once all of its bytes have arrived. A parser is used by one thread at a time.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The byte stream formats that a parser reads (media-source-2/#byte-stream-formats).
enum {
    PrMediaByteStream_None,
    PrMediaByteStream_Mp4,          // mse-byte-stream-format-isobmff
};
typedef uint8_t PrMediaByteStream;

// Returns the byte stream format of the MIME type `type`, ignoring its parameters, or None for a type that no parser
// reads.
PrMediaByteStream prMediaByteStreamFromType(PrStringView type);

// The kinds of track that a byte stream holds.
enum {
    PrMediaTrackKind_Other,
    PrMediaTrackKind_Audio,
    PrMediaTrackKind_Video,
};
typedef uint8_t PrMediaTrackKind;

// A track of an initialization segment. Its views are valid until the parser reads the next initialization segment or
// is destroyed.
typedef struct PrMediaTrackInfo {
    uint32_t id;                    // the byte stream's track ID
    PrMediaTrackKind kind;
    PrStringView codec;             // the codec as a codecs parameter names it (rfc6381), such as avc1.4D401E
    PrStringView language;          // a BCP 47 tag, or empty if the track has none
    PrByteView config;              // the codec's configuration record, such as avcC's or an AudioSpecificConfig
    PrUVec2 extent;                 // the extent of a video track's frames, or zero
    uint32_t sampleRate;            // the sample rate of an audio track, or zero
    uint32_t channels;              // the channels of an audio track, or zero
} PrMediaTrackInfo;

// An initialization segment (media-source-2/#dfn-initialization-segment).
typedef struct PrMediaInitSegment {
    double duration;                // in seconds, or NaN if the segment does not give it
    uint32_t trackCount;
    const PrMediaTrackInfo* tracks;
} PrMediaInitSegment;

// A coded frame (media-source-2/#dfn-coded-frame). Its bytes are valid only during the call that reports it.
typedef struct PrMediaCodedFrame {
    uint32_t track;                 // the ID of its track
    double presentation;            // its presentation timestamp, in seconds
    double decode;                  // its decode timestamp, in seconds
    double duration;                // in seconds
    bool key;                       // a random access point, which decodes without the frames before it
    PrByteView bytes;
} PrMediaCodedFrame;

// Called with an initialization segment.
typedef void (*PrMediaInitSegmentFn)(void* PR_NULLABLE context, const PrMediaInitSegment* segment);

// Called with coded frames in decode order within each track.
typedef void (*PrMediaCodedFramesFn)(void* PR_NULLABLE context, const PrMediaCodedFrame* frames, uint32_t count);

// What a parser reports to.
typedef struct PrMediaSegmentHandlers {
    PrMediaInitSegmentFn initSegment;
    PrMediaCodedFramesFn codedFrames;
    void* PR_NULLABLE context;
} PrMediaSegmentHandlers;

// Parses a byte stream of one format.
typedef struct PrMediaSegmentParser PrMediaSegmentParser;

// Creates a parser of `format`, which must not be None.
PrMediaSegmentParser* prMediaSegmentParserCreate(PrMediaByteStream format);

// Frees the parser.
void prMediaSegmentParserDestroy(PrMediaSegmentParser* PR_NULLABLE parser);

// Parses `bytes`, the next piece of the byte stream, and reports what it completes to `handlers` during the call.
// Returns InvalidData if the stream does not follow its format, after which the parser must be reset before it reads
// more (media-source-2/#sourcebuffer-append-error).
PrStatus prMediaSegmentParserAppend(PrMediaSegmentParser* parser, PrByteView bytes, const PrMediaSegmentHandlers* handlers);

// Returns true if the parser has read part of a media segment, and waits for the rest
// (media-source-2/#dfn-append-state). A parser that waits must not have its timestamp offset changed.
bool prMediaSegmentParserIsParsingMedia(const PrMediaSegmentParser* parser);

// Drops what the parser has read of an incomplete segment, so that the next bytes begin a segment
// (media-source-2/#sourcebuffer-reset-parser-state). An initialization segment that the parser read stays.
void prMediaSegmentParserReset(PrMediaSegmentParser* parser);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
