// Media Source Extensions: MediaSource, SourceBuffer and SourceBufferList, and how a media element plays a media
// source.

#include "html/src/html_media_source.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/fetch_blob.h"
#include "pr/float.h"
#include "pr/memory.h"
#include "pr/string_ops.h"

#include "html/src/html_window.h"

namespace pr_impl {

namespace {

// The bytes of frames that a SourceBuffer keeps, by the kind of its tracks: a few minutes of high definition video, and
// as long of audio.
constexpr size_t kVideoQuota = size_t(150) << 20;
constexpr size_t kAudioQuota = size_t(12) << 20;

// The most SourceBuffers that one media source holds.
constexpr uint32_t kMaxSourceBuffers = 8;

void retainTarget(PrDomEventTarget* target) {
    prDomEventTargetRetain(target);
}

// A task that fires an event at a target, which it holds.
struct TargetEvent {
    PrDomEventTarget* target;
    PrStringView name;
};

void fireTargetEvent(void* data) {
    TargetEvent* event = static_cast<TargetEvent*>(data);
    domEventFire(event->target, event->name);
}

void dropTargetEvent(void* data) {
    TargetEvent* event = static_cast<TargetEvent*>(data);
    prDomEventTargetRelease(event->target);
    prMemoryFree(event);
}

void runTargetEvent(void* data) {
    fireTargetEvent(data);
    dropTargetEvent(data);
}

// Queues a task that fires `name` at `target`.
void queueEvent(PrHtmlWindow* window, PrDomEventTarget* target, PrStringView name) {
    TargetEvent* event = allocZeroed<TargetEvent>();
    retainTarget(target);
    *event = TargetEvent{target, name};
    htmlWindowQueueTask(window, runTargetEvent, event, dropTargetEvent);
}

PrDomEventTarget* targetOf(PrHtmlMediaSource* source) {
    return prHtmlMediaSourceAsEventTarget(source);
}

PrDomEventTarget* targetOf(PrHtmlSourceBuffer* buffer) {
    return prHtmlSourceBufferAsEventTarget(buffer);
}

PrDomEventTarget* targetOf(HtmlSourceBufferList* list) {
    return prHtmlSourceBufferListAsEventTarget(static_cast<PrHtmlSourceBufferList*>(list));
}

void bufferRelease(PrHtmlSourceBuffer* buffer) {
    prDomEventTargetRelease(targetOf(buffer));
}

bool listHas(const HtmlSourceBufferList* list, const PrHtmlSourceBuffer* buffer) {
    for (uint32_t i = 0; i < list->buffers.count; ++i) {
        if (list->buffers.data[i] == buffer)
            return true;
    }
    return false;
}

// Removes the buffer from the list and fires removesourcebuffer at the list, if the list holds it.
void listRemove(PrHtmlWindow* window, HtmlSourceBufferList* list, PrHtmlSourceBuffer* buffer) {
    for (uint32_t i = 0; i < list->buffers.count; ++i) {
        if (list->buffers.data[i] != buffer)
            continue;
        list->buffers.erase(i);
        queueEvent(window, targetOf(list), "removesourcebuffer");
        bufferRelease(buffer);
        return;
    }
}

bool anyUpdating(const PrHtmlMediaSource* source) {
    for (uint32_t i = 0; i < source->sourceBuffers.buffers.count; ++i) {
        if (source->sourceBuffers.buffers.data[i]->update != HtmlSourceUpdate_None)
            return true;
    }
    return false;
}

// Sets an ended source open again, as an append or a change to a buffer does
// (media-source-2/#sourcebuffer-prepare-append ).
void reopen(PrHtmlMediaSource* source) {
    if (source->readyState != PrHtmlMediaSourceReadyState_Ended)
        return;
    source->readyState = PrHtmlMediaSourceReadyState_Open;
    queueEvent(source->window, targetOf(source), "sourceopen");
}

// The highest end time of the source's buffers.
double highestEnd(const PrHtmlMediaSource* source) {
    double end = 0;
    for (uint32_t i = 0; i < source->sourceBuffers.buffers.count; ++i) {
        const PrHtmlSourceBuffer* buffer = source->sourceBuffers.buffers.data[i];
        if (buffer->buffer == nullptr)
            continue;
        double bufferEnd = prMediaSourceBufferHighestEnd(buffer->buffer);
        end = bufferEnd > end ? bufferEnd : end;
    }
    return end;
}

// media-source-2/#duration-change-algorithm, for a duration that the caller checked.
void changeDuration(PrHtmlMediaSource* source, double duration) {
    if (source->duration == duration)
        return;
    double end = highestEnd(source);
    duration = duration < end ? end : duration;
    source->duration = duration;
    if (source->media != nullptr)
        htmlMediaSourceDuration(source->media, duration);
}

// Tells the element that what the source holds has changed.
void sourceChanged(PrHtmlMediaSource* source) {
    if (source->media != nullptr)
        htmlMediaSourceChanged(source->media);
}

// media-source-2/#end-of-stream-algorithm
void endStream(PrHtmlMediaSource* source, PrHtmlEndOfStreamError error) {
    source->readyState = PrHtmlMediaSourceReadyState_Ended;
    queueEvent(source->window, targetOf(source), "sourceended");
    if (error == PrHtmlEndOfStreamError_None) {
        changeDuration(source, highestEnd(source));
        sourceChanged(source);
        return;
    }
    if (source->media != nullptr) {
        PrHtmlMediaError code = PrHtmlMediaError_Decode;
        if (error == PrHtmlEndOfStreamError_Network)
            code = PrHtmlMediaError_Network;
        htmlMediaSourceFailed(source->media, code);
    }
}

// Ends an update: the buffer is no longer updating, and fires `event` and then updateend.
void finishUpdate(PrHtmlSourceBuffer* buffer, PrStringView event) {
    buffer->update = HtmlSourceUpdate_None;
    ++buffer->updates;
    buffer->input.clear();
    queueEvent(buffer->window, targetOf(buffer), event);
    queueEvent(buffer->window, targetOf(buffer), "updateend");
}

// Activates the buffer, as the first initialization segment that gives it audio or video does.
void activate(PrHtmlSourceBuffer* buffer) {
    PrHtmlMediaSource* source = buffer->source;
    if (source == nullptr || listHas(&source->activeSourceBuffers, buffer))
        return;
    // The element's decoder reads the active buffers, and is made again with the new one.
    if (source->media != nullptr)
        htmlMediaSourceTracksChanging(source->media);
    retainTarget(targetOf(buffer));
    // The active buffers keep the order that they have among the source's buffers.
    uint32_t at = 0;
    for (uint32_t i = 0; i < source->sourceBuffers.buffers.count; ++i) {
        PrHtmlSourceBuffer* other = source->sourceBuffers.buffers.data[i];
        if (other == buffer)
            break;
        if (listHas(&source->activeSourceBuffers, other))
            ++at;
    }
    source->activeSourceBuffers.buffers.insert(at, buffer);
    queueEvent(source->window, targetOf(&source->activeSourceBuffers), "addsourcebuffer");
}

// Returns true if the window's codec reads the codec of each audio and video track of the segment.
bool codecsSupported(const PrHtmlWindow* window, const PrMediaInitSegment* segment) {
    PrMediaCodec* codec = window->mediaCodec;
    if (codec == nullptr)
        return false;
    for (uint32_t i = 0; i < segment->trackCount; ++i) {
        const PrMediaTrackInfo& track = segment->tracks[i];
        if (track.kind == PrMediaTrackKind_Other)
            continue;
        PrStringView prefix = track.kind == PrMediaTrackKind_Audio ? "audio/mp4; codecs=\"" : "video/mp4; codecs=\"";
        PrRawString type = prRawStringCopy(prefix);
        prRawStringAppend(&type, track.codec);
        prRawStringAppend(&type, "\"");
        bool supported = prMediaCodecSupportsSourceType(codec, prRawStringView(&type)) != PrMediaSupport_No;
        prRawStringReset(&type);
        if (!supported)
            return false;
    }
    return true;
}

// media-source-2/#sourcebuffer-init-segment-received, after the checks that the source buffer makes.
bool initReceived(void* context, const PrMediaInitSegment* segment, bool first) {
    PrHtmlSourceBuffer* buffer = static_cast<PrHtmlSourceBuffer*>(context);
    PrHtmlMediaSource* source = buffer->source;
    if (source == nullptr)
        return true;
    if (!codecsSupported(buffer->window, segment))
        return false;
    if (prF64IsNan(source->duration))
        changeDuration(source, prF64IsNan(segment->duration) ? kPrF64Infinity : segment->duration);
    for (uint32_t i = 0; i < segment->trackCount; ++i) {
        const PrMediaTrackInfo& track = segment->tracks[i];
        if (track.kind == PrMediaTrackKind_Audio)
            buffer->hasAudio = true;
        if (track.kind == PrMediaTrackKind_Video && !buffer->hasVideo) {
            buffer->hasVideo = true;
            buffer->videoExtent = track.extent;
        }
    }
    if (!first)
        return true;
    buffer->firstInit = true;
    if (buffer->hasAudio || buffer->hasVideo)
        activate(buffer);
    // The element has metadata once every buffer has received its first initialization segment.
    for (uint32_t i = 0; i < source->sourceBuffers.buffers.count; ++i) {
        if (!source->sourceBuffers.buffers.data[i]->firstInit)
            return true;
    }
    if (source->media != nullptr) {
        PrUVec2 extent{};
        for (uint32_t i = 0; i < source->activeSourceBuffers.buffers.count && extent.x == 0; ++i)
            extent = source->activeSourceBuffers.buffers.data[i]->videoExtent;
        htmlMediaSourceMetadata(source->media, extent);
    }
    return true;
}

// A task of an update, for the buffer and the update that it belongs to.
struct UpdateTask {
    PrHtmlSourceBuffer* buffer;
    uint32_t update;
};

void dropUpdate(void* data) {
    UpdateTask* task = static_cast<UpdateTask*>(data);
    bufferRelease(task->buffer);
    prMemoryFree(task);
}

// media-source-2/#sourcebuffer-append-error
void appendError(PrHtmlSourceBuffer* buffer) {
    prMediaSourceBufferResetParser(buffer->buffer);
    finishUpdate(buffer, "error");
    if (buffer->source != nullptr)
        endStream(buffer->source, PrHtmlEndOfStreamError_Decode);
}

// media-source-2/#sourcebuffer-buffer-append
void runAppend(void* data) {
    UpdateTask* task = static_cast<UpdateTask*>(data);
    PrHtmlSourceBuffer* buffer = task->buffer;
    if (buffer->updates == task->update && buffer->update == HtmlSourceUpdate_Append && buffer->buffer != nullptr) {
        PrByteView bytes{buffer->input.data, buffer->input.count};
        PrStatus status = prMediaSourceBufferAppend(buffer->buffer, bytes, initReceived, buffer);
        if (status != PrStatus_Ok) {
            appendError(buffer);
        } else {
            PrHtmlMediaSource* source = buffer->source;
            // A frame past the duration extends it (media-source-2/#sourcebuffer-coded-frame-processing).
            if (source != nullptr && !prF64IsNan(source->duration) && highestEnd(source) > source->duration)
                changeDuration(source, highestEnd(source));
            finishUpdate(buffer, "update");
            if (source != nullptr)
                sourceChanged(source);
        }
    }
    dropUpdate(task);
}

// media-source-2/#sourcebuffer-range-removal, after its checks.
void runRemove(void* data) {
    UpdateTask* task = static_cast<UpdateTask*>(data);
    PrHtmlSourceBuffer* buffer = task->buffer;
    if (buffer->updates == task->update && buffer->update == HtmlSourceUpdate_Remove && buffer->buffer != nullptr) {
        PrHtmlMediaSource* source = buffer->source;
        double duration = source != nullptr ? source->duration : kPrF64Nan;
        prMediaSourceBufferRemove(buffer->buffer, buffer->removeStart, buffer->removeEnd, duration);
        finishUpdate(buffer, "update");
        if (source != nullptr)
            sourceChanged(source);
    }
    dropUpdate(task);
}

// Starts an update: the buffer is updating, fires updatestart, and runs `run` in a task.
void startUpdate(PrHtmlSourceBuffer* buffer, HtmlSourceUpdate update, void (*run)(void* data)) {
    buffer->update = update;
    queueEvent(buffer->window, targetOf(buffer), "updatestart");
    retainTarget(targetOf(buffer));
    UpdateTask* task = allocZeroed<UpdateTask>();
    *task = UpdateTask{buffer, buffer->updates};
    htmlWindowQueueTask(buffer->window, run, task, dropUpdate);
}

// Aborts the update in progress, which fires abort and updateend.
void abortUpdate(PrHtmlSourceBuffer* buffer) {
    if (buffer->update == HtmlSourceUpdate_None)
        return;
    finishUpdate(buffer, "abort");
}

// The checks that every setter of a SourceBuffer makes first: the buffer is in its source, and not updating.
PrStatus checkChangeable(const PrHtmlSourceBuffer* buffer) {
    if (buffer->source == nullptr || buffer->update != HtmlSourceUpdate_None)
        return PrStatus_InvalidState;
    return PrStatus_Ok;
}

// Removes the buffer from its source (media-source-2/#dom-mediasource-removesourcebuffer), after its checks.
void removeBuffer(PrHtmlMediaSource* source, PrHtmlSourceBuffer* buffer) {
    abortUpdate(buffer);
    if (source->media != nullptr)
        htmlMediaSourceTracksChanging(source->media);
    listRemove(source->window, &source->activeSourceBuffers, buffer);
    buffer->source = nullptr;
    // The buffer's frames go now. The object lives on for script, and every call on it throws.
    prMediaSourceBufferDestroy(buffer->buffer);
    buffer->buffer = nullptr;
    listRemove(source->window, &source->sourceBuffers, buffer);
}

void destroyBuffer(DomCountedTarget* target) {
    PrHtmlSourceBuffer* buffer = static_cast<PrHtmlSourceBuffer*>(target);
    prMediaSourceBufferDestroy(buffer->buffer);
    buffer->input.reset();
    domListenersDestroy(buffer->listeners);
    prDomEventTargetRelease(prHtmlWindowAsEventTarget(buffer->window));
    prMemoryFree(buffer);
}

void destroySource(DomCountedTarget* target) {
    PrHtmlMediaSource* source = static_cast<PrHtmlMediaSource*>(target);
    PR_ASSERT(source->media == nullptr);
    for (uint32_t i = 0; i < source->activeSourceBuffers.buffers.count; ++i)
        bufferRelease(source->activeSourceBuffers.buffers.data[i]);
    for (uint32_t i = 0; i < source->sourceBuffers.buffers.count; ++i) {
        PrHtmlSourceBuffer* buffer = source->sourceBuffers.buffers.data[i];
        buffer->source = nullptr;
        bufferRelease(buffer);
    }
    source->activeSourceBuffers.buffers.reset();
    source->sourceBuffers.buffers.reset();
    domListenersDestroy(source->sourceBuffers.listeners);
    domListenersDestroy(source->activeSourceBuffers.listeners);
    domListenersDestroy(source->listeners);
    prDomEventTargetRelease(prHtmlWindowAsEventTarget(source->window));
    prMemoryFree(source);
}

// Returns the codec that reads a type, and the byte stream format that the type names, if the window's codec reads the
// type.
bool supported(const PrHtmlWindow* window, PrStringView type, PrMediaByteStream* format) {
    *format = prMediaByteStreamFromType(type);
    PrMediaCodec* codec = window->mediaCodec;
    return *format != PrMediaByteStream_None && codec != nullptr &&
        prMediaCodecSupportsSourceType(codec, type) != PrMediaSupport_No;
}

} // namespace

PrHtmlMediaSource* PR_NULLABLE htmlMediaSourceFromUrl(PrHtmlWindow* window, PrStringView url) {
    for (uint32_t i = 0; i < window->mediaSources.count; ++i) {
        const HtmlMediaSourceUrl& entry = window->mediaSources.data[i];
        if (prRawStringView(&entry.url) == url)
            return entry.source;
    }
    return nullptr;
}

void htmlMediaSourceUrlsReset(PrHtmlWindow* window) {
    for (uint32_t i = 0; i < window->mediaSources.count; ++i) {
        prRawStringReset(&window->mediaSources.data[i].url);
        prDomEventTargetRelease(targetOf(window->mediaSources.data[i].source));
    }
    window->mediaSources.reset();
}

void htmlMediaSourceRevokeUrl(PrHtmlWindow* window, PrStringView url) {
    for (uint32_t i = 0; i < window->mediaSources.count; ++i) {
        HtmlMediaSourceUrl& entry = window->mediaSources.data[i];
        if (prRawStringView(&entry.url) != url)
            continue;
        prRawStringReset(&entry.url);
        prDomEventTargetRelease(targetOf(entry.source));
        window->mediaSources.erase(i);
        return;
    }
}

bool htmlMediaSourceAttach(PrHtmlMediaSource* source, HtmlMedia* media) {
    if (source->readyState != PrHtmlMediaSourceReadyState_Closed || source->media != nullptr)
        return false;
    retainTarget(targetOf(source));
    source->media = media;
    source->readyState = PrHtmlMediaSourceReadyState_Open;
    queueEvent(source->window, targetOf(source), "sourceopen");
    return true;
}

void htmlMediaSourceDetach(PrHtmlMediaSource* source) {
    source->readyState = PrHtmlMediaSourceReadyState_Closed;
    source->duration = kPrF64Nan;
    source->media = nullptr;
    while (source->activeSourceBuffers.buffers.count != 0)
        listRemove(source->window, &source->activeSourceBuffers, source->activeSourceBuffers.buffers.last());
    while (source->sourceBuffers.buffers.count != 0) {
        PrHtmlSourceBuffer* buffer = source->sourceBuffers.buffers.last();
        abortUpdate(buffer);
        buffer->source = nullptr;
        prMediaSourceBufferDestroy(buffer->buffer);
        buffer->buffer = nullptr;
        listRemove(source->window, &source->sourceBuffers, buffer);
    }
    queueEvent(source->window, targetOf(source), "sourceclose");
    prDomEventTargetRelease(targetOf(source));
}

void htmlMediaSourceBuffered(const PrHtmlMediaSource* source, RawArray<PrMediaTimeRange>* out) {
    out->clear();
    const HtmlSourceBufferList& active = source->activeSourceBuffers;
    if (active.buffers.count == 0)
        return;
    bool ended = source->readyState == PrHtmlMediaSourceReadyState_Ended;
    // The intersection of every active buffer's ranges (media-source-2/#htmlmediaelement-extensions-buffered).
    RawArray<PrMediaTimeRange> ranges{};
    RawArray<PrMediaTimeRange> next{};
    for (uint32_t b = 0; b < active.buffers.count; ++b) {
        const PrHtmlSourceBuffer* buffer = active.buffers.data[b];
        if (buffer->buffer == nullptr)
            continue;
        uint32_t count = prMediaSourceBufferBuffered(buffer->buffer, ended, nullptr, 0);
        ranges.resize(count);
        prMediaSourceBufferBuffered(buffer->buffer, ended, ranges.data, count);
        if (b == 0) {
            out->clear();
            if (count != 0)
                out->append(ranges.data, count);
            continue;
        }
        next.clear();
        uint32_t i = 0;
        uint32_t k = 0;
        while (i < out->count && k < ranges.count) {
            double start = out->data[i].start > ranges.data[k].start ? out->data[i].start : ranges.data[k].start;
            double end = out->data[i].end < ranges.data[k].end ? out->data[i].end : ranges.data[k].end;
            if (start < end)
                next.push(PrMediaTimeRange{start, end});
            if (out->data[i].end < ranges.data[k].end)
                ++i;
            else
                ++k;
        }
        out->clear();
        if (next.count != 0)
            out->append(next.span());
    }
    ranges.reset();
    next.reset();
}

void htmlMediaSourceSeekable(const PrHtmlMediaSource* source, RawArray<PrMediaTimeRange>* out) {
    out->clear();
    if (prF64IsNan(source->duration))
        return;
    if (source->duration != kPrF64Infinity) {
        out->push(PrMediaTimeRange{0, source->duration});
        return;
    }
    // A live source can seek within its live seekable range and what it has buffered.
    RawArray<PrMediaTimeRange> buffered{};
    htmlMediaSourceBuffered(source, &buffered);
    if (!prF64IsNan(source->liveStart)) {
        double start = source->liveStart;
        double end = source->liveEnd;
        if (buffered.count != 0) {
            start = buffered.data[0].start < start ? buffered.data[0].start : start;
            end = buffered.last().end > end ? buffered.last().end : end;
        }
        out->push(PrMediaTimeRange{start, end});
    } else if (buffered.count != 0) {
        out->push(PrMediaTimeRange{0, buffered.last().end});
    }
    buffered.reset();
}

bool htmlMediaSourceHasEnded(const PrHtmlMediaSource* source) {
    return source->readyState == PrHtmlMediaSourceReadyState_Ended;
}

uint32_t htmlMediaSourceActiveBuffers(const PrHtmlMediaSource* source, PrMediaSourceBuffer* PR_NONNULL* PR_NONNULL out,
        uint32_t capacity) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < source->activeSourceBuffers.buffers.count; ++i) {
        PrMediaSourceBuffer* buffer = source->activeSourceBuffers.buffers.data[i]->buffer;
        if (buffer == nullptr)
            continue;
        if (count < capacity)
            out[count] = buffer;
        ++count;
    }
    return count;
}

} // namespace pr_impl

