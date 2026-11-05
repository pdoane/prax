#include "html/src/html_media.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/string_ops.h"
#include "pr/time.h"
#include "pr/url.h"

#include "css/src/css_presentation_hooks.h"
#include "dom/src/dom_element_state.h"
#include "html/src/html_media_source.h"

namespace pr_impl {

namespace {

// The maximum time between two timeupdate events while a media element plays (html/#event-media-timeupdate).
constexpr double kTimeUpdateMilliseconds = 250;

// ---------------------------------------------------------------------------------------------------------------------
// Tasks (html/#queue-a-media-element-task)

// A media element task. It holds its action, the load and seek that it belongs to, so that a later task can drop it,
// and the promises that it settles.
struct MediaTask {
    HtmlMedia* media;
    uint32_t load;
    void (*run)(MediaTask* task);
    PrStringView event;                   // an event to fire, or the null view
    PrStringView after;                   // an event to fire after `event`, or the null view
    RawArray<PrHtmlPlayPromise> promises;

    // The DOMException that the promises are rejected with, or the null view to resolve them.
    PrStringView rejection;

    uint32_t seekCount;                   // for a seek's completion, the seek that it completes
    PrDomElement* PR_NULLABLE target;     // the event's target if it is not the media element; the task holds it
};

// Releases the promises without settling them.
void releasePromises(RawArray<PrHtmlPlayPromise>* promises) {
    for (uint32_t i = 0; i < promises->count; ++i)
        promises->data[i].release(promises->data[i].context);
    promises->reset();
}

void settle(RawArray<PrHtmlPlayPromise>* promises, PrStringView rejection) {
    RawArray<PrHtmlPlayPromise> taken = *promises;
    *promises = {};
    for (uint32_t i = 0; i < taken.count; ++i) {
        taken.data[i].settle(taken.data[i].context, rejection);
        taken.data[i].release(taken.data[i].context);
    }
    taken.reset();
}

// Releases the task and the element that it held, which may free the element and its state.
void dropTask(void* data) {
    MediaTask* task = static_cast<MediaTask*>(data);
    releasePromises(&task->promises);
    PrDomElement* element = task->media->element;
    PrDomElement* target = task->target;
    prMemoryFree(task);
    if (target != nullptr)
        domNodeUnhold(target);
    domNodeUnhold(element);
}

void runTask(void* data) {
    MediaTask* task = static_cast<MediaTask*>(data);
    if (task->load == task->media->load) {
        PrDomElement* target = task->target != nullptr ? task->target : task->media->element;
        if (task->event.data != nullptr)
            domEventFire(target, task->event);
        if (task->run != nullptr)
            task->run(task);
        if (task->after.data != nullptr)
            domEventFire(target, task->after);
        settle(&task->promises, task->rejection);
    }
    dropTask(task);
}

MediaTask* newTask(HtmlMedia* media) {
    domNodeHold(media->element);
    MediaTask* task = allocZeroed<MediaTask>();
    task->media = media;
    task->load = media->load;
    return task;
}

// Queues the task on the window that runs the element, or drops it if no window does.
void queueTask(MediaTask* task) {
    if (PrHtmlWindow* window = task->media->window)
        htmlWindowQueueTask(window, runTask, task, dropTask);
    else
        dropTask(task);
}

void queueEvent(HtmlMedia* media, PrStringView event) {
    MediaTask* task = newTask(media);
    task->event = event;
    queueTask(task);
}

// Queues a task that settles the currently pending promises, firing `event` first if it is not null.
void queueSettle(HtmlMedia* media, PrStringView event, PrStringView rejection) {
    MediaTask* task = newTask(media);
    task->event = event;
    task->promises = media->promises;
    media->promises = {};
    task->rejection = rejection;
    queueTask(task);
}

// ---------------------------------------------------------------------------------------------------------------------
// Playback

// Returns true if the element is potentially playing (html/#potentially-playing). A seek pauses the timeline of a
// potentially playing element.
bool potentiallyPlaying(const HtmlMedia* media) {
    return !media->paused && media->error == PrHtmlMediaError_None &&
        media->readyState >= PrHtmlMediaReadyState_HaveFutureData && !media->seeking &&
        !(media->position >= media->duration && media->rate > 0 && !prF64IsNan(media->duration));
}

// Adds [start, end] to the played ranges (html/#dom-media-played), merging it with each range that it touches.
void addPlayed(HtmlMedia* media, double start, double end) {
    RawArray<double>& ranges = media->played;
    uint32_t at = 0;
    while (at < ranges.count && ranges.data[at + 1] < start)
        at += 2;
    uint32_t last = at;
    while (last < ranges.count && ranges.data[last] <= end) {
        start = ranges.data[last] < start ? ranges.data[last] : start;
        end = ranges.data[last + 1] > end ? ranges.data[last + 1] : end;
        last += 2;
    }
    ranges.erase(at, last - at);
    ranges.insert(at, start);
    ranges.insert(at + 1, end);
}

// The time on the clock that the element's timeline runs on, in milliseconds: its window's clock, which the window's
// timers also run on, or the system's monotonic clock while no window runs the element.
double timelineNow(const HtmlMedia* media) {
    return media->window != nullptr ? htmlWindowClock(media->window) : prTimeMonotonicMilliseconds();
}

// Returns true if the element's audio can play at its rate. At any other rate, the element plays without sound.
bool audioPlaysAtRate(const HtmlMedia* media) {
    return media->rate >= kPrAudioSourceMinRate && media->rate <= kPrAudioSourceMaxRate;
}

// The longest that the position advances past the audio's last report, in milliseconds. Past it the audio has stalled,
// and so does the position.
constexpr double kAudioClockHoldMilliseconds = 200;

// Returns the current playback position, which advances with the element's audio while that plays and has not ended,
// and otherwise with the clock.
double currentPosition(const HtmlMedia* media) {
    if (!potentiallyPlaying(media))
        return media->position;
    double position = 0;
    PrAudioClock clock{};
    bool followsAudio = media->source != nullptr && audioPlaysAtRate(media);
    if (followsAudio) {
        clock = prAudioSourceClock(media->source);
        followsAudio = !clock.ended;
    }
    if (followsAudio) {
        // The position holds until the audio begins, and never falls behind where it last settled.
        double elapsed = prTimeMonotonicMilliseconds() - clock.stamp;
        elapsed = elapsed > kAudioClockHoldMilliseconds ? kAudioClockHoldMilliseconds : elapsed;
        position = clock.valid ? clock.position + elapsed / 1000 * media->rate : media->position;
        position = position < media->position ? media->position : position;
    } else {
        position = media->position + (timelineNow(media) - media->since) / 1000 * media->rate;
    }
    if (!prF64IsNan(media->duration) && position > media->duration)
        position = media->duration;
    if (position > media->playableEnd)
        position = media->playableEnd > media->position ? media->playableEnd : media->position;
    return position < 0 ? 0 : position;
}

// Fixes the position at its current value, and continues from there.
void settlePosition(HtmlMedia* media) {
    double position = currentPosition(media);
    if (position != media->position)
        addPlayed(media, media->position < position ? media->position : position,
            media->position < position ? position : media->position);
    media->position = position;
    media->since = timelineNow(media);
}

// Returns true if the element is muted: as a script last set it, or else as its muted attribute says
// (html/#dom-media-muted).
bool isMuted(const HtmlMedia* media) {
    return media->mutedSet ? media->muted : domAttribute(media->element, PrDomNamespace_None, "muted") != nullptr;
}

// Updates the pseudo-classes of the element's state (DomMediaFlags).
void updateFlags(HtmlMedia* media) {
    DomMediaFlags flags = DomMediaFlag_Tracked;
    if (media->paused)
        flags |= DomMediaFlag_Paused;
    else
        flags |= DomMediaFlag_Playing;
    if (media->seeking)
        flags |= DomMediaFlag_Seeking;
    if (!media->paused && media->networkState == PrHtmlMediaNetworkState_Loading &&
            media->readyState <= PrHtmlMediaReadyState_HaveCurrentData)
        flags |= DomMediaFlag_Buffering;
    if (isMuted(media))
        flags |= DomMediaFlag_Muted;
    domElementState(media->element)->media = flags;
}

void schedule(HtmlMedia* media);

// ---------------------------------------------------------------------------------------------------------------------
// Pictures (html/#the-video-element)

// Marks the picture of a video element out of date, which the window's next rendering update brings up to date.
void pictureChanged(HtmlMedia* media) {
    media->stale = media->element->tag == PrDomTag_Video;
}

// Releases the resource's decoder and its decoded frame.
void closeResource(HtmlMedia* media) {
    // The audio source reads the decoder, and the decoder reads a media source's buffers, so they go in that order.
    prAudioSourceDestroy(media->source);
    media->source = nullptr;
    prMediaDecoderDestroy(media->decoder);
    media->decoder = nullptr;
    if (media->mediaSource != nullptr) {
        htmlMediaSourceDetach(media->mediaSource);
        media->mediaSource = nullptr;
    }
    media->playableEnd = kPrF64Infinity;
    media->seekWaits = false;
    media->decoder = nullptr;
    prDrawImageRelease(media->frame);
    media->frame = nullptr;
    media->frameSerial = 0;
}

// Returns the image that the video shows: its poster frame while no video data is available, or while it is paused with
// the show poster flag set, if it has a poster frame. Otherwise the frame at its playback position.
const PrDrawImage* PR_NULLABLE pictureOf(const HtmlMedia* media) {
    bool data = media->frame != nullptr && media->readyState >= PrHtmlMediaReadyState_HaveCurrentData;
    if (!data || (media->showPoster && media->paused && media->poster != nullptr))
        return media->poster;
    return media->frame;
}

// Decodes the frame at the playback position into the video's frame. Returns false if the frame that shows there is not
// decoded yet, and true once it is, or if none will be.
bool decodeFrame(HtmlMedia* media) {
    if (media->decoder == nullptr || !prMediaDecoderHasFrames(media->decoder))
        return true;
    PrMediaFrame frame{};
    if (!prMediaDecoderFrame(media->decoder, currentPosition(media), &frame))
        return frame.current;
    // The page draws on the CPU, so it keeps a copy of the frame's texels as an image.
    PrGpuImportPixels pixels{};
    if (frame.serial != media->frameSerial && prGpuImportLock(&frame.image, &pixels)) {
        PrDrawImageDesc desc{
            .pixels = pixels.pixels,
            .format = PrPixelFormat_Bgra8,
            .alpha = PrDrawAlpha_Premultiplied,
            .space = PrColorSpace_Srgb,
        };
        const PrDrawImage* image = nullptr;
        if (prDrawImageCreate(&desc, &image) == PrStatus_Ok) {
            prDrawImageRelease(media->frame);
            media->frame = image;
            media->frameSerial = frame.serial;
        }
        prGpuImportUnlock(&frame.image);
    }
    return frame.current;
}

// Shows the video's picture as its element's: its natural size is the video's once it is known, and else the poster
// frame's (html/#concept-video-intrinsic-width).
void showPicture(HtmlMedia* media) {
    DomElementState* state = domElementState(media->element);
    const PrDrawImage* picture = pictureOf(media);
    PrUVec2 natural{media->videoWidth, media->videoHeight};
    if ((natural.x == 0 || natural.y == 0) && media->poster != nullptr)
        natural = prDrawImageExtent(media->poster);
    PrDomImageState image = PrDomImageState_Unavailable;
    if (natural.x != 0 && natural.y != 0)
        image = PrDomImageState_CompletelyAvailable;
    if (state->picture == picture && state->image == image && state->naturalWidth == natural.x &&
            state->naturalHeight == natural.y)
        return;
    if (picture != nullptr)
        prDrawImageRetain(picture);
    prDrawImageRelease(state->picture);
    prDrawImageRelease(state->storedPicture);
    state->picture = picture;
    state->storedPicture = nullptr;
    state->orientation = PrImageOrientation_Identity;
    state->image = image;
    state->naturalWidth = natural.x;
    state->naturalHeight = natural.y;
    if (domIsConnected(media->element))
        cssPresentationElementContentChanged(media->element);
}

// A fetch of a video's poster frame, which holds the element. `generation` tells it from a later fetch, whose result
// replaces it.
struct PosterFetch {
    HtmlMedia* media;
    uint32_t generation;
};

void dropPosterFetch(void* data) {
    PosterFetch* fetch = static_cast<PosterFetch*>(data);
    PrDomElement* element = fetch->media->element;
    prMemoryFree(fetch);
    domNodeUnhold(element);
}

void posterFetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    PosterFetch* fetch = static_cast<PosterFetch*>(data);
    HtmlMedia* media = fetch->media;
    PrImageCodec* images = media->window != nullptr ? media->window->images : nullptr;
    if (fetch->generation == media->posters && images != nullptr && domFetchIsOk(result)) {
        PrByteView bytes{reinterpret_cast<const uint8_t*>(result->body.data), result->body.size};
        const PrDrawImage* poster = nullptr;
        if (prDrawImageCreateFromBytes(images, bytes, &poster, nullptr) == PrStatus_Ok) {
            prDrawImageRelease(media->poster);
            media->poster = poster;
            pictureChanged(media);
            showPicture(media);
        }
    }
    dropPosterFetch(fetch);
}

// Fetches the poster frame that the poster attribute names (html/#attr-video-poster), and drops the last one.
void updatePoster(HtmlMedia* media) {
    ++media->posters;
    prDrawImageRelease(media->poster);
    media->poster = nullptr;
    pictureChanged(media);
    PrStringView source = domAttributeValue(media->element, "poster");
    const DomDocumentHost* host = media->element->document->host;
    PrUrl* url = nullptr;
    if (media->window == nullptr || host == nullptr || source.size == 0 ||
            domDocumentParseUrl(media->element->document, source, &url) != PrStatus_Ok) {
        prUrlDestroy(url);
        return;
    }
    PrRawString request = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
    domNodeHold(media->element);
    PosterFetch* fetch = alloc<PosterFetch>();
    *fetch = PosterFetch{media, media->posters};
    DomFetchRequest asked{
        prRawStringView(&request), DomFetchDestination_Image, DomCorsSettings_NoCors,
        DomFetchInitiator_Video,
    };
    host->fetch(host->context, &asked, posterFetched, fetch, dropPosterFetch);
    prRawStringReset(&request);
    prUrlDestroy(url);
}

// Runs the steps for playing forwards to the end (html/#playing-the-media-resource).
void reachedEnd(HtmlMedia* media) {
    if (domAttribute(media->element, PrDomNamespace_None, "loop") != nullptr) {
        htmlMediaSeek(media, 0);
        return;
    }
    MediaTask* task = newTask(media);
    task->event = "timeupdate";
    task->run = [](MediaTask* task) {
        HtmlMedia* ended = task->media;
        if (!ended->paused) {
            ended->paused = true;
            updateFlags(ended);
            domEventFire(ended->element, "pause");
            settle(&ended->promises, "AbortError");
        }
    };
    task->after = "ended";
    queueTask(task);
}

void timerFired(void* PR_NULLABLE context) {
    HtmlMedia* media = static_cast<HtmlMedia*>(context);
    media->timer = 0;
    settlePosition(media);
    bool end = !prF64IsNan(media->duration) && media->position >= media->duration && media->rate > 0;
    domEventFire(media->element, "timeupdate");
    if (end)
        reachedEnd(media);
    if (media->mediaSource != nullptr)
        htmlMediaSourceChanged(media);
    schedule(media);
}

// Brings what the element's audio plays into line with the element's state.
void updateAudio(HtmlMedia* media) {
    if (media->source == nullptr)
        return;
    bool audible = audioPlaysAtRate(media);
    prAudioSourceSetPlaying(media->source, audible && potentiallyPlaying(media));
    prAudioSourceSetGain(media->source, isMuted(media) ? 0 : float(media->volume));
    if (audible)
        prAudioSourceSetRate(media->source, media->rate, media->preservesPitch);
}

// Sets the timer for the next timeupdate, or for the end if that comes first, while the element plays.
void schedule(HtmlMedia* media) {
    updateAudio(media);
    if (media->timer != 0) {
        htmlWindowClearTimeout(media->window, media->timer);
        media->timer = 0;
    }
    if (media->window == nullptr || !potentiallyPlaying(media) || media->rate == 0)
        return;
    double wait = kTimeUpdateMilliseconds;
    if (!prF64IsNan(media->duration) && media->rate > 0) {
        double end = (media->duration - currentPosition(media)) * 1000 / media->rate;
        if (end < wait)
            wait = end;
    }
    // A media source stalls at the end of what it has buffered.
    if (media->rate > 0 && media->playableEnd != kPrF64Infinity) {
        double stall = (media->playableEnd - currentPosition(media)) * 1000 / media->rate;
        if (stall < wait)
            wait = stall;
    }
    // The timer holds the element.
    PrHtmlTimerHandler handler = {
        timerFired,
        media,
        [](void* context) { domNodeHold(static_cast<HtmlMedia*>(context)->element); },
        [](void* context) { domNodeUnhold(static_cast<HtmlMedia*>(context)->element); },
    };
    // Playing in a loop never ends, so its time updates do not keep the window from being idle. Playing to the end
    // does, because the end fires events.
    bool loops = domAttribute(media->element, PrDomNamespace_None, "loop") != nullptr;
    media->timer = htmlWindowSetTimeout(media->window, &handler, int32_t(prF64Ceil(wait < 0 ? 0 : wait)), false, loops);
}

// html/#notify-about-playing
void notifyPlaying(HtmlMedia* media) {
    queueSettle(media, "playing", {});
}

// Changes the readiness (html/#ready-states), and fires the events of each state passed.
void setReadyState(HtmlMedia* media, PrHtmlMediaReadyState state) {
    PrHtmlMediaReadyState was = media->readyState;
    media->readyState = state;
    if (was < PrHtmlMediaReadyState_HaveCurrentData &&
            state >= PrHtmlMediaReadyState_HaveCurrentData && !media->loadedData) {
        media->loadedData = true;
        queueEvent(media, "loadeddata");
    }
    if (was < PrHtmlMediaReadyState_HaveFutureData && state >= PrHtmlMediaReadyState_HaveFutureData) {
        queueEvent(media, "canplay");
        if (!media->paused)
            notifyPlaying(media);
    }
    if (was < PrHtmlMediaReadyState_HaveEnoughData && state == PrHtmlMediaReadyState_HaveEnoughData) {
        // html/#eligible-for-autoplay: the autoplay attribute plays a paused element that no script has paused or
        // played.
        if (media->paused && media->autoplaying && domAttribute(media->element, PrDomNamespace_None, "autoplay") != nullptr) {
            media->paused = false;
            media->since = timelineNow(media);
            queueEvent(media, "play");
            notifyPlaying(media);
        }
        queueEvent(media, "canplaythrough");
    }
    updateFlags(media);
    schedule(media);
}

// html/#dedicated-media-source-failure-steps
void sourceFailed(HtmlMedia* media, PrStringView message) {
    MediaTask* task = newTask(media);
    prRawStringReplace(&media->errorMessage, message);
    task->run = [](MediaTask* task) {
        HtmlMedia* failed = task->media;
        failed->error = PrHtmlMediaError_SrcNotSupported;
        failed->networkState = PrHtmlMediaNetworkState_NoSource;
        domEventFire(failed->element, "error");
        settle(&failed->promises, "NotSupportedError");
        updateFlags(failed);
    };
    queueTask(task);
}

// html/#concept-media-load-resource, once the resource has arrived. The window's codec reads its metadata, and then all
// of its data, which the engine has as soon as the resource is fetched. Returns false for a resource that the codec
// does not read. Starts playing the resource's audio, if the window has a mixer and the codec decodes the audio.
// Without a source, the element's timeline follows the clock.
void startAudio(HtmlMedia* media, const PrMediaInfo& info) {
    PrAudioMixer* mixer = media->window != nullptr ? media->window->mixer : nullptr;
    if (mixer == nullptr || !info.hasAudio)
        return;
    PrAudioFormat format = prAudioMixerFormat(mixer);
    if (!prMediaDecoderAudioStart(media->decoder, format.sampleRate, format.channels, media->position))
        return;
    PrAudioSourceReadFn read = [](void* context, float* samples, uint32_t frames) {
        PrMediaAudioChunk chunk = prMediaDecoderAudioRead(static_cast<PrMediaDecoder*>(context), samples, frames);
        return PrAudioChunk{.frames = chunk.frames, .time = chunk.time, .ended = chunk.ended};
    };
    media->source = prAudioSourceCreate(mixer, read, media->decoder);
}

// Makes the decoder of the element's media source from its active buffers, if it has none, and starts its audio.
void openSourceDecoder(HtmlMedia* media) {
    PrMediaCodec* codec = media->window != nullptr ? media->window->mediaCodec : nullptr;
    if (media->decoder != nullptr || codec == nullptr)
        return;
    PrMediaSourceBuffer* buffers[8];
    uint32_t count = htmlMediaSourceActiveBuffers(media->mediaSource, buffers, 8);
    if (count == 0)
        return;
    PrMediaInfo info{};
    if (prMediaDecoderCreateFromSource(codec, buffers, count < 8 ? count : 8, &media->decoder, &info) != PrStatus_Ok)
        return;
    startAudio(media, info);
    pictureChanged(media);
}

bool resourceArrived(HtmlMedia* media, PrStringView body) {
    PrMediaCodec* codec = media->window != nullptr ? media->window->mediaCodec : nullptr;
    PrMediaInfo info{};
    PrByteView bytes{reinterpret_cast<const uint8_t*>(body.data), body.size};
    if (codec == nullptr || prMediaDecoderCreate(codec, bytes, &media->decoder, &info) != PrStatus_Ok)
        return false;
    startAudio(media, info);
    media->duration = info.duration;
    queueEvent(media, "durationchange");
    if (info.videoExtent.x != 0 && info.videoExtent.y != 0 && media->element->tag == PrDomTag_Video) {
        media->videoWidth = info.videoExtent.x;
        media->videoHeight = info.videoExtent.y;
        queueEvent(media, "resize");
    }
    pictureChanged(media);
    // The natural size is the video's from the moment that its metadata is known.
    if (media->element->tag == PrDomTag_Video)
        showPicture(media);
    media->readyState = PrHtmlMediaReadyState_HaveMetadata;
    queueEvent(media, "loadedmetadata");
    queueEvent(media, "progress");
    setReadyState(media, PrHtmlMediaReadyState_HaveEnoughData);
    media->networkState = PrHtmlMediaNetworkState_Idle;
    queueEvent(media, "suspend");
    updateFlags(media);
    return true;
}

// Returns the candidate at `source` parsed against the element's document, or null if it does not parse.
PrUrl* PR_NULLABLE candidateUrl(HtmlMedia* media, PrStringView source) {
    PrUrl* url = nullptr;
    bool parsed = source.size != 0 && domDocumentParseUrl(media->element->document, source, &url) == PrStatus_Ok;
    if (parsed)
        return url;
    prUrlDestroy(url);
    return nullptr;
}

void trySources(HtmlMedia* media, const PrDomNode* PR_NULLABLE from);

// Fails the candidate that a fetch was for. A failed resource from the src attribute fails the load. A source child
// fires error at itself and gives way to the next child.
void candidateFailed(MediaTask* fetch) {
    HtmlMedia* media = fetch->media;
    if (fetch->target == nullptr) {
        sourceFailed(media, "The resource is not a supported format");
        return;
    }
    PrDomElement* source = fetch->target;
    MediaTask* failed = newTask(media);
    failed->event = "error";
    failed->target = source;
    domNodeHold(source);
    queueTask(failed);
    trySources(media, source->next);
}

// Receives the result of a fetch of a candidate resource, for the load that it belongs to. The fetch is a task that
// holds the element, and holds the source child that it belongs to as its target.
void candidateFetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    MediaTask* fetch = static_cast<MediaTask*>(data);
    if (fetch->load == fetch->media->load &&
            (result->status != PrStatus_Ok || !resourceArrived(fetch->media, result->body)))
        candidateFailed(fetch);
    dropTask(fetch);
}

