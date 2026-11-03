// ResizeObserver: the observation of the sizes of elements' boxes, gathered and delivered by the rendering update.
//
// resize-observer-1 - https://drafts.csswg.org/resize-observer-1/

#include "pr/css_resize_observer.h"
#include "css/src/css_resize_observer.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_root.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_node.h"

namespace pr_impl {

namespace {

// The size that a new observation was last delivered at, which no box has, so that the first delivery reports any size
// (resize-observer-1/#dom-resizeobservation-resizeobservation).
constexpr PrCssResizeObserverSize kNeverReported = {-1, -1};

// The sizes of an element's boxes, and its content rectangle
// (resize-observer-1/#create-and-populate-resizeobserverentry-h).
struct BoxSizes {
    PrCssResizeObserverSize border;
    PrCssResizeObserverSize content;
    PrCssResizeObserverSize devicePixelContent;
    PrDomRect contentRect;
};

double px(PrLayoutUnit unit) {
    return prLayoutUnitToPxDouble(unit);
}

// Returns the size of the physical extent along the inline and block axes of a writing mode.
PrCssResizeObserverSize logical(double width, double height, bool vertical) {
    return vertical ? PrCssResizeObserverSize{height, width} : PrCssResizeObserverSize{width, height};
}

// Returns the sizes of the element's boxes in the presentation's last layout (resize-observer-1/#calculate-box-size),
// with `pixelRatio` device pixels per CSS pixel. An element without a box, and an inline box, which has no single
// content box, measure zero.
BoxSizes measure(PrCssPresentation* PR_NULLABLE presentation, const PrDomElement* element, double pixelRatio) {
    BoxSizes sizes{};
    PrLayoutNode node = presentation != nullptr ? cssPresentationBox(presentation, element) : kPrLayoutNodeNone;
    if (node == kPrLayoutNodeNone)
        return sizes;
    PrLayoutTree* tree = cssPresentationOwnTree(presentation);
    PrLayoutStyle record = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, node));
    PrLayoutBoxStyle box = prLayoutBoxStyleRecord(tree, record.groups[PrLayoutGroup_Box]);
    PrLayoutRect border{};
    if (box.display == PrLayoutDisplay_Inline || prLayoutNodeBoxes(tree, node, PrLayoutCoordinates_Layout, &border, 1) == 0)
        return sizes;
    PrLayoutVec content = prLayoutNodeContentExtent(tree, node);
    // The padding resolves against the containing block's inline size that the box's layout read.
    PrLayoutUnit base = prLayoutNodeContainingInlineSize(tree, node);
    auto padding = [&](PrRectSide side) {
        return base >= PrLayoutUnit{0} ? prLayoutLengthResolve(tree, box.padding[side], base) : PrLayoutUnit{0};
    };
    PrLayoutUnit left = prLayoutNodeBorderWidth(tree, node, PrRectSide_Left) + padding(PrRectSide_Left);
    PrLayoutUnit top = prLayoutNodeBorderWidth(tree, node, PrRectSide_Top) + padding(PrRectSide_Top);
    bool vertical = prLayoutFontStyleRecord(tree, record.groups[PrLayoutGroup_Font]).writingMode !=
        PrLayoutWritingMode_HorizontalTb;
    sizes.border = logical(px(border.right - border.left), px(border.bottom - border.top), vertical);
    sizes.content = logical(px(content.x), px(content.y), vertical);
    // The content box in device pixels covers the whole pixels between its edges, each rounded to the nearest pixel.
    double x0 = prF64Round(px(border.left + left) * pixelRatio);
    double y0 = prF64Round(px(border.top + top) * pixelRatio);
    double x1 = prF64Round(px(border.left + left + content.x) * pixelRatio);
    double y1 = prF64Round(px(border.top + top + content.y) * pixelRatio);
    sizes.devicePixelContent = logical(x1 - x0, y1 - y0, vertical);
    sizes.contentRect = PrDomRect{
        px(padding(PrRectSide_Left)), px(padding(PrRectSide_Top)), px(content.x),
        px(content.y),
    };
    return sizes;
}