using namespace pr_impl;

PrHtmlMediaSource* prHtmlMediaSourceCreate(PrHtmlWindow* window) {
    PrHtmlMediaSource* source = allocZeroed<PrHtmlMediaSource>();
    source->type = DomTargetType_MediaSource;
    source->destroy = destroySource;
    source->references = 1;
    source->window = window;
    source->duration = kPrF64Nan;
    source->liveStart = kPrF64Nan;
    source->liveEnd = kPrF64Nan;
    source->sourceBuffers.type = DomTargetType_SourceBufferList;
    source->sourceBuffers.owner = targetOf(source);
    source->activeSourceBuffers.type = DomTargetType_SourceBufferList;
    source->activeSourceBuffers.owner = targetOf(source);
    prDomEventTargetRetain(prHtmlWindowAsEventTarget(window));
    return source;
}

PrHtmlMediaSource* PR_NULLABLE prHtmlMediaSourceQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_MediaSource ? static_cast<PrHtmlMediaSource*>(object) : nullptr;
}

PrHtmlSourceBuffer* PR_NULLABLE prHtmlSourceBufferQuery(PrDomEventTarget* PR_NULLABLE object) {
    return object != nullptr && object->type == DomTargetType_SourceBuffer ? static_cast<PrHtmlSourceBuffer*>(object) : nullptr;
}