// Starts fetching the candidate at `url`. If `source` is null, the candidate comes from the src attribute.
void fetchCandidate(HtmlMedia* media, const PrUrl* url, PrDomElement* PR_NULLABLE source) {
    prRawStringReset(&media->currentSrc);
    media->currentSrc = prUrlSerialize(url, PrUrlSerialization_Whole);
    // A URL of a media source attaches the source (media-source-2/#mediasource-attach).
    PrRawString whole = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
    PrHtmlMediaSource* mediaSource = nullptr;
    if (media->window != nullptr)
        mediaSource = htmlMediaSourceFromUrl(media->window, prRawStringView(&whole));
    prRawStringReset(&whole);
    if (mediaSource != nullptr) {
        if (htmlMediaSourceAttach(mediaSource, media)) {
            media->mediaSource = mediaSource;
            media->playableEnd = 0;
            return;
        }
        MediaTask* failed = newTask(media);
        if (source != nullptr) {
            domNodeHold(source);
            failed->target = source;
        }
        candidateFailed(failed);
        dropTask(failed);
        return;
    }
    MediaTask* fetch = newTask(media);
    if (source != nullptr) {
        domNodeHold(source);
        fetch->target = source;
    }
    const DomDocumentHost* host = media->element->document->host;
    if (host == nullptr) {
        candidateFailed(fetch);
        dropTask(fetch);
        return;
    }
    PrRawString request = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
    DomFetchRequest asked{
        prRawStringView(&request), DomFetchDestination_Media,
        domCorsSettings(domAttributeValue(media->element, "crossorigin")),
        DomFetchInitiator(media->element->tag == PrDomTag_Video ? DomFetchInitiator_Video : DomFetchInitiator_Audio),
    };
    host->fetch(host->context, &asked, candidateFetched, fetch, dropTask);
    prRawStringReset(&request);
}

