// Media sources: the track buffers of a SourceBuffer, which Media Source Extensions append byte streams to.

#include "pr/media_source.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/sort.h"

namespace pr_impl {

namespace {

// The bytes of a coded frame, which the buffer and its readers share.
struct FrameData {
    Atomic<uint32_t> references;
    uint32_t size;                  // of the bytes, which follow the structure
};

const uint8_t* bytesOf(const FrameData* data) {
    return reinterpret_cast<const uint8_t*>(data + 1);
}

FrameData* frameDataCreate(PrByteView bytes) {
    FrameData* data = static_cast<FrameData*>(prMemoryAlloc(sizeof(FrameData) + bytes.size));
    data->references.storeRelaxed(1);
    data->size = uint32_t(bytes.size);
    if (bytes.size != 0)
        prMemCopy(data + 1, bytes.data, bytes.size);
    return data;
}

void frameDataRelease(const FrameData* data) {
    FrameData* mutableData = const_cast<FrameData*>(data);
    if (mutableData->references.fetchSub(1) == 1)
        prMemoryFree(mutableData);
}

// A coded frame in a track buffer.
struct BufferFrame {
    double presentation;
    double decode;
    double duration;
    bool key;
    FrameData* data;
};

// A track buffer (media-source-2/#track-buffers): its frames in decode order, and the state of coded frame processing.
struct TrackBuffer {
    uint32_t id;
    PrMediaTrackKind kind;
    RawArray<BufferFrame> frames;
    double lastDecode;              // NaN while unset
    double lastDuration;            // NaN while unset
    double highestEnd;              // NaN while unset
    bool needRandomAccess;

    // The largest gap between a frame's decode and presentation timestamps, and the longest frame, which bound the
    // frames that a new frame can overlap.
    double maxReorder;
    double maxDuration;
};

// A copy of the last initialization segment's tracks, whose views point into `text` and `configs`.
struct TrackCopy {
    PrMediaTrackInfo info;
    uint32_t codecAt;
    uint32_t languageAt;
    uint32_t configAt;
};

struct MediaSourceBuffer {
    PrMutex lock;                   // guards the track buffers' frames, which readers take
    PrMediaSegmentParser* parser;
    PrMediaAppendMode mode;
    double timestampOffset;
    double groupStart;              // NaN while unset
    double groupEnd;
    PrMediaTimeRange appendWindow;
    RawArray<TrackBuffer> tracks;
    bool initialized;               // the first initialization segment has been received
    size_t quota;
    size_t bytes;                   // of every frame held

    RawArray<TrackCopy> copies;
    RawArray<char> text;
    RawArray<uint8_t> configs;

    RawArray<uint32_t> order;          // scratch: the order in which a segment's frames are processed

    // The append in progress: its callback, and whether an initialization segment failed the checks.
    PrMediaSourceInitFn init;
    void* initContext;
    bool failed;
};

void releaseFrames(MediaSourceBuffer* b, TrackBuffer* track, uint32_t from, uint32_t count) {
    for (uint32_t i = from; i < from + count; ++i) {
        b->bytes -= track->frames.data[i].data->size;
        frameDataRelease(track->frames.data[i].data);
    }
    track->frames.erase(from, count);
}

void resetTrack(TrackBuffer* track) {
    track->lastDecode = kPrF64Nan;
    track->lastDuration = kPrF64Nan;
    track->highestEnd = kPrF64Nan;
    track->needRandomAccess = true;
}

TrackBuffer* trackOf(MediaSourceBuffer* b, uint32_t id) {
    for (uint32_t i = 0; i < b->tracks.count; ++i) {
        if (b->tracks.data[i].id == id)
            return &b->tracks.data[i];
    }
    return nullptr;
}

uint32_t countOf(const PrMediaInitSegment* segment, PrMediaTrackKind kind) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < segment->trackCount; ++i)
        count += segment->tracks[i].kind == kind ? 1 : 0;
    return count;
}

