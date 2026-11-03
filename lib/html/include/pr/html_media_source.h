// Media Source Extensions: MediaSource, SourceBuffer and SourceBufferList (media-source-2/).
//
// A media source is a source of media data that scripts append to, which a media element plays once its object URL is
// the element's source. Each call that the IDL lets throw returns a status for the exception: InvalidState for
// InvalidStateError, NotSupported for NotSupportedError, Limit for QuotaExceededError, NotFound for NotFoundError, and
// OutOfRange for a TypeError, such as a negative duration.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/dom.h"
#include "pr/html.h"
#include "pr/media_source.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A MediaSource and a SourceBuffer are reference counted through their event targets. A SourceBufferList is part of its
// MediaSource, and a reference to it is a reference to the MediaSource.
typedef struct PrHtmlMediaSource PrHtmlMediaSource;
typedef struct PrHtmlSourceBuffer PrHtmlSourceBuffer;
typedef struct PrHtmlSourceBufferList PrHtmlSourceBufferList;

// Each as the interface it inherits from.
PR_HANDLE_UPCAST(HtmlMediaSource, DomEventTarget, EventTarget)
PR_HANDLE_UPCAST(HtmlSourceBuffer, DomEventTarget, EventTarget)
PR_HANDLE_UPCAST(HtmlSourceBufferList, DomEventTarget, EventTarget)

// media-source-2/#dom-readystate
enum {
    PrHtmlMediaSourceReadyState_Closed,
    PrHtmlMediaSourceReadyState_Open,
    PrHtmlMediaSourceReadyState_Ended,
};
typedef uint8_t PrHtmlMediaSourceReadyState;

// media-source-2/#dom-endofstreamerror, and None for an end without an error.
enum {
    PrHtmlEndOfStreamError_None,
    PrHtmlEndOfStreamError_Network,
    PrHtmlEndOfStreamError_Decode,
};
typedef uint8_t PrHtmlEndOfStreamError;

// Creates a closed media source whose events are tasks of `window` (media-source-2/#dom-mediasource-constructor). The
// caller holds a reference.
PrHtmlMediaSource* prHtmlMediaSourceCreate(PrHtmlWindow* window);

// Return `object` as each interface, or null if it is not one.
PrHtmlMediaSource* PR_NULLABLE prHtmlMediaSourceQuery(PrDomEventTarget* PR_NULLABLE object);
PrHtmlSourceBuffer* PR_NULLABLE prHtmlSourceBufferQuery(PrDomEventTarget* PR_NULLABLE object);
PrHtmlSourceBufferList* PR_NULLABLE prHtmlSourceBufferListQuery(PrDomEventTarget* PR_NULLABLE object);

// Returns true if a SourceBuffer of the MIME type `type` can be created in the window: the type names a byte stream
// format that the engine parses, and the window's media codec reads its codecs
// (media-source-2/#dom-mediasource-istypesupported ).
bool prHtmlMediaSourceIsTypeSupported(const PrHtmlWindow* window, PrStringView type);

// Creates a blob URL that names the media source, for the window's origin, which lasts until it is revoked or the
// window closes (media-source-2/#mediasource-object-url). No fetch reads it.
PrString prHtmlMediaSourceUrlCreate(PrHtmlWindow* window, PrHtmlMediaSource* source);

// The source's state: closed until an element attaches it, open while script appends, and ended once script says that
// it has appended everything (media-source-2/#dom-mediasource-readystate).
PrHtmlMediaSourceReadyState prHtmlMediaSourceReadyState(const PrHtmlMediaSource* source);

// The duration in seconds, NaN while the source is closed or before an initialization segment gives one
// (media-source-2/#dom-mediasource-duration).
double prHtmlMediaSourceDuration(const PrHtmlMediaSource* source);
PrStatus prHtmlMediaSourceSetDuration(PrHtmlMediaSource* source, double duration);

// The source's SourceBuffers, and those of them that supply the tracks that the element plays.
PrHtmlSourceBufferList* prHtmlMediaSourceSourceBuffers(PrHtmlMediaSource* source);
PrHtmlSourceBufferList* prHtmlMediaSourceActiveSourceBuffers(PrHtmlMediaSource* source);