// Tries the source children from `from` onward, in order. Fetches the first child whose type the engine may play and
// whose src parses, and fires error at each child before it that does not qualify. If no child is left, waits for
// another child.
void trySources(HtmlMedia* media, const PrDomNode* PR_NULLABLE from) {
    for (const PrDomNode* child = from; child != nullptr; child = child->next) {
        PrDomElement* source = domElementFromNode(const_cast<PrDomNode*>(child));
        if (!domIsHtml(source, PrDomTag_Source))
            continue;
        const PrDomAttr* type = domAttribute(source, PrDomNamespace_None, "type");
        PrUrl* url;
        if (type != nullptr && htmlCanPlayType(media->window, prRawStringView(&type->value)) == PrHtmlCanPlay_No)
            url = nullptr;
        else
            url = candidateUrl(media, domAttributeValue(source, "src"));
        if (url != nullptr) {
            fetchCandidate(media, url, source);
            prUrlDestroy(url);
            return;
        }
        MediaTask* failed = newTask(media);
        failed->event = "error";
        failed->target = source;
        domNodeHold(source);
        queueTask(failed);
    }
    // html/#concept-media-load-algorithm: waiting for another source child.
    media->networkState = PrHtmlMediaNetworkState_NoSource;
    updateFlags(media);
}

