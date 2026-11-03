// Media sources: the track buffers of a SourceBuffer, which Media Source Extensions append byte streams to
// (media-source-2/#sourcebuffer).
//
// A source buffer parses what is appended, runs coded frame processing on each coded frame, and keeps the frames in
// track buffers (media-source-2/#track-buffers). It answers what it has buffered, removes ranges, and evicts frames to
// stay within its quota. One thread changes a source buffer. Readers on other threads, such as decoders, read its
// frames through the functions that say so, which take the buffer's lock.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/media_segment.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How appended frames are placed on the timeline (media-source-2/#dom-appendmode).
enum {
    PrMediaAppendMode_Segments,     // at the timestamps that they carry
    PrMediaAppendMode_Sequence,     // one after another, whatever they carry
};
typedef uint8_t PrMediaAppendMode;

// A range of time, in seconds, from `start` up to but not including `end`.
typedef struct PrMediaTimeRange {
    double start;
    double end;
} PrMediaTimeRange;

// A frame of a track buffer, as a reader receives it. The reader holds a reference to its bytes, which stay valid until
// it resets the frame with prMediaBufferedFrameReset, whatever happens to the buffer in the meantime.
typedef struct PrMediaBufferedFrame {
    double presentation;
    double decode;
    double duration;
    bool key;
    PrByteView bytes;
    const void* PR_NULLABLE data;   // the reference to the bytes
} PrMediaBufferedFrame;

// Frees the frame's reference to its bytes, and zeroes the frame. Any thread may call this.
void prMediaBufferedFrameReset(PrMediaBufferedFrame* frame);

// The track buffers of one SourceBuffer.
typedef struct PrMediaSourceBuffer PrMediaSourceBuffer;

// Called when an initialization segment arrives, after the buffer has checked its tracks against the first one's.
// `first` is true for the buffer's first initialization segment (media-source-2/#sourcebuffer-init-segment-received).
// Returns false if the user agent does not support the segment's codecs, which is an append error.
typedef bool (*PrMediaSourceInitFn)(void* PR_NULLABLE context, const PrMediaInitSegment* segment, bool first);

// Creates an empty source buffer that parses `format`, which must not be None, and keeps up to `quota` bytes of frames.
PrMediaSourceBuffer* prMediaSourceBufferCreate(PrMediaByteStream format, size_t quota);

// Frees the buffer. No reader may be using it.
void prMediaSourceBufferDestroy(PrMediaSourceBuffer* PR_NULLABLE buffer);

// Runs the segment parser loop on `bytes` (media-source-2/#sourcebuffer-segment-parser-loop): reports each
// initialization segment to `init` with `context`, and runs coded frame processing on each coded frame. Returns
// InvalidData if the stream is corrupt, an initialization segment has no audio or video track or disagrees with the
// first in its tracks, or `init` rejects it, which is an append error (media-source-2/#sourcebuffer-append-error).
PrStatus prMediaSourceBufferAppend(PrMediaSourceBuffer* buffer, PrByteView bytes, PrMediaSourceInitFn init,
    void* PR_NULLABLE context);

// Returns true if the buffer has read part of a media segment and waits for the rest
// (media-source-2/#dom-sourcebuffer-timestampoffset, the PARSING_MEDIA_SEGMENT append state).
bool prMediaSourceBufferIsParsingMedia(const PrMediaSourceBuffer* buffer);

// Drops what the parser holds of an incomplete segment, and makes the next frame of each track start a coded frame
// group (media-source-2/#sourcebuffer-reset-parser-state).
void prMediaSourceBufferResetParser(PrMediaSourceBuffer* buffer);

// The append mode, timestamp offset and append window (media-source-2/#dom-sourcebuffer). Setting the mode to Sequence,
// or the timestamp offset, follows the attribute's setter.
PrMediaAppendMode prMediaSourceBufferMode(const PrMediaSourceBuffer* buffer);
void prMediaSourceBufferSetMode(PrMediaSourceBuffer* buffer, PrMediaAppendMode mode);
double prMediaSourceBufferTimestampOffset(const PrMediaSourceBuffer* buffer);
void prMediaSourceBufferSetTimestampOffset(PrMediaSourceBuffer* buffer, double offset);
PrMediaTimeRange prMediaSourceBufferAppendWindow(const PrMediaSourceBuffer* buffer);
void prMediaSourceBufferSetAppendWindow(PrMediaSourceBuffer* buffer, PrMediaTimeRange window);

// Writes up to `capacity` of the buffered ranges to `out`, and returns their number: the intersection of every track
// buffer's ranges, with the last range of each extended to the highest end if `ended`
// (media-source-2/#dom-sourcebuffer-buffered). Ranges are in time order and do not touch.
uint32_t prMediaSourceBufferBuffered(const PrMediaSourceBuffer* buffer, bool ended, PrMediaTimeRange* PR_NULLABLE out,
    uint32_t capacity);

// The highest end time of any frame in the buffer, or zero if it holds none (media-source-2/#highest-end-timestamp).
double prMediaSourceBufferHighestEnd(const PrMediaSourceBuffer* buffer);

// Removes the frames that present from `start` to `end`, and the frames that depend on them up to the next random
// access point (media-source-2/#sourcebuffer-coded-frame-removal). `duration` is the media source's duration.
void prMediaSourceBufferRemove(PrMediaSourceBuffer* buffer, double start, double end, double duration);

// Evicts frames to make room for `incoming` more bytes, keeping those near `current`, the media element's current
// playback position (media-source-2/#sourcebuffer-coded-frame-eviction). Returns false if the buffer cannot make room,
// for which appendBuffer throws QuotaExceededError.
bool prMediaSourceBufferEvict(PrMediaSourceBuffer* buffer, double current, size_t incoming);

// The number of the buffer's tracks, as its last initialization segment gives them, and each one. A track's views are
// valid until the next initialization segment.
uint32_t prMediaSourceBufferTrackCount(const PrMediaSourceBuffer* buffer);
PrMediaTrackInfo prMediaSourceBufferTrack(const PrMediaSourceBuffer* buffer, uint32_t index);

// Writes to `out` the frame of the track at `index` that a decoder takes next after the frame at `decode`, in decode
// order: the first frame whose decode timestamp is later. With a `decode` of NaN, the random access point at or before
// `presentation` that starts decoding there. Returns false if no such frame is buffered, and then zeroes `out`. The
// caller releases a frame that it receives. Any thread may call this.
bool prMediaSourceBufferNextFrame(PrMediaSourceBuffer* buffer, uint32_t index, double decode, double presentation,
    PrMediaBufferedFrame* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