// Creates a SourceBuffer of `type` and adds it to the source (media-source-2/#dom-mediasource-addsourcebuffer). On
// success, the caller holds a reference to `*out`.
PrStatus prHtmlMediaSourceAddSourceBuffer(PrHtmlMediaSource* source, PrStringView type,
    PrHtmlSourceBuffer* PR_NULLABLE* PR_NONNULL out);

// media-source-2/#dom-mediasource-removesourcebuffer
PrStatus prHtmlMediaSourceRemoveSourceBuffer(PrHtmlMediaSource* source, PrHtmlSourceBuffer* buffer);

// media-source-2/#dom-mediasource-endofstream
PrStatus prHtmlMediaSourceEndOfStream(PrHtmlMediaSource* source, PrHtmlEndOfStreamError error);

// media-source-2/#dom-mediasource-setliveseekablerange and media-source-2/#dom-mediasource-clearliveseekablerange
PrStatus prHtmlMediaSourceSetLiveSeekableRange(PrHtmlMediaSource* source, double start, double end);
PrStatus prHtmlMediaSourceClearLiveSeekableRange(PrHtmlMediaSource* source);

// The list's length, and its item at `index`, which must be less than the length.
uint32_t prHtmlSourceBufferListLength(const PrHtmlSourceBufferList* list);
PrHtmlSourceBuffer* prHtmlSourceBufferListItem(PrHtmlSourceBufferList* list, uint32_t index);

// The append mode (media-source-2/#dom-sourcebuffer-mode).
PrMediaAppendMode prHtmlSourceBufferMode(const PrHtmlSourceBuffer* buffer);
PrStatus prHtmlSourceBufferSetMode(PrHtmlSourceBuffer* buffer, PrMediaAppendMode mode);

// Returns true while an append or a removal runs (media-source-2/#dom-sourcebuffer-updating).
bool prHtmlSourceBufferIsUpdating(const PrHtmlSourceBuffer* buffer);

// Writes up to `capacity` buffered ranges to `out`, and their number to `*count`
// (media-source-2/#dom-sourcebuffer-buffered ). Returns InvalidState for a buffer removed from its source.
PrStatus prHtmlSourceBufferBuffered(const PrHtmlSourceBuffer* buffer, PrMediaTimeRange* PR_NULLABLE out,
    uint32_t capacity, uint32_t* count);

// media-source-2/#dom-sourcebuffer-timestampoffset, appendwindowstart and appendwindowend
double prHtmlSourceBufferTimestampOffset(const PrHtmlSourceBuffer* buffer);
PrStatus prHtmlSourceBufferSetTimestampOffset(PrHtmlSourceBuffer* buffer, double offset);
double prHtmlSourceBufferAppendWindowStart(const PrHtmlSourceBuffer* buffer);
PrStatus prHtmlSourceBufferSetAppendWindowStart(PrHtmlSourceBuffer* buffer, double start);
double prHtmlSourceBufferAppendWindowEnd(const PrHtmlSourceBuffer* buffer);
PrStatus prHtmlSourceBufferSetAppendWindowEnd(PrHtmlSourceBuffer* buffer, double end);

// Starts appending a copy of `bytes`, which ends in tasks (media-source-2/#dom-sourcebuffer-appendbuffer).
PrStatus prHtmlSourceBufferAppendBuffer(PrHtmlSourceBuffer* buffer, PrByteView bytes);

// media-source-2/#dom-sourcebuffer-abort
PrStatus prHtmlSourceBufferAbort(PrHtmlSourceBuffer* buffer);

// Starts removing the frames that present from `start` to `end`, which ends in tasks
// (media-source-2/#dom-sourcebuffer-remove).
PrStatus prHtmlSourceBufferRemove(PrHtmlSourceBuffer* buffer, double start, double end);

// Lets the next initialization segment change the buffer's codecs to those of `type`
// (media-source-2/#dom-sourcebuffer-changetype).
PrStatus prHtmlSourceBufferChangeType(PrHtmlSourceBuffer* buffer, PrStringView type);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