// html/#concept-media-load-algorithm after its stable state. Selects the src attribute's resource, or else the resource
// of the first source child that loads.
void selectResource(MediaTask* task) {
    HtmlMedia* media = task->media;
    PrDomElement* element = media->element;
    const PrDomAttr* src = domAttribute(element, PrDomNamespace_None, "src");
    const PrDomElement* firstSource = nullptr;
    for (const PrDomNode* child = element->first; child != nullptr && firstSource == nullptr; child = child->next) {
        const PrDomElement* candidate = domElementFromNode(child);
        if (domIsHtml(candidate, PrDomTag_Source))
            firstSource = candidate;
    }
    if (src == nullptr && firstSource == nullptr) {
        media->networkState = PrHtmlMediaNetworkState_Empty;
        updateFlags(media);
        return;
    }
    media->networkState = PrHtmlMediaNetworkState_Loading;
    queueEvent(media, "loadstart");
    if (src != nullptr) {
        PrUrl* url = candidateUrl(media, prRawStringView(&src->value));
        if (url == nullptr) {
            sourceFailed(media, "The src attribute is not a URL");
            return;
        }
        fetchCandidate(media, url, nullptr);
        prUrlDestroy(url);
        return;
    }
    trySources(media, element->first);
}

void selectResourceTask(void* data) {
    MediaTask* task = static_cast<MediaTask*>(data);
    if (task->load == task->media->load)
        selectResource(task);
    dropTask(task);
}

