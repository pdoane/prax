#include "html/src/html_focus.h"

#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/sort.h"
#include "pr/string_ops.h"

#include "css/src/css_hints.h"
#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_root.h"
#include "dom/src/dom_attribute.h"
#include "dom/src/dom_collection.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_event.h"
#include "dom/src/dom_microsyntax.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_slot.h"
#include "html/src/html_window.h"

namespace pr_impl {
namespace {

// Writes the tabindex attribute to `out`, clamped to the range of an int32_t. Returns false if the attribute does not
// parse as an integer.
bool tabIndexAttribute(const PrDomElement* element, int32_t* out) {
    int64_t value = 0;
    if (!domParseInteger(domAttributeValue(element, "tabindex"), &value))
        return false;
    *out = int32_t(value < INT32_MIN ? INT32_MIN : value > INT32_MAX ? INT32_MAX : value);
    return true;
}

// Returns true if the element, or a shadow-including ancestor, is hidden or inert (html/#inert).
bool hiddenOrInert(const PrDomElement* element) {
    for (const PrDomNode* node = element; node != nullptr; node = domShadowIncludingParent(node)) {
        const PrDomElement* ancestor = domElementFromNode(node);
        if (ancestor != nullptr && ancestor->space == PrDomNamespace_Html &&
                (domAttribute(ancestor, PrDomNamespace_None, "hidden") != nullptr ||
                    domAttribute(ancestor, PrDomNamespace_None, "inert") != nullptr))
            return true;
    }
    return false;
}

// Returns true if the element is the summary for its parent details (html/#summary-for-its-parent-details).
bool isDetailsSummary(const PrDomElement* element) {
    if (!domIsHtml(element, PrDomTag_Summary))
        return false;
    const PrDomElement* parent = domElementFromNode(element->parent);
    if (!domIsHtml(parent, PrDomTag_Details))
        return false;
    for (const PrDomNode* child = parent->first; child != nullptr; child = child->next) {
        const PrDomElement* sibling = domElementFromNode(child);
        if (domIsHtml(sibling, PrDomTag_Summary))
            return sibling == element;
    }
    return false;
}

// Returns true if the element is focusable by its kind (html/#focusable-area), regardless of its tabindex.
bool focusableKind(const PrDomElement* element) {
    // An SVG a element that is a hyperlink (SVG2/interact.html#Focus).
    if (element->space == PrDomNamespace_Svg)
        return element->tag == PrDomTag_A && domElementLink(element);
    if (element->space != PrDomNamespace_Html)
        return false;
    switch (element->tag) {
        case PrDomTag_A:
        case PrDomTag_Area:
            return domAttribute(element, PrDomNamespace_None, "href") != nullptr;
        case PrDomTag_Input:
            return !domElementDisabled(element) && domInputType(domAttributeValue(element, "type")) !=
                PrDomInputType_Hidden;
        case PrDomTag_Button:
        case PrDomTag_Select:
        case PrDomTag_Textarea:
            return !domElementDisabled(element);
        case PrDomTag_Iframe:
            return true;
        case PrDomTag_Audio:
        case PrDomTag_Video:
            return domAttribute(element, PrDomNamespace_None, "controls") != nullptr;
        case PrDomTag_Summary:
            return isDetailsSummary(element);
        default: {
            // An editing host (html/#editing-host).
            DomContentEditable editable = domContentEditable(domAttributeValue(element, "contenteditable"));
            return editable == DomContentEditable_True || editable == DomContentEditable_PlaintextOnly;
        }
    }
}

// Returns the focus delegate (html/#focus-delegate) of a shadow host that delegates focus. This is the first focusable
// area in its shadow tree, in tree order. A host within that tree that delegates focus stands for its own delegate.
PrDomElement* PR_NULLABLE focusDelegate(PrDomElement* host) {
    PrDomShadowRoot* shadow = domShadowRootOfHost(host);
    if (shadow == nullptr || !shadow->delegatesFocus)
        return nullptr;
    for (PrDomNode* node = shadow->first; node != nullptr; node = domNodeNext(shadow, node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element == nullptr)
            continue;
        if (htmlFocusable(element))
            return element;
        if (PrDomElement* delegate = focusDelegate(element))
            return delegate;
    }
    return nullptr;
}

// Fires a focus event named `type` at `target`, with `related` as the related target (html/#fire-a-focus-event).
void fireFocusEvent(PrDomElement* target, PrStringView type, bool bubbles, PrDomElement* PR_NULLABLE related) {
    PrDomEventInit init = prDomEventInitDefault();
    init.bubbles = bubbles;
    init.composed = true;
    PrDomFocusEvent* event = domEventCreate<PrDomFocusEvent>(type, init, PrDomEventKind_Focus);
    event->trusted = true;
    event->view = target->document->global;
    domEventTargetAssign(&event->related, related);
    domEventTargetAssign(&event->relatedTarget, related);
    domEventDispatch(target, event);
    domEventRelease(event);
}

// Moves the sequential focus navigation starting point to `element`.
void setStartingPoint(PrDomDocument* document, PrDomElement* PR_NULLABLE element) {
    document->navigationStart = element;
    document->navigationStartAfter = false;
}

PrDomDocument* PR_NULLABLE frameDocument(const PrDomElement* element);

// Runs html/#focus-update-steps between two focused areas of a document, each of which is an element or the viewport.
// The area that loses focus fires blur and focusout, and the area that gains it fires focus and focusin. The viewport
// has the focus while blur and focusout are dispatched, and a focus change that their listeners make persists. An
// element that gains focus becomes the sequential focus navigation starting point. Focus that moves to the viewport
// leaves the starting point at the element that lost it, so that navigation continues from there.
void focusUpdate(PrDomDocument* document, PrDomElement* PR_NULLABLE old, PrDomElement* PR_NULLABLE focused) {
    // The element that loses focus stops matching the focus pseudo-classes before its listeners run.
    const CssPseudoClass states[] = {CssPseudoClass_Focus, CssPseudoClass_FocusVisible, CssPseudoClass_FocusWithin};
    if (old != nullptr) {
        // An iframe that loses focus takes the focus chain inside it along, innermost first (html/#focus-chain).
        if (PrDomDocument* inner = frameDocument(old)) {
            if (inner->focused != nullptr)
                focusUpdate(inner, inner->focused, nullptr);
        }
        document->focused = nullptr;
        cssPresentationStateMoved(old, nullptr, states, 3);
        fireFocusEvent(old, "blur", false, focused);
        fireFocusEvent(old, "focusout", true, focused);
        if (document->focused != nullptr)
            return;
        // A listener can remove the element that was to gain focus, which leaves the focus at the viewport.
        if (focused != nullptr && (!domIsConnected(focused) || focused->document != document))
            focused = nullptr;
    }
    document->focused = focused;
    if (focused != nullptr)
        setStartingPoint(document, focused);
    cssPresentationStateMoved(nullptr, focused, states, 3);
    if (focused != nullptr) {
        fireFocusEvent(focused, "focus", false, old);
        fireFocusEvent(focused, "focusin", true, old);
    }
}

// Returns true if the element takes text input from the keyboard: an input of a type that is typed into, a textarea, or
// an editing host. Focus that a press moves to such an element shows (selectors-4/#the-focus-visible-pseudo).
bool takesTextInput(const PrDomElement* element) {
    if (domIsHtml(element, PrDomTag_Textarea))
        return true;
    if (domIsHtml(element, PrDomTag_Input)) {
        PrDomInputType type = domInputType(domAttributeValue(element, "type"));
        return type >= PrDomInputType_Text && type <= PrDomInputType_Number;
    }
    DomContentEditable editable = domContentEditable(domAttributeValue(element, "contenteditable"));
    return editable == DomContentEditable_True || editable == DomContentEditable_PlaintextOnly;
}

// Returns true if the element owns a focus navigation scope (html/#focus-navigation-scope-owner): a shadow host or a
// slot. The trigger of a showing popover is not one yet.
bool isScopeOwner(const PrDomElement* element) {
    return domShadowRootOfHost(element) != nullptr || domIsHtml(element, PrDomTag_Slot);
}

// Returns true if sequential navigation focuses the element itself. A slot, and a host whose shadow root delegates
// focus, are passed over for their scopes, whatever their tabindex.
bool focusesItself(const PrDomElement* element) {
    if (domIsHtml(element, PrDomTag_Slot))
        return false;
    PrDomShadowRoot* shadow = domShadowRootOfHost(element);
    return (shadow == nullptr || !shadow->delegatesFocus) && htmlFocusable(element);
}

// Returns true if the element's tabindex value is a negative integer, which keeps it out of sequential navigation.
bool skipsNavigation(const PrDomElement* element) {
    int32_t index = 0;
    return tabIndexAttribute(element, &index) && index < 0;
}

// Returns the focus navigation scope owner that `node` belongs to (html/#associated-focus-navigation-owner): its
// document, a shadow host or a slot. A slot owns its children, its fallback content, as it owns its assigned nodes.
// Returns null for a node that a shadow host's child holds and no slot assigns.
PrDomNode* PR_NULLABLE scopeOwnerOf(PrDomNode* node) {
    for (;;) {
        PrDomNode* parent = domNodeParent(node);
        if (parent == nullptr)
            return nullptr;
        if (PrDomShadowRoot* shadow = domNodeShadowRoot(parent))
            return shadow->host;
        if (parent->type == PrDomNodeType_Document)
            return parent;
        PrDomElement* element = domElementFromNode(parent);
        if (domShadowRootOfHost(element) != nullptr)
            return domAssignedSlot(node);
        if (domIsHtml(element, PrDomTag_Slot))
            return parent;
        node = parent;
    }
}

// Appends `root` if it is an element, and its descendants that are elements, to `out` in tree order, except the
// descendants of a shadow host or a slot, which own scopes of their own.
void appendScopeElements(PrDomNode* root, Array<PrDomElement* PR_NONNULL>* out) {
    for (PrDomNode* node = root; node != nullptr;) {
        PrDomElement* element = domElementFromNode(node);
        if (element != nullptr)
            out->push(element);
        bool owner = element != nullptr && isScopeOwner(element);
        node = owner ? (node == root ? nullptr : domNodeNextSkipping(root, node)) : domNodeNext(root, node);
    }
}

// An element of a tabindex-ordered focus navigation scope, with its tabindex value and its position in tree order.
struct ScopeItem {
    PrDomElement* element;
    int32_t tabIndex;
    uint32_t position;
};

// Appends `owner`'s tabindex-ordered focus navigation scope (html/#tabindex-ordered-focus-navigation-scope) to `out`:
// the owners and the focusable areas of its scope whose tabindex value is not negative. Those with a positive value
// come first, in ascending order, and the rest follow in tree order. An element without a tabindex value orders as 0.
void appendOrderedScope(PrDomNode* owner, Array<PrDomElement* PR_NONNULL>* out) {
    Array<PrDomElement* PR_NONNULL> members;
    PrDomElement* element = domElementFromNode(owner);
    if (owner->type == PrDomNodeType_Document) {
        for (PrDomNode* child = owner->first; child != nullptr; child = child->next)
            appendScopeElements(child, &members);
    } else if (PrDomShadowRoot* shadow = domShadowRootOfHost(element)) {
        for (PrDomNode* child = shadow->first; child != nullptr; child = child->next)
            appendScopeElements(child, &members);
    } else {
        // A slot's scope holds its assigned nodes, or its children if it has none.
        uint32_t count = domSlotAssignedCount(element);
        PrDomNode* const* assigned = domSlotAssigned(element);
        for (uint32_t i = 0; i < count; ++i)
            appendScopeElements(assigned[i], &members);
        for (PrDomNode* child = count == 0 ? element->first : nullptr; child != nullptr; child = child->next)
            appendScopeElements(child, &members);
    }
    Array<ScopeItem> items;
    for (uint32_t i = 0; i < members.count; ++i) {
        PrDomElement* member = members.data[i];
        int32_t index = 0;
        if (!tabIndexAttribute(member, &index))
            index = 0;
        if (index >= 0 && (isScopeOwner(member) || htmlFocusable(member)))
            items.push(ScopeItem{member, index, i});
    }
    members.reset();
    sortItems(items.data, items.count, [](const ScopeItem& a, const ScopeItem& b) {
        if ((a.tabIndex == 0) != (b.tabIndex == 0))
            return b.tabIndex == 0;
        return a.tabIndex != b.tabIndex ? a.tabIndex < b.tabIndex : a.position < b.position;
    });
    for (uint32_t i = 0; i < items.count; ++i)
        out->push(items.data[i].element);
    items.reset();
}

// Appends `owner`'s flattened tabindex-ordered focus navigation scope
// (html/#flattened-tabindex-ordered-focus-navigation-scope) to `out`. Each owner in the scope is replaced by its own
// flattened scope, which follows the owner if the owner focuses itself.
void appendFlattenedScope(PrDomNode* owner, Array<PrDomElement* PR_NONNULL>* out) {
    Array<PrDomElement* PR_NONNULL> scope;
    appendOrderedScope(owner, &scope);
    for (uint32_t i = 0; i < scope.count; ++i) {
        PrDomElement* element = scope.data[i];
        bool inner = isScopeOwner(element);
        if (!inner || focusesItself(element))
            out->push(element);
        if (inner)
            appendFlattenedScope(element, out);
    }
    scope.reset();
}

// Returns the document of the iframe `element`, or null if the element is not an iframe with a document.
PrDomDocument* PR_NULLABLE frameDocument(const PrDomElement* element) {
    const PrDomEventTarget* global = element->document->global;
    if (global == nullptr || global->type != DomTargetType_Window)
        return nullptr;
    const PrHtmlWindow* window = static_cast<const PrHtmlWindow*>(global);
    for (uint32_t i = 0; i < window->frames.count; ++i) {
        const HtmlFrame& frame = window->frames.data[i];
        if (frame.container == element && frame.window != nullptr)
            return frame.window->document;
    }
    return nullptr;
}

PrDomElement* PR_NULLABLE firstArea(PrDomDocument* document, bool forward);

// Returns the area that sequential navigation focuses for the sequentially focusable area `element`. This is the
// element itself, or for an iframe with a document, the first or last area of that document. Returns null for an iframe
// whose document has none, which navigation passes over (html/#sequential-navigation-search-algorithm).
PrDomElement* PR_NULLABLE areaOf(PrDomElement* element, bool forward) {
    PrDomDocument* inner = frameDocument(element);
    return inner != nullptr ? firstArea(inner, forward) : element;
}

// Returns the first area that sequential navigation focuses among `elements`, or the last going backward.
PrDomElement* PR_NULLABLE areaAmong(const Array<PrDomElement* PR_NONNULL>& elements, bool forward) {
    for (uint32_t i = 0; i < elements.count; ++i) {
        if (PrDomElement* area = areaOf(elements.data[forward ? i : elements.count - 1 - i], forward))
            return area;
    }
    return nullptr;
}

// Returns the first suitable sequentially focusable area of the document, or the last going backward, or null if it has
// none.
PrDomElement* PR_NULLABLE firstArea(PrDomDocument* document, bool forward) {
    Array<PrDomElement* PR_NONNULL> flattened;
    appendFlattenedScope(document, &flattened);
    PrDomElement* area = areaAmong(flattened, forward);
    flattened.reset();
    return area;
}

// Returns the area that sequential navigation focuses for `element`, an item of a tabindex-ordered scope, or null if it
// focuses none there. An owner that focuses itself comes before its flattened scope.
PrDomElement* PR_NULLABLE itemArea(PrDomElement* element, bool forward) {
    if (!isScopeOwner(element))
        return areaOf(element, forward);
    Array<PrDomElement* PR_NONNULL> flattened;
    if (focusesItself(element))
        flattened.push(element);
    appendFlattenedScope(element, &flattened);
    PrDomElement* area = areaAmong(flattened, forward);
    flattened.reset();
    return area;
}

// Returns the index in `scope` of the item nearest `start` in tree order, after it going forward and before it going
// backward, or -1 if there is none. If `after` is set, the position is just after `start` and its descendants, so that
// they come before it. The items and `start` are in one tree.
int64_t nearestItem(const Array<PrDomElement* PR_NONNULL>& scope, const PrDomNode* start, bool forward, bool after) {
    int64_t nearest = -1;
    for (uint32_t i = 0; i < scope.count; ++i) {
        const PrDomNode* item = scope.data[i];
        bool within = after && domShadowIncludingContains(start, item);
        if (forward ? within || !domNodePrecedes(start, item) : !within && !domNodePrecedes(item, start))
            continue;
        const PrDomNode* best = nearest >= 0 ? scope.data[nearest] : nullptr;
        if (best == nullptr || (forward ? domNodePrecedes(item, best) : domNodePrecedes(best, item)))
            nearest = int64_t(i);
    }
    return nearest;
}

// Moves `*start` up to its nearest shadow-including inclusive ancestor that belongs to a focus navigation scope, and
// returns that scope's owner. Returns null if `*start` reaches a document.
PrDomNode* PR_NULLABLE ownedStart(PrDomNode** start) {
    for (PrDomNode* node = *start; node != nullptr; node = domShadowIncludingParent(node)) {
        if (PrDomNode* owner = scopeOwnerOf(node)) {
            *start = node;
            return owner;
        }
        *start = node;
    }
    return nullptr;
}

// Returns the suitable sequentially focusable area that follows `start` in its document, or that precedes it going
// backward, or null if there is none (html/#sequential-navigation-search-algorithm). The search runs through the scope
// that holds `start`, and then through the scopes around it. A starting point that is in its scope's order moves along
// that order. Any other starting point starts from the item nearest it in tree order, which is how browsers resolve the
// algorithm's "DOM" selection mechanism within a scope. Going forward from a focused owner enters its scope first. If
// `after` is set, the starting point is the position just after `start` and its descendants.
PrDomElement* PR_NULLABLE searchFrom(PrDomNode* start, bool forward, bool after) {
    PrDomNode* given = start;
    PrDomNode* owner = ownedStart(&start);
    after = after && start == given;
    if (owner == nullptr) {
        if (start->type != PrDomNodeType_Document)
            return nullptr;
        return firstArea(static_cast<PrDomDocument*>(start), forward);
    }
    PrDomElement* entered = domElementFromNode(start);
    if (forward && !after && entered != nullptr && isScopeOwner(entered) && entered->document->focused == entered) {
        Array<PrDomElement* PR_NONNULL> flattened;
        appendFlattenedScope(entered, &flattened);
        PrDomElement* area = areaAmong(flattened, forward);
        flattened.reset();
        if (area != nullptr)
            return area;
    }
    Array<PrDomElement* PR_NONNULL> scope;
    PrDomElement* found = nullptr;
    while (found == nullptr && owner != nullptr) {
        scope.count = 0;
        appendOrderedScope(owner, &scope);
        int64_t at = -1;
        for (uint32_t i = 0; i < scope.count && at < 0 && !after; ++i) {
            if (scope.data[i] == start)
                at = int64_t(i);
        }
        int64_t step = forward ? 1 : -1;
        for (int64_t i = at >= 0 ? at + step : nearestItem(scope, start, forward, after);
                i >= 0 && i < int64_t(scope.count) && found == nullptr; i += step)
            found = itemArea(scope.data[i], forward);
        // Past the end of its scope, the search continues from the scope's owner in the scope around it. Going
        // backward, an owner that focuses itself comes before its scope.
        if (found != nullptr || owner->type == PrDomNodeType_Document)
            break;
        PrDomElement* element = domElementFromNode(owner);
        if (!forward && focusesItself(element) && !skipsNavigation(element)) {
            found = element;
            break;
        }
        start = owner;
        owner = ownedStart(&start);
        after = false;
    }
    scope.reset();
    return found;
}

// Returns the window whose document is `document`, or null if it has none.
const PrHtmlWindow* PR_NULLABLE windowOf(const PrDomDocument* document) {
    const PrDomEventTarget* global = document->global;
    if (global == nullptr || global->type != DomTargetType_Window)
        return nullptr;
    const PrHtmlWindow* window = static_cast<const PrHtmlWindow*>(global);
    return window->document == document ? window : nullptr;
}

// Appends `document` and the documents of the frames within it to `out`, each document before its frames' documents.
void appendDocuments(PrDomDocument* document, Array<PrDomDocument* PR_NONNULL>* out) {
    out->push(document);
    const PrHtmlWindow* window = windowOf(document);
    for (uint32_t i = 0; window != nullptr && i < window->frames.count; ++i) {
        const PrHtmlWindow* frame = window->frames.data[i].window;
        if (frame != nullptr && frame->document != nullptr)
            appendDocuments(frame->document, out);
    }
}

// Returns true if every document that holds `document` through frames has the origin of `document`.
bool sameOriginAncestors(const PrDomDocument* document) {
    const PrHtmlWindow* window = windowOf(document);
    if (window == nullptr)
        return false;
    PrString origin = htmlWindowOrigin(window);
    bool same = true;
    for (const PrHtmlWindow* parent = window->parent; same && parent != nullptr; parent = parent->parent) {
        PrString other = htmlWindowOrigin(parent);
        same = prStringView(&other) == prStringView(&origin);
        prStringReset(&other);
    }
    prStringReset(&origin);
    return same;
}

// Returns true if `document`, or a document that holds it through frames, has a target element.
bool targetAround(const PrDomDocument* document) {
    for (const PrDomDocument* at = document; at != nullptr;) {
        if (at->target != nullptr)
            return true;
        const PrDomElement* container = htmlFrameContainer(at);
        at = container != nullptr ? container->document : nullptr;
    }
    return false;
}

// Returns true if an iframe around `document` sandboxes automatic features: its sandbox attribute does not allow
// scripts (html/#sandboxed-automatic-features-browsing-context-flag). The iframe's attribute is read as it is now.
bool sandboxedAutomatic(const PrDomDocument* document) {
    for (const PrDomElement* container = htmlFrameContainer(document); container != nullptr;
            container = htmlFrameContainer(container->document)) {
        const PrDomAttr* sandbox = domAttribute(container, PrDomNamespace_None, "sandbox");
        if (sandbox == nullptr)
            continue;
        Array<PrStringView> tokens;
        domOrderedSetParse(prStringView(&sandbox->value), &tokens);
        bool scripts = false;
        for (uint32_t i = 0; i < tokens.count && !scripts; ++i)
            scripts = asciiEqualsFold(tokens.data[i], "allow-scripts");
        tokens.reset();
        if (!scripts)
            return true;
    }
    return false;
}

// Removes `element` from its document's autofocus candidates.
void dropCandidate(PrDomElement* element) {
    Array<PrDomElement* PR_NONNULL>& candidates = element->document->autofocusCandidates;
    for (uint32_t i = 0; i < candidates.count; ++i) {
        if (candidates.data[i] == element) {
            candidates.erase(i, 1);
            return;
        }
    }
}

} // namespace

bool htmlFocusable(const PrDomElement* element) {
    if (!domIsConnected(element))
        return false;
    // An area is rendered through the image that uses its map, and has no box of its own.
    PrCssPresentation* presentation = cssPresentationOf(element->document);
    bool rendered;
    if (presentation != nullptr && !domIsHtml(element, PrDomTag_Area))
        rendered = cssPresentationIsInteractive(presentation, element);
    else
        rendered = !hiddenOrInert(element);
    if (!rendered)
        return false;
    int32_t index = 0;
    return focusableKind(element) || tabIndexAttribute(element, &index);
}

int32_t htmlTabIndex(const PrDomElement* element) {
    int32_t index = 0;
    if (tabIndexAttribute(element, &index))
        return index;
    if (element->space == PrDomNamespace_Svg)
        return element->tag == PrDomTag_A ? 0 : -1;
    if (element->space != PrDomNamespace_Html)
        return -1;
    switch (element->tag) {
        case PrDomTag_A:
        case PrDomTag_Area:
        case PrDomTag_Button:
        case PrDomTag_Frame:
        case PrDomTag_Iframe:
        case PrDomTag_Input: case PrDomTag_Object: case PrDomTag_Select: case PrDomTag_Textarea:
            return 0;
        default:
            return isDetailsSummary(element) ? 0 : -1;
    }
}

bool htmlFocus(PrDomElement* element, HtmlFocusIndication indication) {
    // Whether an element is rendered can depend on its layout, as a container query decides a display.
    if (PrCssPresentation* presentation = cssPresentationOf(element->document))
        cssPresentationFlush(presentation);
    PrDomDocument* document = element->document;
    PrDomElement* target = element;
    if (PrDomShadowRoot* shadow = domShadowRootOfHost(element); shadow != nullptr && shadow->delegatesFocus) {
        if (document->focused != nullptr && domShadowIncludingContains(element, document->focused))
            return true;
        target = focusDelegate(element);
        if (target == nullptr)
            return false;
    }
    if (!htmlFocusable(target))
        return false;
    bool visible = false;
    switch (indication) {
        case HtmlFocusIndication_Shown:
            visible = true;
            break;
        case HtmlFocusIndication_Hidden:
            visible = false;
            break;
        case HtmlFocusIndication_Script:
            visible = takesTextInput(target) ||
                (!document->pressInput && (document->focused == nullptr || document->focusVisible));
            break;
        case HtmlFocusIndication_Press:
            visible = takesTextInput(target);
            break;
    }
    // The iframes around the document take the focus in the documents that hold them, outermost first
    // (html/#focus-chain).
    if (PrDomElement* container = htmlFrameContainer(document))
        htmlFocus(container, indication);
    if (document->focused == target) {
        if (document->focusVisible != visible) {
            document->focusVisible = visible;
            const CssPseudoClass state = CssPseudoClass_FocusVisible;
            cssPresentationStateMoved(nullptr, target, &state, 1);
        }
        return true;
    }
    document->focusVisible = visible;
    focusUpdate(document, document->focused, target);
    return true;
}

void htmlBlur(PrDomElement* element) {
    PrDomDocument* document = element->document;
    if (document->focused == element)
        focusUpdate(document, element, nullptr);
}

void htmlFocusFixup(PrDomDocument* document) {
    if (document->focused != nullptr && !htmlFocusable(document->focused))
        focusUpdate(document, document->focused, nullptr);
}

void htmlFocusPress(PrDomElement* pressed) {
    PrDomDocument* document = pressed->document;
    document->pressInput = true;
    bool focused = false;
    for (PrDomNode* node = pressed; node != nullptr && !focused; node = domShadowIncludingParent(node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element == nullptr)
            continue;
        PrDomShadowRoot* shadow = domShadowRootOfHost(element);
        if (htmlFocusable(element) || (shadow != nullptr && shadow->delegatesFocus))
            focused = htmlFocus(element, HtmlFocusIndication_Press);
    }
    if (!focused && document->focused != nullptr)
        focusUpdate(document, document->focused, nullptr);
    // A focus listener can remove the element pressed.
    if (domIsConnected(pressed) && pressed->document == document)
        setStartingPoint(document, pressed);
}

void htmlFocusFragment(PrDomElement* target) {
    PrDomDocument* document = target->document;
    if (!htmlFocus(target, HtmlFocusIndication_Script) && document->focused != nullptr)
        focusUpdate(document, document->focused, nullptr);
    if (domIsConnected(target) && target->document == document)
        setStartingPoint(document, target);
}

bool htmlFocusHasAutofocus(PrDomDocument* top) {
    Array<PrDomDocument* PR_NONNULL> documents;
    appendDocuments(top, &documents);
    bool pending = false;
    for (uint32_t i = 0; i < documents.count && !pending; ++i)
        pending = documents.data[i]->autofocusCandidates.count != 0;
    documents.reset();
    return pending;
}

void htmlFocusFlushAutofocus(PrDomDocument* top) {
    Array<PrDomDocument* PR_NONNULL> documents;
    appendDocuments(top, &documents);
    // The candidates in order: the top-level document's, then those of each frame's document.
    Array<PrDomElement* PR_NONNULL> candidates;
    for (uint32_t i = 0; i < documents.count; ++i) {
        const Array<PrDomElement* PR_NONNULL>& own = documents.data[i]->autofocusCandidates;
        for (uint32_t c = 0; c < own.count; ++c)
            candidates.push(own.data[c]);
    }
    bool processed = top->autofocusProcessed || (candidates.count != 0 && (top->focused != nullptr || top->target != nullptr));
    PrDomElement* focus = nullptr;
    for (uint32_t i = 0; i < candidates.count && !processed; ++i) {
        PrDomElement* element = candidates.data[i];
        PrDomDocument* document = element->document;
        if (!domIsConnected(element) || !sameOriginAncestors(document) || sandboxedAutomatic(document)) {
            dropCandidate(element);
            continue;
        }
        if (cssDocumentHasBlockingSheet(document))
            break;
        dropCandidate(element);
        // Whether an element is rendered can depend on its layout.
        if (PrCssPresentation* presentation = cssPresentationOf(document))
            cssPresentationFlush(presentation);
        if (targetAround(document))
            continue;
        PrDomShadowRoot* shadow = domShadowRootOfHost(element);
        bool delegates = shadow != nullptr && shadow->delegatesFocus && focusDelegate(element) != nullptr;
        if (htmlFocusable(element) || delegates) {
            focus = element;
            processed = true;
        }
    }
    candidates.reset();
    if (processed) {
        top->autofocusProcessed = true;
        for (uint32_t i = 0; i < documents.count; ++i)
            documents.data[i]->autofocusCandidates.count = 0;
    }
    documents.reset();
    if (focus != nullptr)
        htmlFocus(focus, HtmlFocusIndication_Shown);
}

void htmlFocusEndAutofocus(PrDomDocument* document) {
    PrDomDocument* top = document;
    for (PrDomElement* container = htmlFrameContainer(top); container != nullptr; container = htmlFrameContainer(top))
        top = container->document;
    if (top != document && !sameOriginAncestors(document))
        return;
    Array<PrDomDocument* PR_NONNULL> documents;
    appendDocuments(top, &documents);
    for (uint32_t i = 0; i < documents.count; ++i)
        documents.data[i]->autofocusCandidates.count = 0;
    documents.reset();
    top->autofocusProcessed = true;
}

void htmlFocusKey(PrDomDocument* document) {
    document->pressInput = false;
    if (document->focused == nullptr || document->focusVisible)
        return;
    document->focusVisible = true;
    const CssPseudoClass state = CssPseudoClass_FocusVisible;
    cssPresentationStateMoved(nullptr, document->focused, &state, 1);
}

bool htmlFocusNavigate(PrDomDocument* document, HtmlFocusDirection direction) {
    bool forward = direction == HtmlFocusDirection_Forward;
    // The search starts in the document of the innermost focused iframe, from the starting point if it is inside the
    // focused area there (html/#sequential-focus-navigation).
    PrDomDocument* at = document;
    while (at->focused != nullptr) {
        PrDomDocument* inner = frameDocument(at->focused);
        if (inner == nullptr)
            break;
        at = inner;
    }
    PrDomElement* focused = at->focused;
    PrDomElement* start = at->navigationStart;
    if (start != nullptr && focused != nullptr && !domShadowIncludingContains(focused, start))
        start = nullptr;
    PrDomElement* area = nullptr;
    if (start != nullptr) {
        area = searchFrom(start, forward, at->navigationStartAfter);
        // A starting point inside the focused element finds what the element holds. Past that, the search moves on from
        // the element itself, in its order.
        if (focused != nullptr && start != focused &&
                (area == nullptr || area == focused || !domShadowIncludingContains(focused, area)))
            area = searchFrom(focused, forward, false);
    } else {
        area = searchFrom(focused != nullptr ? static_cast<PrDomNode*>(focused) : at, forward, false);
    }
    while (area == nullptr) {
        PrDomElement* container = at != document ? htmlFrameContainer(at) : nullptr;
        if (container == nullptr)
            break;
        at = container->document;
        area = searchFrom(container, forward, false);
    }
    if (area != nullptr) {
        htmlFocus(area, HtmlFocusIndication_Shown);
        return true;
    }
    if (document->focused != nullptr)
        focusUpdate(document, document->focused, nullptr);
    setStartingPoint(document, nullptr);
    return false;
}

PrDomElement* PR_NULLABLE htmlKeyTarget(PrDomDocument* document) {
    PrDomDocument* at = document;
    while (at->focused != nullptr) {
        PrDomDocument* inner = frameDocument(at->focused);
        if (inner == nullptr)
            return at->focused;
        at = inner;
    }
    if (PrDomElement* body = domDocumentBody(at))
        return body;
    return domDocumentElement(at);
}

PrDomElement* PR_NULLABLE htmlActiveElement(PrDomNode* root) {
    PrDomDocument* document = root->type == PrDomNodeType_Document ? static_cast<PrDomDocument*>(root) : root->document;
    PrDomNode* candidate = document->focused != nullptr ? static_cast<PrDomNode*>(document->focused) : document;
    // Retarget against the root: move to the host of each shadow tree that contains the candidate and that the root is
    // not in.
    for (;;) {
        PrDomShadowRoot* shadow = domNodeShadowRoot(domNodeRoot(candidate));
        if (shadow == nullptr || shadow == root)
            break;
        bool within = false;
        for (PrDomNode* node = root; node != nullptr && !within; node = domShadowIncludingParent(node))
            within = node == shadow;
        if (within)
            break;
        candidate = shadow->host;
    }
    if (domNodeRoot(candidate) != root)
        return nullptr;
    if (candidate->type != PrDomNodeType_Document)
        return domElementFromNode(candidate);
    if (PrDomElement* body = domDocumentBody(document))
        return body;
    return domDocumentElement(document);
}

bool htmlHasFocus(const PrDomDocument* document) {
    const PrDomEventTarget* global = document->global;
    if (global == nullptr || global->type != DomTargetType_Window)
        return false;
    const PrHtmlWindow* window = static_cast<const PrHtmlWindow*>(global);
    if (window->document != document)
        return false;
    // A nested browsing context has system focus if its container is focused in its parent.
    for (const PrHtmlWindow* parent = window->parent; parent != nullptr; window = parent, parent = parent->parent) {
        bool focused = false;
        for (uint32_t i = 0; i < parent->frames.count; ++i) {
            const HtmlFrame& frame = parent->frames.data[i];
            focused |= frame.window == window && parent->document->focused == frame.container;
        }
        if (!focused)
            return false;
    }
    return true;
}

} // namespace pr_impl
