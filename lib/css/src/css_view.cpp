// CSSOM View: the geometry and scrolling of elements and the viewport, as laid out by the presentation of their
// document.
//
// cssom-view-1 - https://drafts.csswg.org/cssom-view-1/

#include "pr/css_view.h"
#include "css/src/css_view.h"

#include "pr/array.h"
#include "pr/float.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"

#include "css/src/css_animation.h"
#include "css/src/css_boxes.h"
#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_root.h"
#include "css/src/css_style.h"
#include "css/src/css_value.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_node.h"

namespace pr_impl {

namespace {

// Returns the record of the outermost svg element whose content holds `element`, if the element's box has content.
const CssPresentationSvg* PR_NULLABLE svgAround(const PrCssPresentation* presentation, const PrDomElement* element) {
    for (const PrDomNode* at = element; at != nullptr; at = domFlatParent(at)) {
        const PrDomElement* ancestor = domElementFromNode(at);
        if (ancestor == nullptr || !domIsOuterSvg(ancestor))
            continue;
        const CssPresentationSvg* svg = cssPresentationSvgOf(presentation, ancestor);
        return svg != nullptr && svg->functions != nullptr ? svg : nullptr;
    }
    return nullptr;
}

// Returns the presentation of the node's document, brought up to date, if the node is connected in the document.
PrCssPresentation* PR_NULLABLE presentationFor(const PrDomNode* node) {
    PrCssPresentation* presentation = domIsConnected(node) ? cssPresentationOf(node->document) : nullptr;
    if (presentation != nullptr)
        cssPresentationFlush(presentation);
    return presentation;
}

// Convert layout units to pixels, which DOM geometry uses, and pixels to layout units.
double px(PrLayoutUnit unit) {
    return prLayoutUnitToPxDouble(unit);
}
PrLayoutUnit units(double value) {
    return prLayoutUnitFromPxDouble(value);
}

PrDomRect domRect(PrLayoutRect rect) {
    return PrDomRect{px(rect.left), px(rect.top), px(rect.right - rect.left), px(rect.bottom - rect.top)};
}

// Returns a measure rounded to the nearest integer, as a long attribute gives it.
int32_t integer(double value) {
    return isFiniteF64(value) ? int32_t(floorF64(value + 0.5)) : 0;
}

double finiteOr0(double value) {
    return isFiniteF64(value) ? value : 0;
}

// Returns true if a computed overflow value does not scroll. Such a value is visible, or clip, which clips without
// scrolling (css-overflow-3/#valdef-overflow-visible).
bool doesNotScroll(PrCssKeyword overflow) {
    return overflow == PrCssKeyword_Visible || overflow == PrCssKeyword_Clip;
}

// cssom-view-1/#potentially-scrollable, for the body element.
bool isPotentiallyScrollable(PrCssPresentation* presentation, PrDomElement* body) {
    PrDomElement* parent = domElementFromNode(body->parent);
    const CssComputedStyle* own = cssPresentationStyle(presentation, body);
    const CssComputedStyle* around = parent != nullptr ? cssPresentationStyle(presentation, parent) : nullptr;
    if (cssPresentationBox(presentation, body) == kPrLayoutNodeNone || own == nullptr || around == nullptr)
        return false;
    auto scrolls = [](const CssComputedStyle* style) {
        return !doesNotScroll(cssComputedFirstKeyword(style, PrCssProperty_OverflowX)) ||
            !doesNotScroll(cssComputedFirstKeyword(style, PrCssProperty_OverflowY));
    };
    return scrolls(around) && scrolls(own);
}

// The source of an element's scrolling.
enum {
    Scrolling_Own,                  // the element's own scrolling, if it is a scroll container
    Scrolling_Viewport,             // the viewport, if the element is the document's scrolling element
    Scrolling_None,                 // no scrolling, as for the root element of a document in quirks mode
};
typedef uint8_t Scrolling;

Scrolling scrollingOf(PrCssPresentation* presentation, PrDomElement* element) {
    PrDomDocument* document = element->document;
    bool quirks = document->quirks == DomQuirks_Yes;
    if (element == domDocumentElement(document))
        return quirks ? Scrolling_None : Scrolling_Viewport;
    if (quirks && element == domDocumentBody(document) && !isPotentiallyScrollable(presentation, element))
        return Scrolling_Viewport;
    return Scrolling_Own;
}

// Returns true if the element's dimensions are those of the viewport (cssom-view-1/#dom-element-clientwidth).
bool measuresViewport(const PrDomElement* element) {
    PrDomDocument* document = element->document;
    bool quirks = document->quirks == DomQuirks_Yes;
    return (element == domDocumentElement(document) && !quirks) || (element == domDocumentBody(document) && quirks);
}

// Writes the scrolling of the element's block box. Returns false if the element generates no block box.
bool scrollInfoOf(PrCssPresentation* presentation, PrDomElement* element, PrLayoutScrollInfo* out) {
    PrLayoutNode box = cssPresentationBox(presentation, element);
    return box != kPrLayoutNodeNone && prLayoutNodeScrollInfo(cssPresentationOwnTree(presentation), box, out);
}

// Scrolls the source of the element's scrolling to the offsets of `options` that are given. The offsets are relative to
// the current offset if `by` is set, and absolute otherwise.
void scrollElement(PrDomElement* element, const PrCssScrollToOptions* options, bool by) {
    PrCssPresentation* presentation = presentationFor(element);
    if (presentation == nullptr)
        return;
    Scrolling scrolling = scrollingOf(presentation, element);
    if (scrolling == Scrolling_None)
        return;
    PrLayoutNode node;
    if (scrolling == Scrolling_Viewport)
        node = PrLayoutNode(kPrLayoutNodeNone);
    else
        node = cssPresentationBox(presentation, element);
    if (scrolling == Scrolling_Own && node == kPrLayoutNodeNone)
        return;
    PrLayoutScrollInfo info{};
    if (!prLayoutNodeScrollInfo(cssPresentationOwnTree(presentation), node, &info) || !info.scrollContainer)
        return;
    PrLayoutVec to = info.offset;
    if (options->hasLeft)
        to.x = by ? to.x + units(finiteOr0(options->left)) : units(finiteOr0(options->left));
    if (options->hasTop)
        to.y = by ? to.y + units(finiteOr0(options->top)) : units(finiteOr0(options->top));
    cssPresentationScrollTo(presentation, node, to);
}

// Writes the border box of the first box of the element, as laid out and with sticky boxes at their offsets. Returns
// false if the element has no box.
bool firstBox(PrCssPresentation* presentation, const PrDomElement* element, PrLayoutRect* out) {
    PrLayoutNode box = cssPresentationBox(presentation, element);
    return box != kPrLayoutNodeNone &&
        prLayoutNodeBoxes(cssPresentationOwnTree(presentation), box, PrLayoutCoordinates_Positioned, out, 1) != 0;
}

// What decides whether a box belongs to the text of an element.
struct RenderedRoot {
    const PrCssPresentation* presentation;
    const PrDomNode* root;      // the root of the tree of the element
};

// The steps read the children of nodes, so the content of a shadow tree in the flat tree is not part of the text.
bool inOtherTree(void* PR_NULLABLE context, PrLayoutNode box) {
    const RenderedRoot* at = static_cast<const RenderedRoot*>(context);
    const PrDomNode* node = cssPresentationBoxNode(at->presentation, box);
    return node != nullptr && domNodeRoot(node) != at->root;
}

} // namespace

PrLayoutVec cssViewViewportExtent(PrDomDocument* document) {
    PrCssPresentation* presentation = presentationFor(document);
    PrLayoutScrollInfo info{};
    if (presentation == nullptr || !prLayoutNodeScrollInfo(cssPresentationOwnTree(presentation), kPrLayoutNodeNone, &info))
        return PrLayoutVec{};
    return info.portExtent;
}

PrLayoutVec cssViewViewportScroll(PrDomDocument* document) {
    PrCssPresentation* presentation = presentationFor(document);
    PrLayoutScrollInfo info{};
    if (presentation == nullptr || !prLayoutNodeScrollInfo(cssPresentationOwnTree(presentation), kPrLayoutNodeNone, &info))
        return PrLayoutVec{};
    return info.offset;
}

void cssViewScrollViewport(PrDomDocument* document, PrLayoutVec offset) {
    if (PrCssPresentation* presentation = presentationFor(document))
        cssPresentationScrollTo(presentation, kPrLayoutNodeNone, offset);
}

void cssViewScrollViewportBy(PrDomDocument* document, PrLayoutVec delta) {
    cssViewScrollViewport(document, cssViewViewportScroll(document) + delta);
}

bool cssViewIsRendered(PrDomElement* element) {
    PrCssPresentation* presentation = presentationFor(element);
    if (presentation == nullptr)
        return false;
    // A box that the last layout placed nothing for, as content that is laid out when it paints has none yet, is not
    // read.
    PrLayoutNode box = cssPresentationBox(presentation, element);
    return box != kPrLayoutNodeNone && prLayoutNodeBoxes(cssPresentationOwnTree(presentation), box,
        PrLayoutCoordinates_Layout, nullptr, 0) != 0;
}

PrString cssViewRenderedText(PrDomElement* element) {
    PrCssPresentation* presentation = presentationFor(element);
    PR_ASSERT(presentation != nullptr);
    RenderedRoot root{presentation, domNodeRoot(element)};
    PrLayoutRenderedTextDesc desc{
        .context = &root,
        .skips = element->document->slots != nullptr ? inOtherTree : nullptr,
    };
    PrLayoutTree* tree = cssPresentationOwnTree(presentation);
    return prLayoutNodeRenderedText(tree, cssPresentationBox(presentation, element), &desc);
}

PrDomElement* PR_NULLABLE cssViewOffsetParent(PrDomElement* element) {
    PrCssPresentation* presentation = presentationFor(element);
    PrDomDocument* document = element->document;
    if (presentation == nullptr || cssPresentationBox(presentation, element) == kPrLayoutNodeNone ||
            element == domDocumentElement(document) || element == domDocumentBody(document))
        return nullptr;
    const CssComputedStyle* style = cssPresentationStyle(presentation, element);
    PrCssKeyword position = PrCssKeyword_Static;
    if (style != nullptr)
        position = cssComputedFirstKeyword(style, PrCssProperty_Position);
    PrLayoutTree* tree = cssPresentationOwnTree(presentation);
    // The offset parent of a fixed box is the nearest containing block of fixed boxes, or none if no ancestor is one.
    // The offset parent of any other box is the nearest containing block of absolutely positioned boxes, or the body.
    // Layout decides which boxes are containing blocks.
    bool fixed = position == PrCssKeyword_Fixed;
    for (PrDomElement* ancestor = domElementFromNode(element->parent); ancestor != nullptr;
            ancestor = domElementFromNode(ancestor->parent)) {
        PrLayoutNode box = cssPresentationBox(presentation, ancestor);
        if (box == kPrLayoutNodeNone)
            continue;
        if (fixed) {
            if (prLayoutNodeHoldsFixed(tree, box))
                return ancestor;
            continue;
        }
        if (prLayoutNodeHoldsAbsolute(tree, box) || ancestor == domDocumentBody(document))
            return ancestor;
        if (position == PrCssKeyword_Static && (domIsHtml(ancestor, PrDomTag_Td) || domIsHtml(ancestor, PrDomTag_Th) ||
                domIsHtml(ancestor, PrDomTag_Table)))
            return ancestor;
    }
    return nullptr;
}

PrDomRect cssViewOffsetRect(PrDomElement* element) {
    PrCssPresentation* presentation = presentationFor(element);
    if (presentation == nullptr)
        return PrDomRect{};
    PrLayoutNode box = cssPresentationBox(presentation, element);
    PrLayoutRect first{};
    if (box == kPrLayoutNodeNone || !firstBox(presentation, element, &first))
        return PrDomRect{};
    PrLayoutTree* tree = cssPresentationOwnTree(presentation);
    PrLayoutRect bounds = prLayoutNodeBounds(tree, box, PrLayoutCoordinates_Layout);
    PrDomRect rect{px(first.left), px(first.top), px(bounds.right - bounds.left), px(bounds.bottom - bounds.top)};
    // The body's offset is zero, though it has a size (cssom-view-1/#dom-htmlelement-offsettop).
    if (element == domDocumentBody(element->document)) {
        rect.x = 0;
        rect.y = 0;
        return rect;
    }
    // A static body measures from the initial containing block, rather than from its padding box.
    PrDomElement* parent = cssViewOffsetParent(element);
    const CssComputedStyle* parentStyle = parent != nullptr ? cssPresentationStyle(presentation, parent) : nullptr;
    bool fromBody = parent == domDocumentBody(element->document) && (parentStyle == nullptr ||
        cssComputedFirstKeyword(parentStyle, PrCssProperty_Position) == PrCssKeyword_Static);
    PrLayoutRect from{};
    if (parent != nullptr && !fromBody && firstBox(presentation, parent, &from)) {
        PrLayoutNode parentBox = cssPresentationBox(presentation, parent);
        rect.x -= px(from.left + prLayoutNodeBorderWidth(tree, parentBox, PrRectSide_Left));
        rect.y -= px(from.top + prLayoutNodeBorderWidth(tree, parentBox, PrRectSide_Top));
    }
    return rect;
}

} // namespace pr_impl