// html/#concept-media-load-algorithm. Its steps after the first run once the script that invoked it is done
// (html/#await-a-stable-state). An element that no window runs selects no resource.
void resourceSelection(HtmlMedia* media) {
    if (media->window == nullptr)
        return;
    media->networkState = PrHtmlMediaNetworkState_NoSource;
    MediaTask* task = newTask(media);
    const DomDocumentHost* host = media->element->document->host;
    if (!host->queueMicrotask(host->context, selectResourceTask, task))
        dropTask(task);
}

// Queues the end of the latest seek: the media data is at the new position once this task runs, unless a later seek has
// started, whose own task ends the seeking instead.
void queueSeekDone(HtmlMedia* media) {
    MediaTask* done = newTask(media);
    done->seekCount = media->seeks;
    done->run = [](MediaTask* task) {
        HtmlMedia* seeked = task->media;
        if (task->seekCount != seeked->seeks)
            return;
        seeked->seeking = false;
        seeked->since = timelineNow(seeked);
        updateFlags(seeked);
        domEventFire(seeked->element, "timeupdate");
        domEventFire(seeked->element, "seeked");
        schedule(seeked);
    };
    queueTask(done);
}

// html/#internal-play-steps
void internalPlay(HtmlMedia* media) {
    if (media->networkState == PrHtmlMediaNetworkState_Empty)
        resourceSelection(media);
    if (htmlMediaEnded(media))
        htmlMediaSeek(media, 0);
    if (media->paused) {
        media->paused = false;
        // html/#internal-play-steps: playing unsets the show poster flag.
        media->showPoster = false;
        pictureChanged(media);
        media->since = timelineNow(media);
        queueEvent(media, "play");
        if (media->readyState <= PrHtmlMediaReadyState_HaveCurrentData)
            queueEvent(media, "waiting");
        else
            notifyPlaying(media);
    } else if (media->readyState >= PrHtmlMediaReadyState_HaveFutureData) {
        queueSettle(media, {}, {});
    }
    media->autoplaying = false;
    updateFlags(media);
    schedule(media);
}