PrHtmlSourceBufferList* PR_NULLABLE prHtmlSourceBufferListQuery(PrDomEventTarget* PR_NULLABLE object) {
    if (object == nullptr || object->type != DomTargetType_SourceBufferList)
        return nullptr;
    DomPartTarget* part = static_cast<DomPartTarget*>(object);
    return static_cast<PrHtmlSourceBufferList*>(static_cast<HtmlSourceBufferList*>(part));
}

bool prHtmlMediaSourceIsTypeSupported(const PrHtmlWindow* window, PrStringView type) {
    PrMediaByteStream format = PrMediaByteStream_None;
    return supported(window, type, &format);
}

PrRawString prHtmlMediaSourceUrlCreate(PrHtmlWindow* window, PrHtmlMediaSource* source) {
    PrRawString origin = htmlWindowOrigin(window);
    PrRawString url = prFetchBlobUrlGenerate(prRawStringView(&origin));
    prRawStringReset(&origin);
    retainTarget(targetOf(source));
    window->mediaSources.push(HtmlMediaSourceUrl{prRawStringClone(&url), source});
    return url;
}

PrHtmlMediaSourceReadyState prHtmlMediaSourceReadyState(const PrHtmlMediaSource* source) {
    return source->readyState;
}

double prHtmlMediaSourceDuration(const PrHtmlMediaSource* source) {
    return source->readyState == PrHtmlMediaSourceReadyState_Closed ? kPrF64Nan : source->duration;
}