using namespace pr_impl;

uint32_t prCssElementGetClientRects(PrDomElement* element, PrDomRect* PR_NULLABLE out, uint32_t capacity) {
    PrCssPresentation* presentation = presentationFor(element);
    PrLayoutNode box;
    if (presentation != nullptr)
        box = cssPresentationBox(presentation, element);
    else
        box = PrLayoutNode(kPrLayoutNodeNone);
    // An element of SVG content has no box. Its one rectangle is its bounding box
    // (cssom-view-1/#dom-element-getclientrects).
    const CssPresentationSvg* around =
        box == kPrLayoutNodeNone && presentation != nullptr ? svgAround(presentation, element) : nullptr;
    if (around != nullptr) {
        PrDomRect rect{};
        if (!around->functions->clientRect(around->content, element, &rect))
            return 0;
        if (capacity != 0)
            out[0] = rect;
        return 1;
    }
    if (box == kPrLayoutNodeNone)
        return 0;
    PrLayoutTree* tree = cssPresentationOwnTree(presentation);
    uint32_t count = prLayoutNodeBoxes(tree, box, PrLayoutCoordinates_Viewport, nullptr, 0);
    Array<PrLayoutRect> rects;
    rects.resize(count);
    prLayoutNodeBoxes(tree, box, PrLayoutCoordinates_Viewport, rects.data, count);
    // The content of a foreignObject is a layout root of its own, whose rectangles are measured from its containing
    // block. The SVG content places that block on the page.
    const PrDomElement* foreign = nullptr;
    for (const PrDomNode* at = domFlatParent(element); at != nullptr && foreign == nullptr; at = domFlatParent(at)) {
        const PrDomElement* ancestor = domElementFromNode(at);
        if (ancestor != nullptr && ancestor->space == PrDomNamespace_Svg && ancestor->tag == PrDomTag_ForeignObject)
            foreign = ancestor;
    }
    for (uint32_t i = 0; i < count && i < capacity; ++i) {
        PrDomRect rect = domRect(rects.data[i]);
        const CssPresentationSvg* drawing = foreign != nullptr ? svgAround(presentation, foreign) : nullptr;
        if (drawing != nullptr) {
            PrDVec2 corners[4] = {
                {rect.x, rect.y}, {rect.x + rect.width, rect.y},
                {rect.x + rect.width, rect.y + rect.height}, {rect.x, rect.y + rect.height},
            };
            if (drawing->functions->foreignToClient(drawing->content, foreign, corners, 4)) {
                double left = corners[0].x, top = corners[0].y, right = corners[0].x, bottom = corners[0].y;
                for (const PrDVec2& corner : corners) {
                    left = minF64(left, corner.x);
                    top = minF64(top, corner.y);
                    right = maxF64(right, corner.x);
                    bottom = maxF64(bottom, corner.y);
                }
                rect = PrDomRect{left, top, right - left, bottom - top};
            }
        }
        out[i] = rect;
    }
    rects.reset();
    return count;
}