// html/#internal-pause-steps
void internalPause(HtmlMedia* media) {
    if (media->networkState == PrHtmlMediaNetworkState_Empty)
        resourceSelection(media);
    media->autoplaying = false;
    if (!media->paused) {
        settlePosition(media);
        media->paused = true;
        MediaTask* task = newTask(media);
        task->event = "timeupdate";
        task->after = "pause";
        task->promises = media->promises;
        media->promises = {};
        task->rejection = "AbortError";
        queueTask(task);
    }
    updateFlags(media);
    schedule(media);
}

// Sets the playback state to its initial values, which an element has before any window runs it.
void initializePlayback(HtmlMedia* media) {
    media->currentSrc = prRawStringEmpty();
    media->errorMessage = prRawStringEmpty();
    media->position = 0;
    media->since = 0;
    media->duration = kPrF64Nan;
    media->defaultRate = 1;
    media->rate = 1;
    media->volume = 1;
    media->videoWidth = 0;
    media->videoHeight = 0;
    media->timer = 0;
    media->networkState = PrHtmlMediaNetworkState_Empty;
    media->readyState = PrHtmlMediaReadyState_HaveNothing;
    media->error = PrHtmlMediaError_None;
    media->paused = true;
    media->seeking = false;
    media->muted = false;
    media->mutedSet = false;
    media->autoplaying = true;
    media->preservesPitch = true;
    media->loadedData = false;
    media->playableEnd = kPrF64Infinity;
    media->showPoster = true;
}

// Frees what the playback state holds: its resource and its poster frame too. A poster fetch that is still running is
// dropped when it ends.
void resetPlayback(HtmlMedia* media) {
    closeResource(media);
    prDrawImageRelease(media->poster);
    media->poster = nullptr;
    ++media->posters;
    releasePromises(&media->promises);
    media->played.reset();
    prRawStringReset(&media->currentSrc);
    prRawStringReset(&media->errorMessage);
}

// Takes the state out of the window that runs it, and clears its timer there. Reads nothing of the element.
void leaveWindow(HtmlMedia* media) {
    PrHtmlWindow* window = media->window;
    if (window == nullptr)
        return;
    if (media->timer != 0)
        htmlWindowClearTimeout(window, media->timer);
    for (uint32_t i = 0; i < window->mediaElements.count; ++i) {
        if (window->mediaElements.data[i] == media) {
            window->mediaElements.erase(i);
            break;
        }
    }
    media->window = nullptr;
}

// Removes the state from the window that runs it. The window's tasks and timers of the state are gone or are dropped,
// and the state returns to its initial playback state. A later load starts over.
void detach(HtmlMedia* media) {
    if (media->window == nullptr)
        return;
    leaveWindow(media);
    ++media->load;
    resetPlayback(media);
    initializePlayback(media);
    updateFlags(media);
    pictureChanged(media);
}

// Frees the element's state along with the element, which is being destroyed, so nothing here reads it.
void forgetMedia(void* data) {
    HtmlMedia* media = static_cast<HtmlMedia*>(data);
    leaveWindow(media);
    resetPlayback(media);
    domListenersDestroy(media->textTracks.listeners);
    domListenersDestroy(media->audioTracks.listeners);
    domListenersDestroy(media->videoTracks.listeners);
    prMemoryFree(media);
}

// Initializes a list of the element's tracks of the kind that `type` names.
void initializeTracks(HtmlTrackList* list, PrDomElement* element, DomTargetType type) {
    list->type = type;
    list->owner = prDomElementAsEventTarget(element);
}

// html/#media-elements: a removed element pauses once the script that removed it is done, unless the element is back in
// a document.
void removedTask(void* data) {
    MediaTask* task = static_cast<MediaTask*>(data);
    if (!domIsConnected(task->media->element))
        internalPause(task->media);
    dropTask(task);
}

} // namespace

HtmlMedia* htmlMediaFromElement(PrDomElement* element) {
    DomElementState* state = domElementState(element);
    if (state->forgetEmbedded == forgetMedia)
        return static_cast<HtmlMedia*>(state->embedded);
    HtmlMedia* media = allocZeroed<HtmlMedia>();
    media->element = element;
    initializeTracks(&media->textTracks, element, DomTargetType_TextTrackList);
    initializeTracks(&media->audioTracks, element, DomTargetType_AudioTrackList);
    initializeTracks(&media->videoTracks, element, DomTargetType_VideoTrackList);
    initializePlayback(media);
    state->embedded = media;
    state->forgetEmbedded = forgetMedia;
    updateFlags(media);
    return media;
}

HtmlMedia* PR_NULLABLE htmlMediaRunning(PrDomElement* element) {
    HtmlMedia* media = htmlMediaFromElement(element);
    PrHtmlWindow* window = static_cast<PrHtmlWindow*>(element->document->global);
    if (window == nullptr || window->document != element->document || element->document->host == nullptr)
        window = nullptr;
    if (media->window != window) {
        detach(media);
        if (window != nullptr) {
            media->window = window;
            window->mediaElements.push(media);
            // The poster frame is the new window's to fetch.
            if (element->tag == PrDomTag_Video)
                updatePoster(media);
        }
    }
    return window != nullptr ? media : nullptr;
}

