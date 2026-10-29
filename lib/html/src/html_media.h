// Media Elements
//
// html - https://html.spec.whatwg.org/multipage/media.html
//
// A window runs the media elements of its document. The window's clock advances the playback of each element along its
// media timeline. The engine decodes no media data, and represents a resource by its container's metadata
// (PrHtmlMediaElement).

#pragma once

#include "pr/array.h"
#include "pr/dom.h"
#include "pr/html.h"
#include "pr/string_view.h"

#include "html/src/html_window.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The state of a media element (html/#media-element). The element owns the state, and the window runs it. A task or
// timer of the state holds the element (domNodeHold) until the task or timer is done. Otherwise the state does not hold
// the element. The state is destroyed with the element or with the window's document.
struct HtmlMedia {
    PrDomElement* element;
    PrHtmlWindow* window;
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
};

// Returns the media element's state, which its window creates when it is first needed. Returns null for an element that
// is not in its window's document.
HtmlMedia* PR_NULLABLE htmlMediaFromElement(PrDomElement* element);

// Forgets every media element of the window. Called when the window's document goes away.
void htmlMediaReset(PrHtmlWindow* window);

// Handles a change to a media element: its connection, if `attribute` is the null view, or a change of that attribute
// (html/#location-of-the-media-resource, html/#playing-the-media-resource).
void htmlMediaChanged(PrDomElement* element, PrStringView attribute);

// The algorithms of the public API (PrHtmlMediaElement), for an element in its window's document.
void htmlMediaLoad(HtmlMedia* media);
double htmlMediaCurrentTime(HtmlMedia* media);
void htmlMediaSeek(HtmlMedia* media, double seconds);
bool htmlMediaEnded(HtmlMedia* media);
void htmlMediaPlay(HtmlMedia* media, const PrHtmlPlayPromise& promise);
void htmlMediaPause(HtmlMedia* media);
void htmlMediaSetRate(HtmlMedia* media, double rate, bool isDefault);
void htmlMediaSetVolume(HtmlMedia* media, double volume, bool muted);

// Returns how likely the engine is to play a resource of the MIME type `type` (html/#dom-navigator-canplaytype).
PrHtmlCanPlay htmlCanPlayType(PrStringView type);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
