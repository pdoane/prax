// Media Elements
//
// html - https://html.spec.whatwg.org/multipage/media.html
//
// A window runs the media elements of its document. The window's clock advances the playback of each element along its
// media timeline, and the host's media codec reads each resource and decodes the frames of its video
// (PrHtmlHost::media). A video element shows the frame at its playback position, or its poster frame
// (html/#the-video-element), as the picture of its element state, which each rendering update brings up to date. The
// host's mixer plays each resource's audio (PrHtmlHost::mixer), and an element's timeline follows that audio while it
// plays. An element's state lives as long as the element. A window runs it while the element's document is the window's
// document, and the state returns to its initial playback state when no window runs it.

#pragma once

#include "pr/array.h"
#include "pr/audio_mixer.h"
#include "pr/dom.h"
#include "pr/draw_image.h"
#include "pr/html.h"
#include "pr/html_media_source.h"
#include "pr/media.h"
#include "pr/string_view.h"

#include "dom/src/dom_event.h"
#include "html/src/html_window.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A list of a media element's tracks of one kind, which the type of the target names: a TextTrackList
// (html/#texttracklist), an AudioTrackList (html/#audiotracklist) or a VideoTrackList (html/#videotracklist). The list
// is a part of its element (DomPartTarget).
struct HtmlTrackList : DomPartTarget {};

// The state of a media element (html/#media-element). The element owns the state, which is created when it is first
// needed and destroyed with the element. A task or timer of the state holds the element (domNodeHold) until the task or
// timer is done. Otherwise the state does not hold the element.
struct HtmlMedia {
    PrDomElement* element;
    PrHtmlWindow* PR_NULLABLE window;    // the window that runs the element, or null while none does
    HtmlTrackList textTracks;
    HtmlTrackList audioTracks;
    HtmlTrackList videoTracks;
    PrString currentSrc;                 // html/#dom-media-currentsrc
    PrString errorMessage;               // html/#dom-mediaerror-message
    Array<PrHtmlPlayPromise> promises;   // html/#list-of-pending-play-promises
    Array<double> played;                // html/#dom-media-played: each range's start and end, in order
    double position;                     // html/#official-playback-position, in seconds, at `since`
    double since;                        // the value of prTimeMonotonicMilliseconds at which `position` was taken
    double duration;                     // NaN if it is not known
    double defaultRate;
    double rate;
    double volume;
    uint32_t videoWidth;
    uint32_t videoHeight;
    uint32_t load;                       // the number of loads, so that tasks of an earlier load are dropped
    uint32_t seeks;                      // the number of seeks, so that only the latest seek's task ends seeking

    int32_t timer;                       // the window timer of the next timeupdate or end, or zero
    PrHtmlMediaNetworkState networkState;
    PrHtmlMediaReadyState readyState;
    PrHtmlMediaError error;
    bool paused;
    bool seeking;
    bool muted;                          // the value that the muted IDL attribute last set
    bool mutedSet;                       // true once a script sets muted; the content attribute then no longer applies
    bool autoplaying;                    // html/#can-autoplay-flag
    bool preservesPitch;
    bool loadedData;                     // true once loadeddata has fired, until the next load

    // What a video element shows: the decoded frame and the poster frame (html/#poster-frame), each as an image, the
    // serial of the frame (PrMediaFrame::serial), and the generation of the poster's fetch, so that the result of an
    // earlier fetch is dropped.
    PrMediaDecoder* PR_NULLABLE decoder;     // the resource of the current load, or null
    PrAudioSource* PR_NULLABLE source;       // plays the audio of `decoder`, or null if nothing does

    // The media source that the element plays, which it holds, or null for a fetched resource. Playback stops at
    // `playableEnd`, the end of the buffered range at the position, until more data arrives.
    PrHtmlMediaSource* PR_NULLABLE mediaSource;
    double playableEnd;
    bool seekWaits;                          // a seek waits for the media source to buffer its position
    const PrDrawImage* PR_NULLABLE frame;
    const PrDrawImage* PR_NULLABLE poster;
    uint64_t frameSerial;
    uint32_t posters;
    bool showPoster;                     // html/#show-poster-flag