PrStatus prHtmlMediaSourceSetDuration(PrHtmlMediaSource* source, double duration) {
    if (prF64IsNan(duration) || duration < 0)
        return PrStatus_OutOfRange;
    if (source->readyState != PrHtmlMediaSourceReadyState_Open || anyUpdating(source))
        return PrStatus_InvalidState;
    // The duration cannot fall below a buffered frame's presentation (media-source-2/#duration-change-algorithm).
    for (uint32_t i = 0; i < source->sourceBuffers.buffers.count; ++i) {
        const PrHtmlSourceBuffer* buffer = source->sourceBuffers.buffers.data[i];
        if (buffer->buffer == nullptr)
            continue;
        PrMediaTimeRange ranges[1];
        uint32_t count = prMediaSourceBufferBuffered(buffer->buffer, false, nullptr, 0);
        if (count != 0) {
            RawArray<PrMediaTimeRange> all{};
            all.resize(count);
            prMediaSourceBufferBuffered(buffer->buffer, false, all.data, count);
            ranges[0] = all.last();
            all.reset();
            if (duration < ranges[0].start)
                return PrStatus_InvalidState;
        }
    }
    changeDuration(source, duration);
    sourceChanged(source);
    return PrStatus_Ok;
}

PrHtmlSourceBufferList* prHtmlMediaSourceSourceBuffers(PrHtmlMediaSource* source) {
    return static_cast<PrHtmlSourceBufferList*>(&source->sourceBuffers);
}

