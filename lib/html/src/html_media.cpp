#include "html/src/html_media.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/string_ops.h"
#include "pr/time.h"
#include "pr/url.h"

#include "dom/src/dom_mime.h"

namespace pr_impl {

namespace {

// The maximum time between two timeupdate events while a media element plays (html/#event-media-timeupdate).
constexpr double kTimeUpdateMilliseconds = 250;

// ---------------------------------------------------------------------------------------------------------------------
// Containers (the ISO base media file format, ISO/IEC 14496-12)

// The information that a media resource's container gives about it.
struct Metadata {
    double duration;   // in seconds
    uint32_t width;    // the size of its video track, or zero if it has none
    uint32_t height;
};

uint64_t readU64(const uint8_t* bytes) {
    return uint64_t(readBeU32(bytes)) << 32 | readBeU32(bytes + 4);
}

// Passes each box of [data, end) to `visit`, with its type, its payload, and the payload's size.
template <typename Visit>
void eachBox(const uint8_t* data, const uint8_t* end, Visit visit) {
    while (end - data >= 8) {
        uint64_t size = readBeU32(data);
        uint32_t header = 8;
        if (size == 1 && end - data >= 16) {
            size = readU64(data + 8);
            header = 16;
        } else if (size == 0) {
            size = uint64_t(end - data);
        }
        if (size < header || size > uint64_t(end - data))
            return;
        visit(readBeU32(data + 4), data + header, size_t(size - header));
        data += size;
    }
}

constexpr uint32_t fourCc(const char (&code)[5]) {
    return uint32_t(uint8_t(code[0])) << 24 | uint32_t(uint8_t(code[1])) << 16 |
        uint32_t(uint8_t(code[2])) << 8 | uint32_t(uint8_t(code[3]));
}

// Reads the duration of a movie (its mvhd box) and the size of its first video track (a trak whose handler is vide,
// with the size its tkhd box gives). Returns false for a resource that is not a movie.
bool readMp4(PrStringView resource, Metadata* out) {
    const uint8_t* data = reinterpret_cast<const uint8_t*>(resource.data);
    const uint8_t* end = data + resource.size;
    bool typed = false;
    bool timed = false;
    eachBox(data, end, [&](uint32_t type, const uint8_t* payload, size_t size) {
        if (type == fourCc("ftyp"))
            typed = true;
        if (type != fourCc("moov"))
            return;
        eachBox(payload, payload + size, [&](uint32_t child, const uint8_t* body, size_t length) {
            if (child == fourCc("mvhd") && length >= 32) {
                bool wide = body[0] == 1;
                uint32_t scale = readBeU32(body + (wide ? 20 : 12));
                uint64_t units = wide ? readU64(body + 24) : readBeU32(body + 16);
                if (scale != 0) {
                    out->duration = double(units) / scale;
                    timed = true;
                }
            } else if (child == fourCc("trak") && out->width == 0) {
                uint32_t width = 0;
                uint32_t height = 0;
                bool video = false;
                eachBox(body, body + length, [&](uint32_t part, const uint8_t* bytes, size_t count) {
                    if (part == fourCc("tkhd")) {
                        size_t at = bytes[0] == 1 ? 88 : 76;
                        if (count >= at + 8) {
                            width = readBeU32(bytes + at) >> 16;
                            height = readBeU32(bytes + at + 4) >> 16;
                        }
                    } else if (part == fourCc("mdia")) {
                        eachBox(bytes, bytes + count, [&](uint32_t inner, const uint8_t* handler, size_t n) {
                            video |= inner == fourCc("hdlr") && n >= 12 && readBeU32(handler + 8) == fourCc("vide");
                        });
                    }
                });
                if (video) {
                    out->width = width;
                    out->height = height;
                }
            }
        });
    });
    return typed && timed;
}

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
    Array<PrHtmlPlayPromise> promises;

    // The DOMException that the promises are rejected with, or the null view to resolve them.
    PrStringView rejection;

