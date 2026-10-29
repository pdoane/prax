// Focus
//
// html - https://html.spec.whatwg.org/multipage/interaction.html#focus

#pragma once

#include "pr/base.h"
#include "pr/dom.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if the element is a focusable area (html/#focusable-area). With a presentation, the element must be
// rendered and interactive (cssPresentationIsInteractive), which brings the presentation's styles up to date. Without a
// presentation, an element counts as rendered unless it or an ancestor has the hidden or the inert attribute.
bool htmlFocusable(const PrDomElement* element);

// Returns the tabindex attribute if it parses as an integer, and otherwise the default for the element's kind
// (html/#dom-tabindex).
int32_t htmlTabIndex(const PrDomElement* element);

// How a focus change decides whether the focused element shows that it has focus
// (selectors-4/#the-focus-visible-pseudo).
enum {
    HtmlFocusIndication_Shown,
    HtmlFocusIndication_Hidden,

    // Shown on an element that takes text input. Otherwise shown if the document's last input was not a press, and the
    // element that had focus showed it or no element had focus. focus() without FocusOptions' focusVisible decides this
    // way.
    HtmlFocusIndication_Script,

    // Shown only on an element that takes text input, which a press focuses for typing as a key would.
    HtmlFocusIndication_Press,
};
typedef uint8_t HtmlFocusIndication;

// The direction of sequential focus navigation (html/#sequential-focus-direction).
enum {
    HtmlFocusDirection_Forward,
    HtmlFocusDirection_Backward,
};
typedef uint8_t HtmlFocusDirection;

// Moves the document's focus to the element, or to the focus delegate of its shadow root, if that is a focusable area
// (html/#focusing-steps). A host whose shadow root delegates focus keeps it if the focus is already within it. Each
// iframe around the element's document takes the focus in the document that holds it. Returns true if the element or
// its delegate is a focusable area.
bool htmlFocus(PrDomElement* element, HtmlFocusIndication indication);

// If the element has its document's focus, moves the focus to the viewport (html/#dom-blur).
void htmlBlur(PrDomElement* element);

// Moves the document's focus to the viewport if the focused element is no longer a focusable area, firing blur and
// focusout at it (html/#update-the-rendering's focus fixup). Run after the document's styles are brought up to date.
void htmlFocusFixup(PrDomDocument* document);

// Runs the focus processing of a press on `pressed` (html/#focus-processing-model): focuses the nearest focusable
// inclusive ancestor of the element, or moves the focus to the viewport if there is none. The press becomes the
// sequential focus navigation starting point. Run after a mousedown that no listener canceled.
void htmlFocusPress(PrDomElement* pressed);

// Runs the focus steps of scrolling to the fragment `target` (html/#scroll-to-the-fragment-identifier): focuses the
// element if it is a focusable area, and otherwise moves the focus to the viewport. The element becomes the sequential
// focus navigation starting point either way.
void htmlFocusFragment(PrDomElement* target);

// Returns true if the top-level document or the document of a frame within it has autofocus candidates to flush.
bool htmlFocusHasAutofocus(PrDomDocument* top);

// Flushes the autofocus candidates of a top-level document and of its frames' documents
// (html/#flush-autofocus-candidates). It focuses the first candidate that is a focusable area, or the area it delegates
// to, and shows its focus. Once a candidate is focused, or the document already has a focused element or a target, the
// document's autofocus is processed and no candidate is focused again. A candidate in a document whose scripts a style
// sheet blocks waits for the sheet, with the candidates after it.
void htmlFocusFlushAutofocus(PrDomDocument* top);

// Ends the autofocus of the top-level document around `document`, if the two are same origin: no candidate is focused
// from then on. The dialog focusing steps end it (html/#dialog-focusing-steps).
void htmlFocusEndAutofocus(PrDomDocument* document);

// Records a key pressed in the document. The focused element shows its focus from then on. A modifier key alone is not
// recorded.
void htmlFocusKey(PrDomDocument* document);

// Moves the focus to the next or previous suitable sequentially focusable area (html/#sequential-focus-navigation),
// from the starting point in `document`'s focused area, and shows it. The search enters the documents of iframes in
// order, and leaves them for the documents around them. Returns false if no area follows in `direction`, and then moves
// the focus to the viewport, so that the next navigation starts again at the first or last area.
bool htmlFocusNavigate(PrDomDocument* document, HtmlFocusDirection direction);

// Returns the element that key input goes to: the focused element of the document's focused iframe, through each nested
// one, or else the document's focused element, or else its body or its document element (uievents/#event-type-keydown).
PrDomElement* PR_NULLABLE htmlKeyTarget(PrDomDocument* document);

// Returns the focused element as a document or a shadow root sees it (html/#dom-documentorshadowroot-activeelement).
PrDomElement* PR_NULLABLE htmlActiveElement(PrDomNode* root);

// Returns true if the document has focus (html/#has-focus-steps). The top-level browsing context is taken to have the
// system focus.
bool htmlHasFocus(const PrDomDocument* document);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