PrDomRect prCssElementGetBoundingClientRect(PrDomElement* element) {
    uint32_t count = prCssElementGetClientRects(element, nullptr, 0);
    if (count == 0)
        return PrDomRect{};
    Array<PrDomRect> rects;
    rects.resize(count);
    prCssElementGetClientRects(element, rects.data, count);
    PrDomRect bounds = rects.data[0];
    bool any = false;
    for (uint32_t i = 0; i < count; ++i) {
        const PrDomRect& r = rects.data[i];
        // A rect of zero width or height is skipped. If every rect is such a rect, the result is the first rect
        // (cssom-view-1/#dom-element-getboundingclientrect).
        if (r.width == 0 || r.height == 0)
            continue;
        if (!any) {
            bounds = r;
            any = true;
            continue;
        }
        double left = minF64(bounds.x, r.x);
        double top = minF64(bounds.y, r.y);
        double right = maxF64(bounds.x + bounds.width, r.x + r.width);
        double bottom = maxF64(bounds.y + bounds.height, r.y + r.height);
        bounds = PrDomRect{left, top, right - left, bottom - top};
    }
    rects.reset();
    return bounds;
}

double prCssElementScrollTop(PrDomElement* element) {
    PrCssPresentation* presentation = presentationFor(element);
    if (presentation == nullptr)
        return 0;
    switch (scrollingOf(presentation, element)) {
        case Scrolling_None:
            return 0;
        case Scrolling_Viewport:
            return px(cssViewViewportScroll(element->document).y);
        default:
            break;
    }
    PrLayoutScrollInfo info{};
    return scrollInfoOf(presentation, element, &info) ? px(info.offset.y) : 0;
}