// Copies the segment's tracks, so that they outlive the parser's next segment.
void copyTracks(MediaSourceBuffer* b, const PrMediaInitSegment* segment) {
    b->copies.clear();
    b->text.clear();
    b->configs.clear();
    for (uint32_t i = 0; i < segment->trackCount; ++i) {
        const PrMediaTrackInfo& info = segment->tracks[i];
        TrackCopy copy{info, b->text.count, 0, b->configs.count};
        if (info.codec.size != 0)
            b->text.append(info.codec.data, uint32_t(info.codec.size));
        copy.languageAt = b->text.count;
        if (info.language.size != 0)
            b->text.append(info.language.data, uint32_t(info.language.size));
        if (info.config.size != 0)
            b->configs.append(info.config.data, uint32_t(info.config.size));
        b->copies.push(copy);
    }
}

// media-source-2/#sourcebuffer-init-segment-received
void initSegmentReceived(void* context, const PrMediaInitSegment* segment) {
    MediaSourceBuffer* b = static_cast<MediaSourceBuffer*>(context);
    if (b->failed)
        return;
    bool first = !b->initialized;
    if (countOf(segment, PrMediaTrackKind_Audio) + countOf(segment, PrMediaTrackKind_Video) == 0) {
        b->failed = true;
        return;
    }
    if (!first) {
        // A later segment has the same number of tracks of each kind. Where a kind has more than one track, their IDs
        // match the first segment's, and where it has one, the ID may change.
        constexpr PrMediaTrackKind kKinds[] = {PrMediaTrackKind_Audio, PrMediaTrackKind_Video, PrMediaTrackKind_Other};
        for (PrMediaTrackKind kind : kKinds) {
            uint32_t count = 0;
            for (uint32_t i = 0; i < b->tracks.count; ++i)
                count += b->tracks.data[i].kind == kind ? 1 : 0;
            if (count != countOf(segment, kind)) {
                b->failed = true;
                return;
            }
        }
        for (uint32_t i = 0; i < segment->trackCount; ++i) {
            const PrMediaTrackInfo& info = segment->tracks[i];
            bool matched = false;
            for (uint32_t k = 0; k < b->copies.count && !matched; ++k) {
                const PrMediaTrackInfo& old = b->copies.data[k].info;
                matched = old.kind == info.kind && (old.id == info.id || countOf(segment, info.kind) == 1);
            }
            if (!matched) {
                b->failed = true;
                return;
            }
        }
        // A track that is the only one of its kind takes the new segment's ID.
        for (uint32_t i = 0; i < segment->trackCount; ++i) {
            const PrMediaTrackInfo& info = segment->tracks[i];
            if (countOf(segment, info.kind) != 1)
                continue;
            for (uint32_t k = 0; k < b->tracks.count; ++k) {
                if (b->tracks.data[k].kind == info.kind)
                    b->tracks.data[k].id = info.id;
            }
        }
    } else {
        prMutexLock(&b->lock);
        for (uint32_t i = 0; i < segment->trackCount; ++i) {
            TrackBuffer track{};
            track.id = segment->tracks[i].id;
            track.kind = segment->tracks[i].kind;
            resetTrack(&track);
            b->tracks.push(track);
        }
        prMutexUnlock(&b->lock);
        b->initialized = true;
    }
    // The buffer's tracks are the segment's before the callback, which may read them.
    copyTracks(b, segment);
    for (uint32_t i = 0; i < b->tracks.count; ++i)
        b->tracks.data[i].needRandomAccess = true;
    if (b->init != nullptr && !b->init(b->initContext, segment, first))
        b->failed = true;
}

// Returns the index of the first frame whose decode timestamp is at or after `decode`.
uint32_t lowerDecode(const TrackBuffer* track, double decode) {
    uint32_t low = 0;
    uint32_t high = track->frames.count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (track->frames.data[mid].decode < decode)
            low = mid + 1;
        else
            high = mid;
    }
    return low;
}