PrHtmlSourceBufferList* prHtmlMediaSourceActiveSourceBuffers(PrHtmlMediaSource* source) {
    return static_cast<PrHtmlSourceBufferList*>(&source->activeSourceBuffers);
}

PrStatus prHtmlMediaSourceAddSourceBuffer(PrHtmlMediaSource* source, PrStringView type,
        PrHtmlSourceBuffer* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (type.size == 0)
        return PrStatus_OutOfRange;
    PrMediaByteStream format = PrMediaByteStream_None;
    if (!supported(source->window, type, &format))
        return PrStatus_NotSupported;
    if (source->sourceBuffers.buffers.count >= kMaxSourceBuffers)
        return PrStatus_Limit;
    if (source->readyState != PrHtmlMediaSourceReadyState_Open)
        return PrStatus_InvalidState;
    PrHtmlSourceBuffer* buffer = allocZeroed<PrHtmlSourceBuffer>();
    buffer->type = DomTargetType_SourceBuffer;
    buffer->destroy = destroyBuffer;
    buffer->references = 1;
    buffer->window = source->window;
    buffer->source = source;
    bool audioOnly = prAsciiStartsWithFold(type, "audio/");
    buffer->buffer = prMediaSourceBufferCreate(format, audioOnly ? kAudioQuota : kVideoQuota);
    prDomEventTargetRetain(prHtmlWindowAsEventTarget(source->window));
    retainTarget(targetOf(buffer));
    source->sourceBuffers.buffers.push(buffer);
    queueEvent(source->window, targetOf(&source->sourceBuffers), "addsourcebuffer");
    *out = buffer;
    return PrStatus_Ok;
}