double prCssElementScrollLeft(PrDomElement* element) {
    PrCssPresentation* presentation = presentationFor(element);
    if (presentation == nullptr)
        return 0;
    switch (scrollingOf(presentation, element)) {
        case Scrolling_None:
            return 0;
        case Scrolling_Viewport:
            return px(cssViewViewportScroll(element->document).x);
        default:
            break;
    }
    PrLayoutScrollInfo info{};
    return scrollInfoOf(presentation, element, &info) ? px(info.offset.x) : 0;
}

void prCssElementSetScrollTop(PrDomElement* element, double value) {
    PrCssScrollToOptions options{false, true, 0, value};
    scrollElement(element, &options, false);
}

void prCssElementSetScrollLeft(PrDomElement* element, double value) {
    PrCssScrollToOptions options{true, false, value, 0};
    scrollElement(element, &options, false);
}

void prCssElementScroll(PrDomElement* element, const PrCssScrollToOptions* options) {
    scrollElement(element, options, false);
}

void prCssElementScrollBy(PrDomElement* element, const PrCssScrollToOptions* options) {
    scrollElement(element, options, true);
}

void prCssElementScrollIntoView(PrDomElement* element, const PrCssScrollIntoViewOptions* options) {
    PrCssPresentation* presentation = presentationFor(element);
    if (presentation == nullptr || cssPresentationBox(presentation, element) == kPrLayoutNodeNone)
        return;
    // Each scrolling box that the element is in, innermost first, and then the viewport, scrolls the element's border
    // box to the position in the box's scrollport where the options align it. The border box is measured where the
    // earlier scrolls left the element, and where it was laid out in any sticky box around it, or itself: a sticky box
    // stuck at an edge of the scrollport is shown already, and the scroll brings its place in flow into view.
    auto align = [](PrCssScrollLogicalPosition position, double start, double end, double portStart, double portEnd) {
        double size = end - start;
        double portSize = portEnd - portStart;
        switch (position) {
            case PrCssScrollLogicalPosition_Start: return start - portStart;
            case PrCssScrollLogicalPosition_End: return end - portEnd;
            case PrCssScrollLogicalPosition_Center: return (start + end) / 2 - (portStart + portEnd) / 2;
            default:
                // Nearest: no scroll if the box is inside the scrollport or covers it, and otherwise align the edge
                // that moves least.
                if ((start >= portStart && end <= portEnd) || (start <= portStart && end >= portEnd))
                    return 0.0;
                if ((start < portStart && size <= portSize) || (end > portEnd && size > portSize))
                    return start - portStart;
                return end - portEnd;
        }
    };
    PrLayoutTree* tree = cssPresentationOwnTree(presentation);
    for (PrDomElement* ancestor = domElementFromNode(element->parent);; ancestor = domElementFromNode(ancestor->parent)) {
        bool viewport = ancestor == nullptr;
        PrLayoutNode node = viewport ? PrLayoutNode(kPrLayoutNodeNone) : cssPresentationBox(presentation, ancestor);
        PrLayoutScrollInfo info{};
        bool scrolls = node != kPrLayoutNodeNone && prLayoutNodeScrollInfo(tree, node, &info) && info.scrollContainer;
        if (!viewport && !scrolls)
            continue;
        if (viewport && !prLayoutNodeScrollInfo(tree, kPrLayoutNodeNone, &info))
            break;
        PrDomRect target = prCssElementGetBoundingClientRect(element);
        PrLayoutNode box = cssPresentationBox(presentation, element);
        PrLayoutRect laid = prLayoutNodeBounds(tree, box, PrLayoutCoordinates_Layout);
        PrLayoutRect moved = prLayoutNodeBounds(tree, box, PrLayoutCoordinates_Positioned);
        target.x -= px(moved.left - laid.left);
        target.y -= px(moved.top - laid.top);
        PrLayoutRect port{PrLayoutUnit{0}, PrLayoutUnit{0}, info.portExtent.x, info.portExtent.y};
        if (!viewport) {
            PrLayoutRect border = prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Viewport);
            port.left = border.left + prLayoutNodeBorderWidth(tree, node, PrRectSide_Left);
            port.top = border.top + prLayoutNodeBorderWidth(tree, node, PrRectSide_Top);
            port.right = port.left + info.portExtent.x;
            port.bottom = port.top + info.portExtent.y;
        }
        double dx = align(options->inlinePosition, target.x, target.x + target.width, px(port.left), px(port.right));
        double dy = align(options->block, target.y, target.y + target.height, px(port.top), px(port.bottom));
        cssPresentationScrollTo(presentation, node, info.offset + PrLayoutVec{units(dx), units(dy)});
        tree = cssPresentationOwnTree(presentation);
        if (viewport)
            break;
    }
}