// Removes the frames whose presentation timestamps are in [start, end), and then the frames that depend on each, up to
// the next random access point in decode order. Frames are found among those whose decode timestamps can place them in
// the range. Returns the decode index at which the first removal happened, or UINT32_MAX for none.
uint32_t removePresenting(MediaSourceBuffer* b, TrackBuffer* track, double start, double end) {
    uint32_t first = UINT32_MAX;
    uint32_t at = lowerDecode(track, start - track->maxReorder - track->maxDuration);
    while (at < track->frames.count) {
        const BufferFrame& frame = track->frames.data[at];
        if (frame.decode > end + track->maxReorder)
            break;
        if (frame.presentation >= start && frame.presentation < end) {
            first = first < at ? first : at;
            releaseFrames(b, track, at, 1);
            // The frames after it in decode order up to the next random access point depend on it.
            uint32_t run = 0;
            while (at + run < track->frames.count && !track->frames.data[at + run].key)
                ++run;
            releaseFrames(b, track, at, run);
            continue;
        }
        ++at;
    }
    return first;
}

// Inserts a frame in decode order.
void insertFrame(MediaSourceBuffer* b, TrackBuffer* track, const BufferFrame& frame) {
    uint32_t at = track->frames.count;
    if (at != 0 && track->frames.data[at - 1].decode > frame.decode)
        at = lowerDecode(track, frame.decode);
    track->frames.insert(at, frame);
    b->bytes += frame.data->size;
    double reorder = frame.presentation - frame.decode;
    reorder = reorder < 0 ? -reorder : reorder;
    track->maxReorder = reorder > track->maxReorder ? reorder : track->maxReorder;
    track->maxDuration = frame.duration > track->maxDuration ? frame.duration : track->maxDuration;
}

// The coded frame processing algorithm for one frame (media-source-2/#sourcebuffer-coded-frame-processing).
void processFrame(MediaSourceBuffer* b, const PrMediaCodedFrame& coded) {
    for (;;) {
        double presentation = coded.presentation;
        double decode = coded.decode;
        double duration = coded.duration;
        if (b->mode == PrMediaAppendMode_Sequence && !prF64IsNan(b->groupStart)) {
            b->timestampOffset = b->groupStart - presentation;
            b->groupEnd = b->groupStart;
            for (uint32_t i = 0; i < b->tracks.count; ++i)
                b->tracks.data[i].needRandomAccess = true;
            b->groupStart = kPrF64Nan;
        }
        presentation += b->timestampOffset;
        decode += b->timestampOffset;
        TrackBuffer* track = trackOf(b, coded.track);
        if (track == nullptr)
            return;
        // A frame out of order, or far past the last, starts a new coded frame group.
        if (!prF64IsNan(track->lastDecode) && (decode < track->lastDecode ||
                decode - track->lastDecode > 2 * track->lastDuration)) {
            if (b->mode == PrMediaAppendMode_Segments)
                b->groupEnd = presentation;
            else
                b->groupStart = b->groupEnd;
            for (uint32_t i = 0; i < b->tracks.count; ++i)
                resetTrack(&b->tracks.data[i]);
            continue;
        }
        double end = presentation + duration;
        // A frame outside the append window is dropped.
        if (presentation < b->appendWindow.start || end > b->appendWindow.end) {
            track->needRandomAccess = true;
            return;
        }
        if (track->needRandomAccess) {
            if (!coded.key)
                return;
            track->needRandomAccess = false;
        }
        // The frames that the new frame overlaps are removed, with what depends on them. Of a frame that the new one
        // starts within, a video frame is removed only if the new frame starts within its first microsecond.
        if (prF64IsNan(track->lastDecode)) {
            uint32_t at = lowerDecode(track, presentation - track->maxReorder - track->maxDuration);
            for (; at < track->frames.count; ++at) {
                const BufferFrame& frame = track->frames.data[at];
                if (frame.decode > presentation + track->maxReorder)
                    break;
                if (frame.presentation < presentation && presentation < frame.presentation + frame.duration &&
                        track->kind == PrMediaTrackKind_Video && presentation < frame.presentation + 1e-6) {
                    removePresenting(b, track, frame.presentation, presentation);
                    break;
                }
            }
        }
        double from = prF64IsNan(track->highestEnd) ? presentation : track->highestEnd;
        if (from < end)
            removePresenting(b, track, from > presentation ? from : presentation, end);
        insertFrame(b, track, BufferFrame{presentation, decode, duration, coded.key, frameDataCreate(coded.bytes)});
        track->lastDecode = decode;
        track->lastDuration = duration;
        track->highestEnd = prF64IsNan(track->highestEnd) || end > track->highestEnd ? end : track->highestEnd;
        b->groupEnd = end > b->groupEnd ? end : b->groupEnd;
        return;
    }
}

