// IntersectionObserver: the intersections of observed elements with a root, computed by the rendering update and
// delivered by a task.
//
// intersection-observer - https://w3c.github.io/IntersectionObserver/

#include "pr/css_intersection_observer.h"
#include "css/src/css_intersection_observer.h"

#include "pr/alloc.h"
#include "pr/css_view.h"
#include "pr/float.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/text_buffer.h"

#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_root.h"
#include "css/src/css_serialize.h"
#include "css/src/css_syntax.h"
#include "css/src/css_unit.h"
#include "css/src/css_view.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_node.h"

namespace pr_impl {

namespace {

// A rectangle in the viewport's coordinates, in CSS pixels, by its edges.
struct Edges {
    double left;
    double top;
    double right;
    double bottom;
};

double px(PrLayoutUnit unit) {
    return prLayoutUnitToPxDouble(unit);
}

Edges edgesOf(PrDomRect rect) {
    return Edges{rect.x, rect.y, rect.x + rect.width, rect.y + rect.height};
}

PrDomRect rectOf(Edges edges) {
    return PrDomRect{edges.left, edges.top, edges.right - edges.left, edges.bottom - edges.top};
}

double areaOf(Edges edges) {
    return (edges.right - edges.left) * (edges.bottom - edges.top);
}

// Writes the intersection of `a` and `b` to `out`. Returns false if they do not meet. Rectangles that share only an
// edge meet, in a rectangle of no area.
bool intersect(Edges a, Edges b, Edges* out) {
    Edges meet{maxF64(a.left, b.left), maxF64(a.top, b.top), minF64(a.right, b.right), minF64(a.bottom, b.bottom)};
    if (meet.right < meet.left || meet.bottom < meet.top)
        return false;
    *out = meet;
    return true;
}

// Returns the rectangle grown outward by the margin's offsets, top, right, bottom and left. A percentage of the left or
// right offset is of the rectangle's width, and one of the top or bottom offset is of its height, so that a margin in
// percentages grows the rectangle in proportion along each axis. The specification resolves every percentage against
// the width (intersection-observer/#intersection-observer-interface).
Edges grow(Edges edges, const CssIntersectionOffset margin[4]) {
    double width = edges.right - edges.left;
    double height = edges.bottom - edges.top;
    auto resolve = [&](const CssIntersectionOffset& offset, double base) {
        return offset.percent ? offset.value * base / 100 : offset.value;
    };
    return Edges{edges.left - resolve(margin[3], width), edges.top - resolve(margin[0], height),
                 edges.right + resolve(margin[1], width), edges.bottom + resolve(margin[2], height)};
}

// Reads a margin of one to four absolute lengths or percentages into its four offsets
// (intersection-observer/#parse-a-margin). Returns false if `text` is not one.
bool parseMargin(PrStringView text, CssIntersectionOffset out[4]) {
    CssTokenArray tokens = {};
    CssTokenScratch scratch = {};
    cssTokenize(&tokens, &scratch, text);
    CssIntersectionOffset offsets[4] = {};
    uint32_t count = 0;
    bool ok = true;
    for (uint32_t i = 0; ok && i < cssTokenCount(&tokens); ++i) {
        CssTokenKind kind = cssTokenKind(&tokens, i);
        if (kind == CssTokenKind_Whitespace)
            continue;
        PrCssUnit unit = kind == CssTokenKind_Dimension ? cssTokenUnit(&tokens, i) : PrCssUnit_Unknown;
        bool length = unit != PrCssUnit_Unknown && cssUnitCategory(unit) == CssUnitCategory_Length
                      && cssUnitScale(unit) != 0;
        ok = count < 4 && (length || kind == CssTokenKind_Percentage);
        // A length is a whole number of pixels, toward zero.
        if (ok) {
            double number = cssTokenNumber(&tokens, i);
            double pixels = kind == CssTokenKind_Percentage ? 0 : number * cssUnitScale(unit);
            offsets[count++] = kind == CssTokenKind_Percentage
                                   ? CssIntersectionOffset{number, true}
                                   : CssIntersectionOffset{pixels < 0 ? -floorF64(-pixels) : floorF64(pixels), false};
        }
    }
    cssTokenArrayReset(&tokens);
    cssTokenScratchReset(&scratch);
    if (!ok)
        return false;
    if (count == 0)
        offsets[count++] = CssIntersectionOffset{0, false};
    // One offset is every side's, two are the vertical and the horizontal sides', and three leave the left the right's.
    out[0] = offsets[0];
    out[1] = count > 1 ? offsets[1] : offsets[0];
    out[2] = count > 2 ? offsets[2] : offsets[0];
    out[3] = count > 3 ? offsets[3] : out[1];
    return true;
}

// Returns the margin's offsets serialized, each a number followed by px or %, separated by spaces.
PrString serializeMargin(const CssIntersectionOffset margin[4]) {
    TextBuffer text = {};
    for (uint32_t i = 0; i < 4; ++i) {
        if (i != 0)
            textAppendChar(&text, ' ');
        cssSerializeNumber(&text, margin[i].value);
        textAppend(&text, margin[i].percent ? "%" : "px");
    }
    return textTake(&text);
}

// Writes the padding box of a box in the viewport's coordinates. Returns false if the node has no box.
bool paddingBoxOf(PrLayoutTree* tree, PrLayoutNode node, Edges* out) {
    PrLayoutRect box{};
    if (prLayoutNodeBoxes(tree, node, PrLayoutCoordinates_Viewport, &box, 1) == 0)
        return false;
    *out = Edges{px(box.left + prLayoutNodeBorderWidth(tree, node, PrRectSide_Left)),
                 px(box.top + prLayoutNodeBorderWidth(tree, node, PrRectSide_Top)),
                 px(box.right - prLayoutNodeBorderWidth(tree, node, PrRectSide_Right)),
                 px(box.bottom - prLayoutNodeBorderWidth(tree, node, PrRectSide_Bottom))};
    return true;
}

// Returns true if `node` is one of the shadow-including descendants of `ancestor`.
bool isDescendant(const PrDomNode* node, const PrDomNode* ancestor) {
    for (const PrDomNode* at = domShadowIncludingParent(node); at != nullptr; at = domShadowIncludingParent(at)) {
        if (at == ancestor)
            return true;
    }
    return false;
}

// Returns the box that the target's intersection is clipped from: its own box, or for an SVG element that CSS does not
// lay out, the box of the nearest element around it that CSS lays out. Returns kPrLayoutNodeNone if the target is not
// rendered.
PrLayoutNode clippedBoxOf(PrCssPresentation* presentation, const PrDomElement* target) {
    PrLayoutNode box = cssPresentationBox(presentation, target);
    if (box != kPrLayoutNodeNone || target->space != PrDomNamespace_Svg)
        return box;
    for (const PrDomNode* at = domFlatParent(target); at != nullptr; at = domFlatParent(at)) {
        const PrDomElement* element = domElementFromNode(at);
        if (element == nullptr)
            return kPrLayoutNodeNone;
        box = cssPresentationBox(presentation, element);
        if (box != kPrLayoutNodeNone)
            return box;
    }
    return kPrLayoutNodeNone;
}

// Returns true if the box of the element `root` is a scroll container, which clips its content to its padding box.
bool clipsContent(PrCssPresentation* presentation, PrLayoutNode root) {
    PrLayoutScrollInfo info{};
    return root != kPrLayoutNodeNone && prLayoutNodeScrollInfo(cssPresentationOwnTree(presentation), root, &info)
           && info.scrollContainer;
}

// Returns the observer's root intersection rectangle, grown by its root margin
// (intersection-observer/#intersection-observer-interface). A root that is a document, or no root, is the document's
// viewport. A root that clips its content is its padding box, which the scroll margin also grows, and any other root
// element is its bounding box.
Edges rootBoundsOf(const PrCssIntersectionObserver* observer, PrCssPresentation* presentation) {
    const PrDomNode* root = observer->root;
    PrDomElement* element = root != nullptr ? domElementFromNode(const_cast<PrDomNode*>(root)) : nullptr;
    if (element == nullptr) {
        PrDomDocument* document = root != nullptr ? root->document : observer->document;
        PrLayoutVec extent = cssViewViewportExtent(document);
        return grow(Edges{0, 0, px(extent.x), px(extent.y)}, observer->rootMargin);
    }
    PrLayoutNode node = cssPresentationBox(presentation, element);
    Edges padding{};
    if (clipsContent(presentation, node) && paddingBoxOf(cssPresentationOwnTree(presentation), node, &padding))
        return grow(grow(padding, observer->scrollMargin), observer->rootMargin);
    return grow(edgesOf(prCssElementGetBoundingClientRect(element)), observer->rootMargin);
}

// What an update found of a target: its bounding box, and the part of it that shows within the root. A target that the
// update could not measure against the root (`measured` false) has empty rectangles.
struct Intersection {
    Edges target;
    Edges shown;
    bool intersecting;
    bool measured;
};

// Computes the intersection of the target with the observer's root
// (intersection-observer/#calculate-intersection-rect-algo): its bounding box, cut by the padding box of each scroll
// container that it is in, up to the root, grown by the scroll margin, and cut by the root's rectangle. A target that
// is not rendered, in another document, or not inside an element root is not measured, and intersects nothing.
Intersection intersectionOf(const PrCssIntersectionObserver* observer, PrCssPresentation* presentation,
                            PrDomElement* target, Edges rootBounds) {
    Intersection result{};
    PrLayoutNode node = clippedBoxOf(presentation, target);
    const PrDomNode* root = observer->root;
    PrDomDocument* document = root != nullptr ? root->document : observer->document;
    PrDomElement* rootElement = root != nullptr ? domElementFromNode(const_cast<PrDomNode*>(root)) : nullptr;
    if (node == kPrLayoutNodeNone || target->document != document
        || (rootElement != nullptr && !isDescendant(target, root)))
        return result;
    result.measured = true;
    result.target = edgesOf(prCssElementGetBoundingClientRect(target));
    Edges shown = result.target;
    PrLayoutTree* tree = cssPresentationOwnTree(presentation);
    PrLayoutNode rootNode = rootElement != nullptr ? cssPresentationBox(presentation, rootElement) : kPrLayoutNodeNone;
    for (PrLayoutNode container = prLayoutNodeScrollContainer(tree, node);
         container != kPrLayoutNodeNone && container != rootNode;
         container = prLayoutNodeScrollContainer(tree, container)) {
        // The scroll containers outside an element root do not clip within it.
        const PrDomNode* owner = cssPresentationNodeFromLayout(presentation, container);
        if (rootElement != nullptr && (owner == nullptr || !isDescendant(owner, root)))
            break;
        Edges clip{};
        if (paddingBoxOf(tree, container, &clip) && !intersect(shown, grow(clip, observer->scrollMargin), &shown))
            return result;
    }
    result.intersecting = intersect(shown, rootBounds, &result.shown);
    return result;
}

// Returns the index of the first threshold above the ratio, or the number of thresholds if none is above it.
int32_t thresholdIndexOf(const Array<double>& thresholds, double ratio) {
    uint32_t index = 0;
    while (index < thresholds.count && thresholds.data[index] <= ratio)
        ++index;
    return int32_t(index);
}

void removeObservation(PrCssIntersectionObserver* observer, uint32_t index) {
    domNodeRelease(observer->observations.data[index].target);
    observer->observations.erase(index);
    if (observer->observations.count == 0 && observer->callback.observing != nullptr)
        observer->callback.observing(observer->callback.context, observer, false);
}

// Computes each observation of the observer at `time`, and queues an entry for each that crossed a threshold, or began
// or stopped intersecting. Returns true if it queued an entry.
bool update(PrCssIntersectionObserver* observer, PrCssPresentation* presentation, double time) {
    Edges rootBounds = rootBoundsOf(observer, presentation);
    bool queued = false;
    for (uint32_t i = 0; i < observer->observations.count; ++i) {
        CssIntersectionObservation& observation = observer->observations.data[i];
        if (observation.updated && time - observation.lastUpdateTime < double(observer->delay))
            continue;
        observation.updated = true;
        observation.lastUpdateTime = time;
        Intersection found = intersectionOf(observer, presentation, observation.target, rootBounds);
        double targetArea = areaOf(found.target);
        double ratio = targetArea > 0 ? areaOf(found.shown) / targetArea : found.intersecting ? 1 : 0;
        int32_t thresholdIndex = thresholdIndexOf(observer->thresholds, ratio);
        // A target intersects only once it reaches a threshold (w3c/IntersectionObserver#432), so that a target below
        // the lowest threshold is reported as not intersecting.
        bool intersecting = found.intersecting && thresholdIndex > 0;
        // The engine cannot rule out that other content covers the target, so no target is visible
        // (intersection-observer/#calculate-visibility-algo).
        bool visible = false;
        if (thresholdIndex != observation.previousThresholdIndex
            || intersecting != observation.previousIsIntersecting || visible != observation.previousIsVisible) {
            domNodeRetain(observation.target);
            // A target that was not measured against the root reports empty root bounds.
            observer->queued.push(PrCssIntersectionObserverEntry{.time = time,
                                                                 .rootBounds = found.measured ? rectOf(rootBounds)
                                                                                              : PrDomRect{},
                                                                 .boundingClientRect = rectOf(found.target),
                                                                 .intersectionRect = rectOf(found.shown),
                                                                 .intersectionRatio = ratio,
                                                                 .target = observation.target,
                                                                 .isIntersecting = intersecting,
                                                                 .isVisible = visible});
            queued = true;
        }
        observation.previousThresholdIndex = thresholdIndex;
        observation.previousIsIntersecting = intersecting;
        observation.previousIsVisible = visible;
    }
    return queued;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prCssIntersectionObserverCreate(PrDomDocument* document, const PrCssIntersectionObserverInit* init,
                                         const PrCssIntersectionCallback* callback,
                                         PrCssIntersectionObserver* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    CssIntersectionOffset rootMargin[4];
    CssIntersectionOffset scrollMargin[4];
    if (!parseMargin(init->rootMargin, rootMargin) || !parseMargin(init->scrollMargin, scrollMargin))
        return PrStatus_Syntax;
    for (uint32_t i = 0; i < init->thresholdCount; ++i) {
        double threshold = init->thresholds[i];
        if (!(threshold >= 0 && threshold <= 1))
            return PrStatus_OutOfRange;
    }
    PrCssIntersectionObserver* observer = allocZeroed<PrCssIntersectionObserver>();
    observer->references = 1;
    observer->document = document;
    domNodeRetain(document);
    observer->root = init->root;
    if (init->root != nullptr)
        domNodeRetain(init->root);
    observer->callback = *callback;
    if (callback->retain != nullptr)
        callback->retain(callback->context);
    for (uint32_t i = 0; i < 4; ++i) {
        observer->rootMargin[i] = rootMargin[i];
        observer->scrollMargin[i] = scrollMargin[i];
    }
    for (uint32_t i = 0; i < init->thresholdCount; ++i) {
        // Each threshold goes in its place among the ones before it, which keeps them ascending.
        uint32_t at = observer->thresholds.count;
        while (at > 0 && observer->thresholds.data[at - 1] > init->thresholds[i])
            --at;
        observer->thresholds.insert(at, init->thresholds[i]);
    }
    if (observer->thresholds.count == 0)
        observer->thresholds.push(0);
    // A visibility tracker updates at most every 100 ms (intersection-observer/#initialize-new-intersection-observer).
    observer->delay = init->trackVisibility && init->delay < 100 ? 100 : init->delay;
    observer->trackVisibility = init->trackVisibility;
    cssDocumentRoot(document)->intersectionObservers.push(observer);
    *out = observer;
    return PrStatus_Ok;
}

void prCssIntersectionObserverRetain(PrCssIntersectionObserver* self) {
    ++self->references;
}

void prCssIntersectionObserverRelease(PrCssIntersectionObserver* PR_NULLABLE self) {
    if (self == nullptr || --self->references != 0)
        return;
    Array<PrCssIntersectionObserver* PR_NONNULL>& observers = cssDocumentRoot(self->document)->intersectionObservers;
    for (uint32_t i = 0; i < observers.count; ++i) {
        if (observers.data[i] == self) {
            observers.erase(i);
            break;
        }
    }
    for (uint32_t i = 0; i < self->observations.count; ++i)
        domNodeRelease(self->observations.data[i].target);
    self->observations.reset();
    for (uint32_t i = 0; i < self->queued.count; ++i)
        domNodeRelease(self->queued.data[i].target);
    self->queued.reset();
    self->thresholds.reset();
    if (self->callback.release != nullptr)
        self->callback.release(self->callback.context);
    domNodeRelease(self->root);
    domNodeRelease(self->document);
    prMemoryFree(self);
}

PrDomNode* PR_NULLABLE prCssIntersectionObserverRoot(const PrCssIntersectionObserver* self) {
    return self->root;
}

int32_t prCssIntersectionObserverDelay(const PrCssIntersectionObserver* self) {
    return self->delay;
}

bool prCssIntersectionObserverTrackVisibility(const PrCssIntersectionObserver* self) {
    return self->trackVisibility;
}

PrString prCssIntersectionObserverRootMargin(const PrCssIntersectionObserver* self) {
    return serializeMargin(self->rootMargin);
}

PrString prCssIntersectionObserverScrollMargin(const PrCssIntersectionObserver* self) {
    return serializeMargin(self->scrollMargin);
}

const double* prCssIntersectionObserverThresholds(const PrCssIntersectionObserver* self, uint32_t* count) {
    *count = self->thresholds.count;
    return self->thresholds.data;
}

void prCssIntersectionObserverObserve(PrCssIntersectionObserver* self, PrDomElement* target) {
    for (uint32_t i = 0; i < self->observations.count; ++i) {
        if (self->observations.data[i].target == target)
            return;
    }
    domNodeRetain(target);
    self->observations.push(CssIntersectionObservation{.target = target, .previousThresholdIndex = -1});
    if (self->observations.count == 1 && self->callback.observing != nullptr)
        self->callback.observing(self->callback.context, self, true);
}

void prCssIntersectionObserverUnobserve(PrCssIntersectionObserver* self, PrDomElement* target) {
    for (uint32_t i = 0; i < self->observations.count; ++i) {
        if (self->observations.data[i].target == target) {
            removeObservation(self, i);
            return;
        }
    }
}

void prCssIntersectionObserverDisconnect(PrCssIntersectionObserver* self) {
    while (self->observations.count != 0)
        removeObservation(self, self->observations.count - 1);
}

uint32_t prCssIntersectionObserverTakeRecords(PrCssIntersectionObserver* self, PrCssIntersectionObserverEntry* out,
                                              uint32_t capacity) {
    uint32_t taken = capacity < self->queued.count ? capacity : self->queued.count;
    for (uint32_t i = 0; i < taken; ++i)
        out[i] = self->queued.data[i];
    for (uint32_t i = 0; i < taken; ++i)
        self->queued.erase(0);
    return taken;
}

bool prCssDocumentIntersectionObservationsPending(const PrDomDocument* document) {
    const CssRoot* root = document->style;
    for (uint32_t i = 0; root != nullptr && i < root->intersectionObservers.count; ++i) {
        const PrCssIntersectionObserver* observer = root->intersectionObservers.data[i];
        for (uint32_t j = 0; j < observer->observations.count; ++j) {
            if (!observer->observations.data[j].updated)
                return true;
        }
    }
    return false;
}

bool prCssDocumentUpdateIntersectionObservations(PrDomDocument* document, double time) {
    CssRoot* root = cssDocumentRoot(document);
    bool observing = false;
    for (uint32_t i = 0; i < root->intersectionObservers.count && !observing; ++i)
        observing = root->intersectionObservers.data[i]->observations.count != 0;
    PrCssPresentation* presentation = cssPresentationOf(document);
    if (!observing || presentation == nullptr)
        return false;
    cssPresentationFlush(presentation);
    bool queued = false;
    for (uint32_t i = 0; i < root->intersectionObservers.count; ++i)
        queued = update(root->intersectionObservers.data[i], presentation, time) || queued;
    if (!queued || root->intersectionTaskQueued)
        return false;
    root->intersectionTaskQueued = true;
    return true;
}

void prCssDocumentNotifyIntersectionObservers(PrDomDocument* document) {
    CssRoot* root = cssDocumentRoot(document);
    root->intersectionTaskQueued = false;
    // The callbacks may create observers and free them, so the notification holds the observers that it calls.
    Array<PrCssIntersectionObserver* PR_NONNULL> observers;
    for (uint32_t i = 0; i < root->intersectionObservers.count; ++i) {
        prCssIntersectionObserverRetain(root->intersectionObservers.data[i]);
        observers.push(root->intersectionObservers.data[i]);
    }
    for (uint32_t i = 0; i < observers.count; ++i) {
        PrCssIntersectionObserver* observer = observers.data[i];
        if (observer->queued.count != 0)
            observer->callback.invoke(observer->callback.context, observer);
    }
    for (uint32_t i = 0; i < observers.count; ++i)
        prCssIntersectionObserverRelease(observers.data[i]);
    observers.reset();
}
