// Event targets: the base of every object that events are dispatched to.
//
// dom - https://dom.spec.whatwg.org/

#pragma once

#include "pr/dom.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct DomListeners;

// The type of an event target. A node's type is its PrDomNodeType (dom/#interface-node). The other types follow every
// node type, so that a range check tells a node from any other target (domNodeFromTarget). A type of a part of another
// target (DomPartTarget) has DomTargetTypeFlag_Part, and one of a reference-counted target (DomCountedTarget) has
// DomTargetTypeFlag_Counted, so that references to them need no list of the types.
enum {
    DomTargetTypeFlag_Part = 0x1000,
    DomTargetTypeFlag_Counted = 0x2000,

    DomTargetType_EventTarget = 0,          // a standalone EventTarget (dom/#interface-eventtarget)
    DomTargetType_FontFaceSet = 0x100,      // a FontFaceSet (css-font-loading-3/#fontfaceset)
    DomTargetType_PseudoElement,            // a pseudo-element that a presentation generates (CssPresentationPseudo)
    DomTargetType_AbortSignal,              // an AbortSignal (dom/#interface-AbortSignal)

    // Reference-counted targets: a Window (html/#the-window-object), a MediaQueryList
    // (cssom-view-1/#the-mediaquerylist-interface), an XMLHttpRequest (xhr/#interface-xmlhttprequest) and a MessagePort
    // (html/#message-ports).
    DomTargetType_Window = DomTargetTypeFlag_Counted | 0x100,
    DomTargetType_MediaQueryList,
    DomTargetType_XmlHttpRequest,
    DomTargetType_MessagePort,

    // Parts of other targets: a PreferenceObject (mediaqueries-5/#preferenceobject), a Performance
    // (hr-time-3/#sec-performance), a ScreenOrientation (screen-orientation/#screenorientation-interface), an
    // XMLHttpRequestUpload (xhr/#xmlhttprequestupload), a TextTrackList (html/#texttracklist), an AudioTrackList
    // (html/#audiotracklist) and a VideoTrackList (html/#videotracklist).
    DomTargetType_Preference = DomTargetTypeFlag_Part | 0x100,
    DomTargetType_Performance,
    DomTargetType_ScreenOrientation,
    DomTargetType_XmlHttpRequestUpload,
    DomTargetType_TextTrackList,
    DomTargetType_AudioTrackList,
    DomTargetType_VideoTrackList,
};
typedef uint16_t DomTargetType;

// The base of every object that events are dispatched to (dom/#interface-eventtarget).
struct DomEventTarget {
    DomListeners* PR_NULLABLE listeners;   // created when the first listener is added
    void* PR_NULLABLE binding;             // data that the script binding keeps for it (prDomEventTargetBinding)
    DomTargetType type;                    // a node's PrDomNodeType, or a DomTargetType_ value
};

} // namespace pr_impl

// C opaque types
struct PrDomEventTarget : pr_impl::DomEventTarget {};

PR_ASSUME_NONNULL_END