void htmlMediaReset(PrHtmlWindow* window) {
    // The window's tasks and timers are gone, and with them the references that held the elements.
    while (window->mediaElements.count != 0) {
        HtmlMedia* media = window->mediaElements.last();
        media->timer = 0;
        detach(media);
        if (media->element->tag == PrDomTag_Video)
            showPicture(media);
    }
    window->mediaElements.reset();
}

bool htmlMediaAwaitsPicture(const PrHtmlWindow* window) {
    for (uint32_t i = 0; i < window->mediaElements.count; ++i) {
        if (window->mediaElements.data[i]->stale)
            return true;
    }
    return false;
}

bool htmlMediaWantsFrame(const PrHtmlWindow* window) {
    for (uint32_t i = 0; i < window->mediaElements.count; ++i) {
        const HtmlMedia* media = window->mediaElements.data[i];
        if (media->stale || (potentiallyPlaying(media) && media->frame != nullptr))
            return true;
    }
    return false;
}

void htmlMediaShow(PrHtmlWindow* window) {
    for (uint32_t i = 0; i < window->mediaElements.count; ++i) {
        HtmlMedia* media = window->mediaElements.data[i];
        if (media->element->tag != PrDomTag_Video || (!media->stale && !potentiallyPlaying(media)))
            continue;
        // A frame that is not decoded yet leaves the picture out of date, and the last frame shows until it is.
        media->stale = !decodeFrame(media);
        showPicture(media);
    }
}

void htmlMediaChanged(PrDomElement* element, PrStringView attribute) {
    bool connection = attribute.data == nullptr;
    if (!connection && attribute != "src" && attribute != "muted" && attribute != "poster")
        return;
    HtmlMedia* media = htmlMediaRunning(element);
    if (media == nullptr)
        return;
    if (attribute == "poster") {
        if (element->tag == PrDomTag_Video)
            updatePoster(media);
    } else if (attribute == "muted") {
        updateFlags(media);
        updateAudio(media);
    } else if (!connection) {
        // html/#location-of-the-media-resource: setting or changing src loads the element. Removing it does not.
        if (domAttribute(element, PrDomNamespace_None, "src") != nullptr)
            htmlMediaLoad(media);
    } else if (domIsConnected(element)) {
        if (media->networkState == PrHtmlMediaNetworkState_Empty)
            resourceSelection(media);
    } else {
        MediaTask* task = newTask(media);
        const DomDocumentHost* host = element->document->host;
        if (!host->queueMicrotask(host->context, removedTask, task))
            dropTask(task);
    }
}

void htmlMediaLoad(HtmlMedia* media) {
    ++media->load;
    // html/#media-element-load-algorithm: the load drops the last resource, and sets the show poster flag.
    closeResource(media);
    media->showPoster = true;
    pictureChanged(media);
    media->played.clear();
    if (media->networkState == PrHtmlMediaNetworkState_Loading || media->networkState == PrHtmlMediaNetworkState_Idle)
        queueEvent(media, "abort");
    if (media->networkState != PrHtmlMediaNetworkState_Empty) {
        queueEvent(media, "emptied");
        media->networkState = PrHtmlMediaNetworkState_Empty;
        media->readyState = PrHtmlMediaReadyState_HaveNothing;
        media->loadedData = false;
        if (!media->paused) {
            media->paused = true;
            queueSettle(media, {}, "AbortError");
        }
        media->seeking = false;
        if (media->position != 0) {
            media->position = 0;
            queueEvent(media, "timeupdate");
        }
        media->duration = kPrF64Nan;
        media->videoWidth = 0;
        media->videoHeight = 0;
    }
    media->rate = media->defaultRate;
    media->error = PrHtmlMediaError_None;
    prRawStringReplace(&media->errorMessage, "");
    media->autoplaying = true;
    // A load rejects the play() promises that are still pending (html/#media-element-load-algorithm).
    queueSettle(media, {}, "AbortError");
    resourceSelection(media);
    updateFlags(media);
    schedule(media);
}

double htmlMediaCurrentTime(HtmlMedia* media) {
    return currentPosition(media);
}

// html/#dom-media-seek
void htmlMediaSeek(HtmlMedia* media, double seconds) {
    if (media->readyState == PrHtmlMediaReadyState_HaveNothing) {
        media->position = seconds < 0 ? 0 : seconds;
        return;
    }
    settlePosition(media);
    if (!prF64IsNan(media->duration) && seconds > media->duration)
        seconds = media->duration;
    if (seconds < 0)
        seconds = 0;
    media->seeking = true;
    // html/#dom-media-seek: seeking unsets the show poster flag.
    media->showPoster = false;
    pictureChanged(media);
    MediaTask* start = newTask(media);
    start->event = "seeking";
    queueTask(start);
    media->position = seconds;
    media->since = timelineNow(media);
    if (media->source != nullptr) {
        prMediaDecoderAudioSeek(media->decoder, seconds);
        prAudioSourceReset(media->source);
    }
    ++media->seeks;
    // A media source may not hold the new position yet, and the seek waits for it
    // (media-source-2/#mediasource-seeking).
    media->seekWaits = media->mediaSource != nullptr;
    if (media->mediaSource != nullptr)
        htmlMediaSourceChanged(media);
    else
        queueSeekDone(media);
    updateFlags(media);
    schedule(media);
}

bool htmlMediaEnded(HtmlMedia* media) {
    // html/#ended-playback, playing forwards.
    return media->readyState >= PrHtmlMediaReadyState_HaveMetadata && !prF64IsNan(media->duration) &&
        currentPosition(media) >= media->duration && media->rate >= 0 &&
        domAttribute(media->element, PrDomNamespace_None, "loop") == nullptr;
}

void htmlMediaPlay(HtmlMedia* media, const PrHtmlPlayPromise& promise) {
    if (media->error == PrHtmlMediaError_SrcNotSupported) {
        promise.settle(promise.context, "NotSupportedError");
        promise.release(promise.context);
        return;
    }
    media->promises.push(promise);
    internalPlay(media);
}

void htmlMediaPause(HtmlMedia* media) {
    internalPause(media);
}