PrStatus prHtmlMediaSourceRemoveSourceBuffer(PrHtmlMediaSource* source, PrHtmlSourceBuffer* buffer) {
    if (!listHas(&source->sourceBuffers, buffer))
        return PrStatus_NotFound;
    removeBuffer(source, buffer);
    sourceChanged(source);
    return PrStatus_Ok;
}

PrStatus prHtmlMediaSourceEndOfStream(PrHtmlMediaSource* source, PrHtmlEndOfStreamError error) {
    if (source->readyState != PrHtmlMediaSourceReadyState_Open || anyUpdating(source))
        return PrStatus_InvalidState;
    endStream(source, error);
    return PrStatus_Ok;
}

PrStatus prHtmlMediaSourceSetLiveSeekableRange(PrHtmlMediaSource* source, double start, double end) {
    if (source->readyState != PrHtmlMediaSourceReadyState_Open)
        return PrStatus_InvalidState;
    if (start < 0 || start > end || prF64IsNan(start) || prF64IsNan(end))
        return PrStatus_OutOfRange;
    source->liveStart = start;
    source->liveEnd = end;
    return PrStatus_Ok;
}

PrStatus prHtmlMediaSourceClearLiveSeekableRange(PrHtmlMediaSource* source) {
    if (source->readyState != PrHtmlMediaSourceReadyState_Open)
        return PrStatus_InvalidState;
    source->liveStart = kPrF64Nan;
    source->liveEnd = kPrF64Nan;
    return PrStatus_Ok;
}