    // The picture is out of date: the frame or poster frame that the element shows may not be the one that shows at its
    // playback position. A playing video's picture goes out of date as its position advances, which htmlMediaWantsFrame
    // accounts for.
    bool stale;
};

// Returns the media element's state, creating it if the element has none.
HtmlMedia* htmlMediaFromElement(PrDomElement* element);

// Returns the media element's state if a window runs it: the window whose document is the element's document, which
// takes the state from any other window that ran it. Returns null if no window runs the element.
HtmlMedia* PR_NULLABLE htmlMediaRunning(PrDomElement* element);

// Stops running every media element of the window. Called when the window's document goes away.
void htmlMediaReset(PrHtmlWindow* window);

// Returns true if a video element of the window shows a picture that is out of date: a frame or a poster frame that it
// has yet to show, which the next rendering update brings up to date if it is decoded by then.
bool htmlMediaAwaitsPicture(const PrHtmlWindow* window);

// Returns true if a rendering update would change what a video element of the window shows: if its picture is out of
// date, or it plays.
bool htmlMediaWantsFrame(const PrHtmlWindow* window);

// Brings the picture of each video element of the window up to date, as part of a rendering update
// (html/#update-the-rendering).
void htmlMediaShow(PrHtmlWindow* window);

// Handles a change to a media element: its connection, if `attribute` is the null view, or a change of that attribute
// (html/#location-of-the-media-resource, html/#playing-the-media-resource).
void htmlMediaChanged(PrDomElement* element, PrStringView attribute);

// The algorithms of the public API (PrHtmlMediaElement), for an element that a window runs.
void htmlMediaLoad(HtmlMedia* media);
double htmlMediaCurrentTime(HtmlMedia* media);
void htmlMediaSeek(HtmlMedia* media, double seconds);
bool htmlMediaEnded(HtmlMedia* media);
void htmlMediaPlay(HtmlMedia* media, const PrHtmlPlayPromise& promise);
void htmlMediaPause(HtmlMedia* media);
void htmlMediaSetRate(HtmlMedia* media, double rate, bool isDefault);
void htmlMediaSetVolume(HtmlMedia* media, double volume, bool muted);
void htmlMediaSetPreservesPitch(HtmlMedia* media, bool preserves);

// The element's side of a media source (media-source-2/#htmlmediaelement-extensions). Its first initialization segments
// give the element its metadata and the natural size of `videoExtent`; a duration change fires durationchange; a change
// to what is buffered, or the end of the stream, updates the ready state and ends a seek that waits for data; and an
// end of stream with an error fails the element with `error`.
void htmlMediaSourceMetadata(HtmlMedia* media, PrUVec2 videoExtent);
void htmlMediaSourceDuration(HtmlMedia* media, double duration);
void htmlMediaSourceChanged(HtmlMedia* media);
void htmlMediaSourceFailed(HtmlMedia* media, PrHtmlMediaError error);

// Drops the element's decoder of its media source, before the source's active buffers change. The next update makes it
// again from the buffers then active.
void htmlMediaSourceTracksChanging(HtmlMedia* media);

// Returns true if the element's error attribute is set, which stops appends to its media source.
bool htmlMediaSourceHasError(const HtmlMedia* media);

// Returns how likely the window's codec is to play a resource of the MIME type `type`
// (html/#dom-navigator-canplaytype). No resource plays without a window or a codec.
PrHtmlCanPlay htmlCanPlayType(const PrHtmlWindow* PR_NULLABLE window, PrStringView type);

} // namespace pr_impl

// C opaque types
struct PrHtmlTrackList : pr_impl::HtmlTrackList {};

PR_ASSUME_NONNULL_END