PrCssResizeObserverSize observedSize(const BoxSizes& sizes, PrCssResizeObserverBox box) {
    switch (box) {
        case PrCssResizeObserverBox_BorderBox:
            return sizes.border;
        case PrCssResizeObserverBox_DevicePixelContentBox:
            return sizes.devicePixelContent;
        default:
            return sizes.content;
    }
}

bool sameSize(PrCssResizeObserverSize a, PrCssResizeObserverSize b) {
    return a.inlineSize == b.inlineSize && a.blockSize == b.blockSize;
}

// Returns the number of nodes on the path from the node up the flat tree to its root
// (resize-observer-1/#calculate-depth-for-node-h).
uint32_t depthOf(const PrDomNode* node) {
    uint32_t depth = 0;
    for (const PrDomNode* at = node; at != nullptr; at = domFlatParent(at))
        ++depth;
    return depth;
}

// Removes the observation at `index`, and tells the callback once the observer observes no element.
void removeObservation(PrCssResizeObserver* observer, uint32_t index) {
    domNodeRelease(observer->observations.data[index].target);
    observer->observations.erase(index);
    if (observer->observations.count == 0 && observer->callback.observing != nullptr)
        observer->callback.observing(observer->callback.context, observer, false);
}

// Marks the observations whose observed sizes changed as active if they are deeper than `depth`, and notes the others
// as skipped (resize-observer-1/#gather-active-observations-h). Returns true if any is active.
bool gather(const Array<PrCssResizeObserver* PR_NONNULL>& observers, PrCssPresentation* PR_NULLABLE presentation,
        uint32_t depth, double pixelRatio) {
    bool any = false;
    for (uint32_t i = 0; i < observers.count; ++i) {
        PrCssResizeObserver* observer = observers.data[i];
        observer->skipped = false;
        for (uint32_t j = 0; j < observer->observations.count; ++j) {
            CssResizeObservation& observation = observer->observations.data[j];
            PrCssResizeObserverSize size = observedSize(measure(presentation, observation.target, pixelRatio),
                observation.box);
            observation.active = false;
            if (sameSize(size, observation.lastReported))
                continue;
            if (depthOf(observation.target) > depth) {
                observation.active = true;
                any = true;
            } else {
                observer->skipped = true;
            }
        }
    }
    return any;
}