uint32_t prHtmlSourceBufferListLength(const PrHtmlSourceBufferList* list) {
    return list->buffers.count;
}

PrHtmlSourceBuffer* prHtmlSourceBufferListItem(PrHtmlSourceBufferList* list, uint32_t index) {
    PR_ASSERT(index < list->buffers.count);
    return list->buffers.data[index];
}

PrMediaAppendMode prHtmlSourceBufferMode(const PrHtmlSourceBuffer* buffer) {
    return buffer->buffer != nullptr ? prMediaSourceBufferMode(buffer->buffer) : PrMediaAppendMode_Segments;
}

PrStatus prHtmlSourceBufferSetMode(PrHtmlSourceBuffer* buffer, PrMediaAppendMode mode) {
    PrStatus status = checkChangeable(buffer);
    if (status != PrStatus_Ok)
        return status;
    reopen(buffer->source);
    if (prMediaSourceBufferIsParsingMedia(buffer->buffer))
        return PrStatus_InvalidState;
    prMediaSourceBufferSetMode(buffer->buffer, mode);
    return PrStatus_Ok;
}

bool prHtmlSourceBufferIsUpdating(const PrHtmlSourceBuffer* buffer) {
    return buffer->update != HtmlSourceUpdate_None;
}

PrStatus prHtmlSourceBufferBuffered(const PrHtmlSourceBuffer* buffer, PrMediaTimeRange* PR_NULLABLE out,
        uint32_t capacity, uint32_t* count) {
    *count = 0;
    if (buffer->source == nullptr || buffer->buffer == nullptr)
        return PrStatus_InvalidState;
    bool ended = buffer->source->readyState == PrHtmlMediaSourceReadyState_Ended;
    *count = prMediaSourceBufferBuffered(buffer->buffer, ended, out, capacity);
    return PrStatus_Ok;
}

double prHtmlSourceBufferTimestampOffset(const PrHtmlSourceBuffer* buffer) {
    return buffer->buffer != nullptr ? prMediaSourceBufferTimestampOffset(buffer->buffer) : 0;
}

PrStatus prHtmlSourceBufferSetTimestampOffset(PrHtmlSourceBuffer* buffer, double offset) {
    PrStatus status = checkChangeable(buffer);
    if (status != PrStatus_Ok)
        return status;
    reopen(buffer->source);
    if (prMediaSourceBufferIsParsingMedia(buffer->buffer))
        return PrStatus_InvalidState;
    prMediaSourceBufferSetTimestampOffset(buffer->buffer, offset);
    return PrStatus_Ok;
}