    uint32_t seekCount;                   // for a seek's completion, the seek that it completes
    PrDomElement* PR_NULLABLE target;     // the event's target if it is not the media element; the task holds it
};

// Releases the promises without settling them.
void releasePromises(Array<PrHtmlPlayPromise>* promises) {
    for (uint32_t i = 0; i < promises->count; ++i)
        promises->data[i].release(promises->data[i].context);
    promises->reset();
}

void settle(Array<PrHtmlPlayPromise>* promises, PrStringView rejection) {
    Array<PrHtmlPlayPromise> taken = *promises;
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

void queueTask(MediaTask* task) {
    htmlWindowQueueTask(task->media->window, runTask, task, dropTask);
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
        !(media->position >= media->duration && media->rate > 0 && !isNanF64(media->duration));
}

// Adds [start, end] to the played ranges (html/#dom-media-played), merging it with each range that it touches.
void addPlayed(HtmlMedia* media, double start, double end) {
    Array<double>& ranges = media->played;
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

// Returns the current playback position, which advances with the clock while the element plays.
double currentPosition(const HtmlMedia* media) {
    if (!potentiallyPlaying(media))
        return media->position;
    double position = media->position + (prTimeMonotonicMilliseconds() - media->since) / 1000 * media->rate;
    if (!isNanF64(media->duration) && position > media->duration)
        position = media->duration;
    return position < 0 ? 0 : position;
}

// Fixes the position at its current value, and continues from there.
void settlePosition(HtmlMedia* media) {
    double position = currentPosition(media);
    if (position != media->position)
        addPlayed(media, media->position < position ? media->position : position,
            media->position < position ? position : media->position);
    media->position = position;
    media->since = prTimeMonotonicMilliseconds();
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

// Runs the steps for playing forwards to the end (html/#playing-the-media-resource).
void reachedEnd(HtmlMedia* media) {
    if (domAttribute(media->element, PrDomNamespace_None, "loop") != nullptr) {
        htmlMediaSeek(media, 0);
        return;
    }
    MediaTask* task = newTask(media);
    task->event = "timeupdate";
    task->run = [](MediaTask* self) {
        HtmlMedia* ended = self->media;
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
    bool end = !isNanF64(media->duration) && media->position >= media->duration && media->rate > 0;
    domEventFire(media->element, "timeupdate");
    if (end)
        reachedEnd(media);
    schedule(media);
}

// Sets the timer for the next timeupdate, or for the end if that comes first, while the element plays.
void schedule(HtmlMedia* media) {
    if (media->timer != 0) {
        htmlWindowClearTimeout(media->window, media->timer);
        media->timer = 0;
    }
    if (!potentiallyPlaying(media) || media->rate == 0)
        return;
    double wait = kTimeUpdateMilliseconds;
    if (!isNanF64(media->duration) && media->rate > 0) {
        double end = (media->duration - currentPosition(media)) * 1000 / media->rate;
        if (end < wait)
            wait = end;
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
    media->timer = htmlWindowSetTimeout(media->window, &handler, int32_t(ceilF64(wait < 0 ? 0 : wait)), false, loops);
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
            media->since = prTimeMonotonicMilliseconds();
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
    prStringReplace(&media->errorMessage, message);
    task->run = [](MediaTask* self) {
        HtmlMedia* failed = self->media;
        failed->error = PrHtmlMediaError_SrcNotSupported;
        failed->networkState = PrHtmlMediaNetworkState_NoSource;
        domEventFire(failed->element, "error");
        settle(&failed->promises, "NotSupportedError");
        updateFlags(failed);
    };
    queueTask(task);
}

// html/#concept-media-load-resource, once the resource has arrived. Reads its metadata, and then all of its data, which
// the engine has as soon as the resource is fetched. Returns false for a resource whose format the engine does not
// read.
bool resourceArrived(HtmlMedia* media, PrStringView body) {
    Metadata metadata = {};
    if (!readMp4(body, &metadata))
        return false;
    media->duration = metadata.duration;
    queueEvent(media, "durationchange");
    if (metadata.width != 0 && metadata.height != 0 && media->element->tag == PrDomTag_Video) {
        media->videoWidth = metadata.width;
        media->videoHeight = metadata.height;
        queueEvent(media, "resize");
    }
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
    prStringReset(&media->currentSrc);
    media->currentSrc = prUrlSerialize(url, PrUrlSerialization_Whole);
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
    PrString request = prUrlSerialize(url, PrUrlSerialization_ExcludeFragment);
    DomFetchRequest asked{
        prStringView(&request), DomFetchDestination_Media,
        domCorsSettings(domAttributeValue(media->element, "crossorigin")),
    };
    host->fetch(host->context, &asked, candidateFetched, fetch, dropTask);
    prStringReset(&request);
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
        if (type != nullptr && htmlCanPlayType(prStringView(&type->value)) == PrHtmlCanPlay_No)
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
        PrUrl* url = candidateUrl(media, prStringView(&src->value));
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
// (html/#await-a-stable-state).
void resourceSelection(HtmlMedia* media) {
    media->networkState = PrHtmlMediaNetworkState_NoSource;
    MediaTask* task = newTask(media);
    const DomDocumentHost* host = media->element->document->host;
    if (!host->queueMicrotask(host->context, selectResourceTask, task))
        dropTask(task);
}

// html/#internal-play-steps
void internalPlay(HtmlMedia* media) {
    if (media->networkState == PrHtmlMediaNetworkState_Empty)
        resourceSelection(media);
    if (htmlMediaEnded(media))
        htmlMediaSeek(media, 0);
    if (media->paused) {
        media->paused = false;
        media->since = prTimeMonotonicMilliseconds();
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

// Frees the state, which no task or timer holds, and removes it from the window's list.
void destroyMedia(HtmlMedia* media) {
    PrHtmlWindow* window = media->window;
    for (uint32_t i = 0; i < window->mediaElements.count; ++i) {
        if (window->mediaElements.data[i] == media) {
            window->mediaElements.erase(i);
            break;
        }
    }
    releasePromises(&media->promises);
    media->played.reset();
    prStringReset(&media->currentSrc);
    prStringReset(&media->errorMessage);
    prMemoryFree(media);
}

// Frees the element's state along with the element.
void forgetMedia(void* data) {
    destroyMedia(static_cast<HtmlMedia*>(data));
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

HtmlMedia* PR_NULLABLE htmlMediaFromElement(PrDomElement* element) {
    PrHtmlWindow* window = static_cast<PrHtmlWindow*>(element->document->global);
    if (window == nullptr || window->document != element->document || element->document->host == nullptr)
        return nullptr;
    DomElementState* state = domElementState(element);
    if (state->forgetEmbedded == forgetMedia)
        return static_cast<HtmlMedia*>(state->embedded);
    HtmlMedia* media = allocZeroed<HtmlMedia>();
    media->element = element;
    state->embedded = media;
    state->forgetEmbedded = forgetMedia;
    media->window = window;
    media->currentSrc = prStringEmpty();
    media->errorMessage = prStringEmpty();
    media->duration = kNanF64;
    media->defaultRate = 1;
    media->rate = 1;
    media->volume = 1;
    media->paused = true;
    media->autoplaying = true;
    media->preservesPitch = true;
    window->mediaElements.push(media);
    updateFlags(media);
    return media;
}

void htmlMediaReset(PrHtmlWindow* window) {
    // The window's tasks and timers are gone, and with them the references that held the elements.
    while (window->mediaElements.count != 0) {
        HtmlMedia* media = window->mediaElements.last();
        DomElementState* state = media->element->state;
        state->media = 0;
        state->embedded = nullptr;
        state->forgetEmbedded = nullptr;
        destroyMedia(media);
    }
    window->mediaElements.reset();
}

void htmlMediaChanged(PrDomElement* element, PrStringView attribute) {
    bool connection = attribute.data == nullptr;
    if (!connection && attribute != "src" && attribute != "muted")
        return;
    HtmlMedia* media = htmlMediaFromElement(element);
    if (media == nullptr)
        return;
    if (attribute == "muted") {
        updateFlags(media);
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
        media->duration = kNanF64;
        media->videoWidth = 0;
        media->videoHeight = 0;
    }
    media->rate = media->defaultRate;
    media->error = PrHtmlMediaError_None;
    prStringReplace(&media->errorMessage, "");
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
    if (!isNanF64(media->duration) && seconds > media->duration)
        seconds = media->duration;
    if (seconds < 0)
        seconds = 0;
    media->seeking = true;
    MediaTask* start = newTask(media);
    start->event = "seeking";
    queueTask(start);
    media->position = seconds;
    media->since = prTimeMonotonicMilliseconds();
    // The media data is at the new position once the task that follows the seeking event runs.
    MediaTask* done = newTask(media);
    done->seekCount = ++media->seeks;
    done->run = [](MediaTask* self) {
        // If a later seek has started, its own task ends the seeking instead.
        HtmlMedia* seeked = self->media;
        if (self->seekCount != seeked->seeks)
            return;
        seeked->seeking = false;
        seeked->since = prTimeMonotonicMilliseconds();
        updateFlags(seeked);
        domEventFire(seeked->element, "timeupdate");
        domEventFire(seeked->element, "seeked");
        schedule(seeked);
    };
    queueTask(done);
    updateFlags(media);
    schedule(media);
}

bool htmlMediaEnded(HtmlMedia* media) {
    // html/#ended-playback, playing forwards.
    return media->readyState >= PrHtmlMediaReadyState_HaveMetadata && !isNanF64(media->duration) &&
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
}

PrHtmlCanPlay htmlCanPlayType(PrStringView type) {
    PrStringView essence = domMimeTypeEssence(type);
    // The engine reads the duration from an MP4 container, and plays the timeline of any codec in one.
    if (asciiEqualsFold(essence, "video/mp4") || asciiEqualsFold(essence, "audio/mp4"))
        return PrHtmlCanPlay_Maybe;
    return PrHtmlCanPlay_No;
}

} // namespace pr_impl
