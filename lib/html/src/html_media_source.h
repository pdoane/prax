// Media Source Extensions: MediaSource, SourceBuffer and SourceBufferList, and how a media element plays a media source
// (media-source-2/#htmlmediaelement-extensions).

#pragma once

#include "pr/html_media_source.h"
#include "pr/media_source.h"
#include "pr/raw_array.h"

#include "dom/src/dom_event.h"
#include "html/src/html_media.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A SourceBufferList (media-source-2/#sourcebufferlist). It holds a reference to each of its buffers.
struct HtmlSourceBufferList : DomPartTarget {
    RawArray<PrHtmlSourceBuffer*> buffers;
};

// What a SourceBuffer is doing between updatestart and updateend.
enum {
    HtmlSourceUpdate_None,
    HtmlSourceUpdate_Append,
    HtmlSourceUpdate_Remove,
};
typedef uint8_t HtmlSourceUpdate;

// A SourceBuffer (media-source-2/#sourcebuffer). `source` is the media source that it belongs to, without a counted
// reference, and null once removed from it.
struct HtmlSourceBuffer : DomCountedTarget {
    PrHtmlWindow* window;
    PrHtmlMediaSource* PR_NULLABLE source;
    PrMediaSourceBuffer* PR_NULLABLE buffer;
    HtmlSourceUpdate update;
    uint32_t updates;               // the number of updates started, so that a task of an aborted one does nothing
    RawArray<uint8_t> input;        // the bytes of the append in progress
    double removeStart;
    double removeEnd;
    bool firstInit;                 // media-source-2/#dfn-first-initialization-segment-received-flag
    bool hasAudio;
    bool hasVideo;
    PrUVec2 videoExtent;
};

// A MediaSource (media-source-2/#mediasource). `media` is the state of the element that it is attached to, without a
// counted reference; the element holds the source.
struct HtmlMediaSource : DomCountedTarget {
    PrHtmlWindow* window;
    PrHtmlMediaSourceReadyState readyState;
    double duration;
    double liveStart;               // the live seekable range, or NaN for none
    double liveEnd;
    HtmlSourceBufferList sourceBuffers;
    HtmlSourceBufferList activeSourceBuffers;
    HtmlMedia* PR_NULLABLE media;
};

// Returns the media source that the window's `url` names, or null.
PrHtmlMediaSource* PR_NULLABLE htmlMediaSourceFromUrl(PrHtmlWindow* window, PrStringView url);

// Releases the window's object URLs of media sources. Called when the window closes.
void htmlMediaSourceUrlsReset(PrHtmlWindow* window);

// Revokes `url`, if it names one of the window's media sources.
void htmlMediaSourceRevokeUrl(PrHtmlWindow* window, PrStringView url);

// Attaches the source to the media element (media-source-2/#mediasource-attach). Returns false if the source is not
// closed, which fails the element's load. The element takes a reference to the source.
bool htmlMediaSourceAttach(PrHtmlMediaSource* source, HtmlMedia* media);

// Detaches the source from its element (media-source-2/#mediasource-detach), and releases the element's reference.
void htmlMediaSourceDetach(PrHtmlMediaSource* source);

// Writes the ranges that the element has buffered (media-source-2/#htmlmediaelement-extensions-buffered).
void htmlMediaSourceBuffered(const PrHtmlMediaSource* source, RawArray<PrMediaTimeRange>* out);

// Writes the ranges that the element can seek to (media-source-2/#htmlmediaelement-extensions-seekable).
void htmlMediaSourceSeekable(const PrHtmlMediaSource* source, RawArray<PrMediaTimeRange>* out);

// Returns true if the source has ended, so that the element plays to the end of what is buffered.
bool htmlMediaSourceHasEnded(const PrHtmlMediaSource* source);

// Writes up to `capacity` of the track buffers of the source's active SourceBuffers to `out`, in order, and returns
// their number.
uint32_t htmlMediaSourceActiveBuffers(const PrHtmlMediaSource* source, PrMediaSourceBuffer* PR_NONNULL* PR_NONNULL out,
    uint32_t capacity);

} // namespace pr_impl

// C opaque types
struct PrHtmlMediaSource : pr_impl::HtmlMediaSource {};
struct PrHtmlSourceBuffer : pr_impl::HtmlSourceBuffer {};
struct PrHtmlSourceBufferList : pr_impl::HtmlSourceBufferList {};

PR_ASSUME_NONNULL_END