// Calls each observer with the entries of its active observations, which then record the sizes delivered
// (resize-observer-1/#broadcast-active-resize-observations). Returns the depth of the shallowest target delivered.
uint32_t broadcast(const Array<PrCssResizeObserver* PR_NONNULL>& observers,
        PrCssPresentation* PR_NULLABLE presentation, double pixelRatio) {
    uint32_t shallowest = UINT32_MAX;
    Array<PrCssResizeObserverEntry> entries;
    for (uint32_t i = 0; i < observers.count; ++i) {
        PrCssResizeObserver* observer = observers.data[i];
        entries.clear();
        for (uint32_t j = 0; j < observer->observations.count; ++j) {
            CssResizeObservation& observation = observer->observations.data[j];
            if (!observation.active)
                continue;
            observation.active = false;
            BoxSizes sizes = measure(presentation, observation.target, pixelRatio);
            observation.lastReported = observedSize(sizes, observation.box);
            uint32_t depth = depthOf(observation.target);
            shallowest = depth < shallowest ? depth : shallowest;
            entries.push(PrCssResizeObserverEntry{
                .target = observation.target,
                .contentRect = sizes.contentRect,
                .borderBoxSize = sizes.border,
                .contentBoxSize = sizes.content,
                .devicePixelContentBoxSize = sizes.devicePixelContent,
            });
        }
        if (entries.count == 0)
            continue;
        // The callback may stop observing the targets, so the call holds them.
        for (uint32_t j = 0; j < entries.count; ++j)
            domNodeRetain(entries.data[j].target);
        observer->callback.invoke(observer->callback.context, observer, entries.data, entries.count);
        for (uint32_t j = 0; j < entries.count; ++j)
            domNodeRelease(entries.data[j].target);
    }
    entries.reset();
    return shallowest;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrCssResizeObserver* prCssResizeObserverCreate(PrDomDocument* document, const PrCssResizeCallback* callback) {
    PrCssResizeObserver* observer = allocZeroed<PrCssResizeObserver>();
    observer->references = 1;
    observer->document = document;
    domNodeRetain(document);
    observer->callback = *callback;
    if (callback->retain != nullptr)
        callback->retain(callback->context);
    cssDocumentRoot(document)->resizeObservers.push(observer);
    return observer;
}

void prCssResizeObserverRetain(PrCssResizeObserver* observer) {
    ++observer->references;
}

void prCssResizeObserverRelease(PrCssResizeObserver* PR_NULLABLE observer) {
    if (observer == nullptr || --observer->references != 0)
        return;
    Array<PrCssResizeObserver* PR_NONNULL>& observers = cssDocumentRoot(observer->document)->resizeObservers;
    for (uint32_t i = 0; i < observers.count; ++i) {
        if (observers.data[i] == observer) {
            observers.erase(i);
            break;
        }
    }
    for (uint32_t i = 0; i < observer->observations.count; ++i)
        domNodeRelease(observer->observations.data[i].target);
    observer->observations.reset();
    if (observer->callback.release != nullptr)
        observer->callback.release(observer->callback.context);
    domNodeRelease(observer->document);
    prMemoryFree(observer);
}

void prCssResizeObserverObserve(PrCssResizeObserver* observer, PrDomElement* target, PrCssResizeObserverBox box) {
    // An observation that replaces one of the same target does not stop the observer from observing.
    bool observing = observer->observations.count != 0;
    for (uint32_t i = 0; i < observer->observations.count; ++i) {
        if (observer->observations.data[i].target == target) {
            domNodeRelease(target);
            observer->observations.erase(i);
            break;
        }
    }
    domNodeRetain(target);
    observer->observations.push(CssResizeObservation{.target = target, .box = box, .lastReported = kNeverReported});
    cssDocumentRoot(observer->document)->resizeObservationsPending = true;
    if (!observing && observer->callback.observing != nullptr)
        observer->callback.observing(observer->callback.context, observer, true);
}

void prCssResizeObserverUnobserve(PrCssResizeObserver* observer, PrDomElement* target) {
    for (uint32_t i = 0; i < observer->observations.count; ++i) {
        if (observer->observations.data[i].target == target) {
            removeObservation(observer, i);
            return;
        }
    }
}

void prCssResizeObserverDisconnect(PrCssResizeObserver* observer) {
    while (observer->observations.count != 0)
        removeObservation(observer, observer->observations.count - 1);
}

bool prCssDocumentResizeObservationsPending(const PrDomDocument* document) {
    const CssRoot* root = document->style;
    return root != nullptr && root->resizeObservationsPending;
}

bool prCssDocumentBroadcastResizeObservations(PrDomDocument* document, double pixelRatio) {
    CssRoot* root = cssDocumentRoot(document);
    root->resizeObservationsPending = false;
    bool observing = false;
    for (uint32_t i = 0; i < root->resizeObservers.count && !observing; ++i)
        observing = root->resizeObservers.data[i]->observations.count != 0;
    if (!observing)
        return false;
    // The callbacks may create observers and free them, so each round reads a list of the observers that it holds.
    Array<PrCssResizeObserver* PR_NONNULL> observers;
    uint32_t depth = 0;
    bool skipped = false;
    for (;;) {
        PrCssPresentation* presentation = cssPresentationOf(document);
        if (presentation != nullptr)
            cssPresentationFlush(presentation);
        observers.clear();
        for (uint32_t i = 0; i < root->resizeObservers.count; ++i) {
            prCssResizeObserverRetain(root->resizeObservers.data[i]);
            observers.push(root->resizeObservers.data[i]);
        }
        bool active = gather(observers, presentation, depth, pixelRatio);
        skipped = false;
        for (uint32_t i = 0; i < observers.count; ++i)
            skipped = skipped || observers.data[i]->skipped;
        if (active)
            depth = broadcast(observers, presentation, pixelRatio);
        for (uint32_t i = 0; i < observers.count; ++i)
            prCssResizeObserverRelease(observers.data[i]);
        if (!active)
            break;
    }
    observers.reset();
    // A skipped observation is delivered by the next rendering update, which it asks for.
    root->resizeObservationsPending = skipped;
    return skipped;
}