int32_t prCssElementScrollWidth(PrDomElement* element) {
    PrCssPresentation* presentation = presentationFor(element);
    if (presentation == nullptr)
        return 0;
    PrLayoutScrollInfo info{};
    bool found;
    if (measuresViewport(element))
        found = prLayoutNodeScrollInfo(cssPresentationOwnTree(presentation), kPrLayoutNodeNone, &info);
    else
        found = scrollInfoOf(presentation, element, &info);
    return found ? integer(px(info.content.right - info.content.left)) : 0;
}

int32_t prCssElementScrollHeight(PrDomElement* element) {
    PrCssPresentation* presentation = presentationFor(element);
    if (presentation == nullptr)
        return 0;
    PrLayoutScrollInfo info{};
    bool found;
    if (measuresViewport(element))
        found = prLayoutNodeScrollInfo(cssPresentationOwnTree(presentation), kPrLayoutNodeNone, &info);
    else
        found = scrollInfoOf(presentation, element, &info);
    return found ? integer(px(info.content.bottom - info.content.top)) : 0;
}

int32_t prCssElementClientTop(PrDomElement* element) {
    PrCssPresentation* presentation = presentationFor(element);
    PrLayoutScrollInfo info{};
    if (presentation == nullptr || !scrollInfoOf(presentation, element, &info))
        return 0;
    PrLayoutNode box = cssPresentationBox(presentation, element);
    return integer(prLayoutUnitToPxDouble(
        prLayoutNodeBorderWidth(cssPresentationOwnTree(presentation), box, PrRectSide_Top)));
}

