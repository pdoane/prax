// Event targets: the base of every object that events are dispatched to.
//
// dom - https://dom.spec.whatwg.org/

#pragma once

#include "pr/dom.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct DomListeners;

// The type of an event target. A node's type is its PrDomNodeType (dom/#interface-node). The other types follow every
// node type, so that a range check tells a node from any other target (domNodeFromTarget).
enum {
    DomTargetType_EventTarget = 0,          // a standalone EventTarget (dom/#interface-eventtarget)
    DomTargetType_Window = 0x100,           // a Window (html/#the-window-object)
    DomTargetType_MediaQueryList,           // a MediaQueryList (cssom-view-1/#the-mediaquerylist-interface)
    DomTargetType_FontFaceSet,              // a FontFaceSet (css-font-loading-3/#fontfaceset)
    DomTargetType_Preference,               // a PreferenceObject (mediaqueries-5/#preferenceobject)
    DomTargetType_Performance,              // a Performance (hr-time-3/#sec-performance)
    DomTargetType_ScreenOrientation,        // a ScreenOrientation (screen-orientation/#screenorientation-interface)
    DomTargetType_PseudoElement,            // a pseudo-element that a presentation generates (CssPresentationPseudo)
    DomTargetType_AbortSignal,              // an AbortSignal (dom/#interface-AbortSignal)
    DomTargetType_XmlHttpRequest,           // an XMLHttpRequest (xhr/#interface-xmlhttprequest)
    DomTargetType_XmlHttpRequestUpload,     // an XMLHttpRequestUpload (xhr/#xmlhttprequestupload)
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