void codedFramesReceived(void* context, const PrMediaCodedFrame* frames, uint32_t count) {
    MediaSourceBuffer* b = static_cast<MediaSourceBuffer*>(context);
    if (b->failed)
        return;
    // The frames of a segment's tracks are processed in decode order across the tracks, keeping each track's own order,
    // so that a group in sequence mode starts at the segment's earliest frame whichever track the segment lists first.
    b->order.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        b->order.data[i] = i;
    sortItems(b->order.data, count, [frames](uint32_t x, uint32_t y) {
        return frames[x].decode < frames[y].decode || (frames[x].decode == frames[y].decode && x < y);
    });
    prMutexLock(&b->lock);
    for (uint32_t i = 0; i < count; ++i)
        processFrame(b, frames[b->order.data[i]]);
    prMutexUnlock(&b->lock);
}

// Writes the track's ranges, merged across gaps no longer than its longest frame.
void trackRanges(const TrackBuffer* track, RawArray<PrMediaTimeRange>* out) {
    out->clear();
    for (uint32_t i = 0; i < track->frames.count; ++i) {
        const BufferFrame& frame = track->frames.data[i];
        out->push(PrMediaTimeRange{frame.presentation, frame.presentation + frame.duration});
    }
    sortItems(out->data, out->count, [](const PrMediaTimeRange& a, const PrMediaTimeRange& b) {
        return a.start < b.start;
    });
    uint32_t merged = 0;
    for (uint32_t i = 0; i < out->count; ++i) {
        PrMediaTimeRange range = out->data[i];
        if (merged != 0 && range.start <= out->data[merged - 1].end + track->maxDuration) {
            if (range.end > out->data[merged - 1].end)
                out->data[merged - 1].end = range.end;
            continue;
        }
        out->data[merged++] = range;
    }
    out->resize(merged);
}