int32_t prCssElementClientLeft(PrDomElement* element) {
    PrCssPresentation* presentation = presentationFor(element);
    PrLayoutScrollInfo info{};
    if (presentation == nullptr || !scrollInfoOf(presentation, element, &info))
        return 0;
    PrLayoutNode box = cssPresentationBox(presentation, element);
    return integer(prLayoutUnitToPxDouble(
        prLayoutNodeBorderWidth(cssPresentationOwnTree(presentation), box, PrRectSide_Left)));
}

int32_t prCssElementClientWidth(PrDomElement* element) {
    if (measuresViewport(element))
        return integer(px(cssViewViewportExtent(element->document).x));
    PrCssPresentation* presentation = presentationFor(element);
    PrLayoutScrollInfo info{};
    return presentation != nullptr && scrollInfoOf(presentation, element, &info) ? integer(px(info.portExtent.x)) : 0;
}

int32_t prCssElementClientHeight(PrDomElement* element) {
    if (measuresViewport(element))
        return integer(px(cssViewViewportExtent(element->document).y));
    PrCssPresentation* presentation = presentationFor(element);
    PrLayoutScrollInfo info{};
    return presentation != nullptr && scrollInfoOf(presentation, element, &info) ? integer(px(info.portExtent.y)) : 0;
}