void htmlMediaSetRate(HtmlMedia* media, double rate, bool isDefault) {
    settlePosition(media);
    if (isDefault)
        media->defaultRate = rate;
    else
        media->rate = rate;
    queueEvent(media, "ratechange");
    schedule(media);
}

void htmlMediaSetVolume(HtmlMedia* media, double volume, bool muted) {
    if (volume == media->volume && muted == isMuted(media) && media->mutedSet)
        return;
    media->volume = volume;
    media->muted = muted;
    media->mutedSet = true;
    queueEvent(media, "volumechange");
    updateFlags(media);
    updateAudio(media);
}

void htmlMediaSetPreservesPitch(HtmlMedia* media, bool preserves) {
    media->preservesPitch = preserves;
    updateAudio(media);
}

void htmlMediaSourceMetadata(HtmlMedia* media, PrUVec2 videoExtent) {
    if (media->readyState != PrHtmlMediaReadyState_HaveNothing)
        return;
    if (videoExtent.x != 0 && videoExtent.y != 0 && media->element->tag == PrDomTag_Video) {
        media->videoWidth = videoExtent.x;
        media->videoHeight = videoExtent.y;
        queueEvent(media, "resize");
    }
    media->readyState = PrHtmlMediaReadyState_HaveMetadata;
    queueEvent(media, "loadedmetadata");
    htmlMediaSourceChanged(media);
}

void htmlMediaSourceDuration(HtmlMedia* media, double duration) {
    settlePosition(media);
    media->duration = duration;
    queueEvent(media, "durationchange");
    // A position past the new duration moves to it (media-source-2/#duration-change-algorithm).
    if (media->position > duration)
        media->position = duration;
    schedule(media);
}

// The time that a media source's data at the position must reach past it for the element to have enough data to play
// through, in seconds. A source that has ended has all of its data, however much that is.
constexpr double kEnoughAhead = 10;

// The longest gap in a media source's buffered ranges that playback crosses rather than waits at, in seconds. Edit
// lists and codec delays leave such gaps, most often before the first frame.
constexpr double kSkippableGap = 0.25;

void htmlMediaSourceChanged(HtmlMedia* media) {
    if (media->mediaSource == nullptr || media->readyState == PrHtmlMediaReadyState_HaveNothing)
        return;
    openSourceDecoder(media);
    if (media->decoder != nullptr)
        prMediaDecoderSetSourceEnded(media->decoder, htmlMediaSourceHasEnded(media->mediaSource));
    settlePosition(media);
    RawArray<PrMediaTimeRange> ranges{};
    htmlMediaSourceBuffered(media->mediaSource, &ranges);
    bool ended = htmlMediaSourceHasEnded(media->mediaSource);
    double position = media->position;
    // The data at the position: the range that holds it, or one that starts a short gap after it, which playback
    // crosses so that a page whose first frame starts a little after zero plays from the start.
    double end = -1;
    for (uint32_t i = 0; i < ranges.count; ++i) {
        if (ranges.data[i].start <= position + kSkippableGap && position < ranges.data[i].end)
            end = ranges.data[i].end;
    }
    bool last = end >= 0 && ranges.count != 0 && end == ranges.last().end;
    ranges.reset();
    // An ended source plays to the end of its last range, which is the end of the media.
    bool atEnd = ended && !prF64IsNan(media->duration) && position >= media->duration;
    media->playableEnd = end >= 0 ? (ended && last ? kPrF64Infinity : end) : (atEnd ? kPrF64Infinity : position);
    PrHtmlMediaReadyState state = PrHtmlMediaReadyState_HaveMetadata;
    if (atEnd || (end >= 0 && (end - position >= kEnoughAhead || (ended && last))))
        state = PrHtmlMediaReadyState_HaveEnoughData;
    else if (end > position)
        state = PrHtmlMediaReadyState_HaveFutureData;
    else if (end >= 0)
        state = PrHtmlMediaReadyState_HaveCurrentData;
    // A playing element that runs out of data waits for more (html/#ready-states).
    if (state < PrHtmlMediaReadyState_HaveFutureData && media->readyState >= PrHtmlMediaReadyState_HaveFutureData &&
            !media->paused)
        queueEvent(media, "waiting");
    if (media->seekWaits && state >= PrHtmlMediaReadyState_HaveCurrentData) {
        media->seekWaits = false;
        queueSeekDone(media);
    }
    if (state != media->readyState)
        setReadyState(media, state);
    else
        schedule(media);
    pictureChanged(media);
}

void htmlMediaSourceFailed(HtmlMedia* media, PrHtmlMediaError error) {
    // An error before the metadata is a failure of the resource (media-source-2/#end-of-stream-algorithm).
    if (media->readyState == PrHtmlMediaReadyState_HaveNothing) {
        sourceFailed(media, "The media source ended with an error");
        return;
    }
    MediaTask* task = newTask(media);
    task->run = [](MediaTask* task) {
        domEventFire(task->media->element, "error");
    };
    media->error = error;
    media->networkState = PrHtmlMediaNetworkState_Idle;
    queueTask(task);
    updateFlags(media);
}

void htmlMediaSourceTracksChanging(HtmlMedia* media) {
    prAudioSourceDestroy(media->source);
    media->source = nullptr;
    prMediaDecoderDestroy(media->decoder);
    media->decoder = nullptr;
    prDrawImageRelease(media->frame);
    media->frame = nullptr;
    media->frameSerial = 0;
}

bool htmlMediaSourceHasError(const HtmlMedia* media) {
    return media->error != PrHtmlMediaError_None;
}

PrHtmlCanPlay htmlCanPlayType(const PrHtmlWindow* PR_NULLABLE window, PrStringView type) {
    PrMediaCodec* codec = window != nullptr ? window->mediaCodec : nullptr;
    if (codec == nullptr)
        return PrHtmlCanPlay_No;
    switch (prMediaCodecSupportsType(codec, type)) {
        case PrMediaSupport_Probably: return PrHtmlCanPlay_Probably;
        case PrMediaSupport_Maybe: return PrHtmlCanPlay_Maybe;
        default: return PrHtmlCanPlay_No;
    }
}

} // namespace pr_impl