double prHtmlSourceBufferAppendWindowStart(const PrHtmlSourceBuffer* buffer) {
    return buffer->buffer != nullptr ? prMediaSourceBufferAppendWindow(buffer->buffer).start : 0;
}

PrStatus prHtmlSourceBufferSetAppendWindowStart(PrHtmlSourceBuffer* buffer, double start) {
    PrStatus status = checkChangeable(buffer);
    if (status != PrStatus_Ok)
        return status;
    PrMediaTimeRange window = prMediaSourceBufferAppendWindow(buffer->buffer);
    if (prF64IsNan(start) || start < 0 || start >= window.end)
        return PrStatus_OutOfRange;
    window.start = start;
    prMediaSourceBufferSetAppendWindow(buffer->buffer, window);
    return PrStatus_Ok;
}

double prHtmlSourceBufferAppendWindowEnd(const PrHtmlSourceBuffer* buffer) {
    return buffer->buffer != nullptr ? prMediaSourceBufferAppendWindow(buffer->buffer).end : kPrF64Infinity;
}

PrStatus prHtmlSourceBufferSetAppendWindowEnd(PrHtmlSourceBuffer* buffer, double end) {
    PrStatus status = checkChangeable(buffer);
    if (status != PrStatus_Ok)
        return status;
    PrMediaTimeRange window = prMediaSourceBufferAppendWindow(buffer->buffer);
    if (prF64IsNan(end) || end <= window.start)
        return PrStatus_OutOfRange;
    window.end = end;
    prMediaSourceBufferSetAppendWindow(buffer->buffer, window);
    return PrStatus_Ok;
}

PrStatus prHtmlSourceBufferAppendBuffer(PrHtmlSourceBuffer* buffer, PrByteView bytes) {
    // media-source-2/#sourcebuffer-prepare-append
    PrStatus status = checkChangeable(buffer);
    if (status != PrStatus_Ok)
        return status;
    PrHtmlMediaSource* source = buffer->source;
    if (source->media != nullptr && htmlMediaSourceHasError(source->media))
        return PrStatus_InvalidState;
    reopen(source);
    double current = source->media != nullptr ? htmlMediaCurrentTime(source->media) : 0;
    if (!prMediaSourceBufferEvict(buffer->buffer, current, bytes.size))
        return PrStatus_Limit;
    buffer->input.clear();
    if (bytes.size != 0)
        buffer->input.append(bytes.data, uint32_t(bytes.size));
    startUpdate(buffer, HtmlSourceUpdate_Append, runAppend);
    return PrStatus_Ok;
}

PrStatus prHtmlSourceBufferAbort(PrHtmlSourceBuffer* buffer) {
    if (buffer->source == nullptr || buffer->source->readyState != PrHtmlMediaSourceReadyState_Open ||
            buffer->update == HtmlSourceUpdate_Remove)
        return PrStatus_InvalidState;
    abortUpdate(buffer);
    prMediaSourceBufferResetParser(buffer->buffer);
    prMediaSourceBufferSetAppendWindow(buffer->buffer, PrMediaTimeRange{0, kPrF64Infinity});
    return PrStatus_Ok;
}

PrStatus prHtmlSourceBufferRemove(PrHtmlSourceBuffer* buffer, double start, double end) {
    PrStatus status = checkChangeable(buffer);
    if (status != PrStatus_Ok)
        return status;
    PrHtmlMediaSource* source = buffer->source;
    double duration = source->duration;
    if (prF64IsNan(duration) || prF64IsNan(start) || prF64IsNan(end) || start < 0 || start > duration || end <= start)
        return PrStatus_OutOfRange;
    reopen(source);
    buffer->removeStart = start;
    buffer->removeEnd = end;
    startUpdate(buffer, HtmlSourceUpdate_Remove, runRemove);
    return PrStatus_Ok;
}

PrStatus prHtmlSourceBufferChangeType(PrHtmlSourceBuffer* buffer, PrStringView type) {
    if (type.size == 0)
        return PrStatus_OutOfRange;
    PrStatus status = checkChangeable(buffer);
    if (status != PrStatus_Ok)
        return status;
    PrMediaByteStream format = PrMediaByteStream_None;
    if (!supported(buffer->window, type, &format))
        return PrStatus_NotSupported;
    reopen(buffer->source);
    prMediaSourceBufferResetParser(buffer->buffer);
    return PrStatus_Ok;
}