double trackHighestEnd(const TrackBuffer* track) {
    double end = 0;
    for (uint32_t i = 0; i < track->frames.count; ++i) {
        double frameEnd = track->frames.data[i].presentation + track->frames.data[i].duration;
        end = frameEnd > end ? frameEnd : end;
    }
    return end;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrMediaSourceBuffer : MediaSourceBuffer {};

void prMediaBufferedFrameReset(PrMediaBufferedFrame* frame) {
    if (frame->data != nullptr)
        frameDataRelease(static_cast<const FrameData*>(frame->data));
    *frame = PrMediaBufferedFrame{};
}

PrMediaSourceBuffer* prMediaSourceBufferCreate(PrMediaByteStream format, size_t quota) {
    PrMediaSourceBuffer* b = allocZeroed<PrMediaSourceBuffer>();
    prMutexInit(&b->lock);
    b->parser = prMediaSegmentParserCreate(format);
    b->groupStart = kPrF64Nan;
    b->appendWindow = PrMediaTimeRange{0, kPrF64Infinity};
    b->quota = quota;
    return b;
}

void prMediaSourceBufferDestroy(PrMediaSourceBuffer* PR_NULLABLE buffer) {
    if (buffer == nullptr)
        return;
    for (uint32_t i = 0; i < buffer->tracks.count; ++i) {
        TrackBuffer& track = buffer->tracks.data[i];
        releaseFrames(buffer, &track, 0, track.frames.count);
        track.frames.reset();
    }
    buffer->tracks.reset();
    buffer->copies.reset();
    buffer->text.reset();
    buffer->configs.reset();
    buffer->order.reset();
    prMediaSegmentParserDestroy(buffer->parser);
    prMutexReset(&buffer->lock);
    prMemoryFree(buffer);
}

PrStatus prMediaSourceBufferAppend(PrMediaSourceBuffer* buffer, PrByteView bytes, PrMediaSourceInitFn init,
        void* PR_NULLABLE context) {
    buffer->init = init;
    buffer->initContext = context;
    buffer->failed = false;
    PrMediaSegmentHandlers handlers{
        .initSegment = initSegmentReceived,
        .codedFrames = codedFramesReceived,
        .context = buffer,
    };
    PrStatus status = prMediaSegmentParserAppend(buffer->parser, bytes, &handlers);
    buffer->init = nullptr;
    buffer->initContext = nullptr;
    return buffer->failed ? PrStatus_InvalidData : status;
}

bool prMediaSourceBufferIsParsingMedia(const PrMediaSourceBuffer* buffer) {
    return prMediaSegmentParserIsParsingMedia(buffer->parser);
}

void prMediaSourceBufferResetParser(PrMediaSourceBuffer* buffer) {
    prMediaSegmentParserReset(buffer->parser);
    for (uint32_t i = 0; i < buffer->tracks.count; ++i)
        resetTrack(&buffer->tracks.data[i]);
}

PrMediaAppendMode prMediaSourceBufferMode(const PrMediaSourceBuffer* buffer) {
    return buffer->mode;
}

void prMediaSourceBufferSetMode(PrMediaSourceBuffer* buffer, PrMediaAppendMode mode) {
    // media-source-2/#dom-sourcebuffer-mode: sequence mode starts a coded frame group at the group end.
    if (mode == PrMediaAppendMode_Sequence)
        buffer->groupStart = buffer->groupEnd;
    buffer->mode = mode;
}

double prMediaSourceBufferTimestampOffset(const PrMediaSourceBuffer* buffer) {
    return buffer->timestampOffset;
}

void prMediaSourceBufferSetTimestampOffset(PrMediaSourceBuffer* buffer, double offset) {
    buffer->timestampOffset = offset;
    // media-source-2/#dom-sourcebuffer-timestampoffset: in sequence mode, the next frame starts a group there.
    if (buffer->mode == PrMediaAppendMode_Sequence)
        buffer->groupStart = offset;
}

PrMediaTimeRange prMediaSourceBufferAppendWindow(const PrMediaSourceBuffer* buffer) {
    return buffer->appendWindow;
}

void prMediaSourceBufferSetAppendWindow(PrMediaSourceBuffer* buffer, PrMediaTimeRange window) {
    buffer->appendWindow = window;
}

uint32_t prMediaSourceBufferBuffered(const PrMediaSourceBuffer* buffer, bool ended, PrMediaTimeRange* PR_NULLABLE out,
        uint32_t capacity) {
    if (buffer->tracks.count == 0)
        return 0;
    PrMediaSourceBuffer* b = const_cast<PrMediaSourceBuffer*>(buffer);
    double highest = prMediaSourceBufferHighestEnd(buffer);
    RawArray<PrMediaTimeRange> result{};
    RawArray<PrMediaTimeRange> ranges{};
    RawArray<PrMediaTimeRange> next{};
    // The intersection starts as one range from zero to the highest end, which each track's ranges narrow.
    result.push(PrMediaTimeRange{0, highest});
    for (uint32_t t = 0; t < b->tracks.count; ++t) {
        trackRanges(&b->tracks.data[t], &ranges);
        if (ended && ranges.count != 0)
            ranges.data[ranges.count - 1].end = highest;
        next.clear();
        uint32_t i = 0;
        uint32_t k = 0;
        while (i < result.count && k < ranges.count) {
            double start = result.data[i].start > ranges.data[k].start ? result.data[i].start : ranges.data[k].start;
            double end = result.data[i].end < ranges.data[k].end ? result.data[i].end : ranges.data[k].end;
            if (start < end)
                next.push(PrMediaTimeRange{start, end});
            if (result.data[i].end < ranges.data[k].end)
                ++i;
            else
                ++k;
        }
        RawArray<PrMediaTimeRange> swap = result;
        result = next;
        next = swap;
    }
    uint32_t count = result.count;
    for (uint32_t i = 0; i < count && i < capacity; ++i)
        out[i] = result.data[i];
    result.reset();
    ranges.reset();
    next.reset();
    return count;
}

double prMediaSourceBufferHighestEnd(const PrMediaSourceBuffer* buffer) {
    double end = 0;
    for (uint32_t i = 0; i < buffer->tracks.count; ++i) {
        double trackEnd = trackHighestEnd(&buffer->tracks.data[i]);
        end = trackEnd > end ? trackEnd : end;
    }
    return end;
}

void prMediaSourceBufferRemove(PrMediaSourceBuffer* buffer, double start, double end, double duration) {
    prMutexLock(&buffer->lock);
    for (uint32_t t = 0; t < buffer->tracks.count; ++t) {
        TrackBuffer* track = &buffer->tracks.data[t];
        // The removal runs on to the first random access point at or after its end, so that no frame that remains
        // depends on one removed.
        double removeEnd = end;
        double nextKey = kPrF64Infinity;
        for (uint32_t i = 0; i < track->frames.count; ++i) {
            const BufferFrame& frame = track->frames.data[i];
            if (frame.key && frame.presentation >= end && frame.presentation < nextKey)
                nextKey = frame.presentation;
        }
        if (nextKey != kPrF64Infinity)
            removeEnd = nextKey;
        else if (!prF64IsNan(duration) && duration > removeEnd)
            removeEnd = duration;
        else
            removeEnd = kPrF64Infinity;
        if (removePresenting(buffer, track, start, removeEnd) != UINT32_MAX)
            track->needRandomAccess = true;
    }
    prMutexUnlock(&buffer->lock);
}

bool prMediaSourceBufferEvict(PrMediaSourceBuffer* buffer, double current, size_t incoming) {
    if (buffer->bytes + incoming <= buffer->quota)
        return true;
    // The frames that present before the random access point at or before the current position can go, since playback
    // has passed them.
    for (uint32_t t = 0; t < buffer->tracks.count && buffer->bytes + incoming > buffer->quota; ++t) {
        TrackBuffer* track = &buffer->tracks.data[t];
        double keep = 0;
        for (uint32_t i = 0; i < track->frames.count; ++i) {
            const BufferFrame& frame = track->frames.data[i];
            if (frame.key && frame.presentation <= current && frame.presentation > keep)
                keep = frame.presentation;
        }
        if (keep > 0)
            prMediaSourceBufferRemove(buffer, 0, keep, kPrF64Nan);
    }
    return buffer->bytes + incoming <= buffer->quota;
}

uint32_t prMediaSourceBufferTrackCount(const PrMediaSourceBuffer* buffer) {
    return buffer->copies.count;
}

PrMediaTrackInfo prMediaSourceBufferTrack(const PrMediaSourceBuffer* buffer, uint32_t index) {
    PR_ASSERT(index < buffer->copies.count);
    const TrackCopy& copy = buffer->copies.data[index];
    PrMediaTrackInfo info = copy.info;
    info.codec = PrStringView{buffer->text.data + copy.codecAt, info.codec.size};
    info.language = PrStringView{buffer->text.data + copy.languageAt, info.language.size};
    info.config = PrByteView{buffer->configs.data + copy.configAt, info.config.size};
    return info;
}

bool prMediaSourceBufferNextFrame(PrMediaSourceBuffer* buffer, uint32_t index, double decode, double presentation,
        PrMediaBufferedFrame* out) {
    *out = PrMediaBufferedFrame{};
    prMutexLock(&buffer->lock);
    bool found = false;
    if (index < buffer->tracks.count) {
        const TrackBuffer* track = &buffer->tracks.data[index];
        uint32_t at = UINT32_MAX;
        if (!prF64IsNan(decode)) {
            // The first frame decoded after `decode`.
            uint32_t low = lowerDecode(track, decode);
            while (low < track->frames.count && track->frames.data[low].decode <= decode)
                ++low;
            at = low < track->frames.count ? low : UINT32_MAX;
        } else {
            // The last random access point in decode order that presents at or before the time.
            for (uint32_t i = 0; i < track->frames.count; ++i) {
                const BufferFrame& frame = track->frames.data[i];
                if (frame.key && frame.presentation <= presentation + 1e-6)
                    at = i;
            }
        }
        if (at != UINT32_MAX) {
            const BufferFrame& frame = track->frames.data[at];
            frame.data->references.fetchAdd(1);
            *out = PrMediaBufferedFrame{
                .presentation = frame.presentation,
                .decode = frame.decode,
                .duration = frame.duration,
                .key = frame.key,
                .bytes = PrByteView{bytesOf(frame.data), frame.data->size},
                .data = frame.data,
            };
            found = true;
        }
    }
    prMutexUnlock(&buffer->lock);
    return found;
}