PrDomElement* PR_NULLABLE prCssDocumentScrollingElement(PrDomDocument* document) {
    if (document->quirks != DomQuirks_Yes)
        return domDocumentElement(document);
    PrDomElement* body = domDocumentBody(document);
    PrCssPresentation* presentation = presentationFor(document);
    if (body == nullptr || (presentation != nullptr && isPotentiallyScrollable(presentation, body)))
        return nullptr;
    return body;
}

uint32_t prCssDocumentElementsFromPoint(PrDomDocument* document, double x, double y,
        PrDomElement* PR_NULLABLE* PR_NULLABLE out, uint32_t capacity) {
    PrCssPresentation* presentation = presentationFor(document);
    if (presentation == nullptr)
        return 0;
    PrLayoutVec extent = cssViewViewportExtent(document);
    if (!(x >= 0 && y >= 0 && x <= px(extent.x) && y <= px(extent.y)))
        return 0;
    Array<PrDomNode*> nodes;
    cssPresentationHits(presentation, PrLayoutVec{units(x), units(y)}, UINT32_MAX, &nodes);
    Array<PrDomElement*> elements;
    auto add = [&](PrDomElement* element) {
        if (elements.find(element) == kArrayMissing)
            elements.push(element);
    };
    for (uint32_t i = 0; i < nodes.count; ++i) {
        PrDomNode* node = nodes.data[i];
        PrDomElement* element = domElementFromNode(node);
        if (element == nullptr)
            element = domElementFromNode(node->parent);
        // An element of a shadow tree is retargeted against the document: its host stands for it (dom/#retarget).
        while (element != nullptr) {
            const PrDomShadowRoot* shadow = domNodeShadowRoot(domNodeRoot(element));
            if (shadow == nullptr)
                break;
            element = shadow->host;
        }
        if (element != nullptr)
            add(element);
    }
    PrDomElement* root = domDocumentElement(document);
    if (root != nullptr && (elements.count == 0 || elements.last() != root))
        elements.push(root);
    for (uint32_t i = 0; i < elements.count && i < capacity; ++i)
        out[i] = elements.data[i];
    uint32_t count = elements.count;
    nodes.reset();
    elements.reset();
    return count;
}

PrDomElement* PR_NULLABLE prCssDocumentElementFromPoint(PrDomDocument* document, double x, double y) {
    PrDomElement* first = nullptr;
    prCssDocumentElementsFromPoint(document, x, y, &first, 1);
    return first;
}
