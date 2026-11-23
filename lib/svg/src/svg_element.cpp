// SVG elements: the interfaces of the elements in the SVG namespace

#include "pr/svg_element.h"

#include "pr/alloc.h"
#include "pr/css.h"
#include "pr/float.h"
#include "pr/mathml.h"
#include "pr/memory.h"
#include "pr/svg_types.h"

#include "css/src/css_boxes.h"
#include "css/src/css_serialize.h"
#include "dom/src/dom_collection.h"
#include "dom/src/dom_event.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_tag.h"
#include "svg/src/svg_animate.h"
#include "svg/src/svg_document.h"
#include "svg/src/svg_draw.h"
#include "svg/src/svg_hints.h"
#include "svg/src/svg_query.h"
#include "svg/src/svg_render.h"
#include "svg/src/svg_text.h"

namespace pr_impl {

namespace {

// The characters of a text content element: the text that holds it, as laid out, and the range of its own.
struct TextQuery {
    SvgQuery query;
    const SvgTextCharacter* PR_NULLABLE characters;
    uint32_t start;
    uint32_t end;
};

bool isTextContent(const PrDomElement* element) {
    return element->space == PrDomNamespace_Svg &&
        (element->tag == PrDomTag_Text || element->tag == PrDomTag_Tspan || element->tag == PrDomTag_TextPath);
}

// Finds the characters of `element`. Returns false, with no characters, if the element is not in a text that renders.
// The caller ends the query in either case.
bool textOf(const PrDomElement* element, TextQuery* out) {
    *out = TextQuery{};
    if (!isTextContent(element))
        return false;
    const PrDomElement* text = element;
    while (text != nullptr && !(text->space == PrDomNamespace_Svg && text->tag == PrDomTag_Text))
        text = domElementFromNode(text->parent);
    if (text == nullptr || !svgQueryBegin(&out->query, text))
        return false;
    const SvgNode& node = out->query.tree->nodes.data[out->query.node];
    if (node.kind != SvgNodeKind_Text)
        return false;
    const SvgTextElement* own = svgTextElementFromElement(node.text, element);
    if (own == nullptr)
        return false;
    out->characters = svgTextCharacters(node.text);
    out->start = own->start;
    out->end = own->end;
    return true;
}

// Returns the character that holds UTF-16 code unit `index` of the query's characters, or kArrayMissing.
uint32_t characterAt(const TextQuery& q, uint32_t index) {
    uint32_t units = 0;
    for (uint32_t i = q.start; i < q.end; ++i) {
        units += q.characters[i].units;
        if (index < units)
            return i;
    }
    return kArrayMissing;
}

// Returns the direction of a character's baseline, and the direction down across it.
PrVec2 alongOf(const SvgTextCharacter& c) {
    float radians = c.direction * (kPrF32Pi / 180);
    return PrVec2{prF32Cos(radians), prF32Sin(radians)};
}

PrVec2 acrossOf(const SvgTextCharacter& c) {
    PrVec2 along = alongOf(c);
    return PrVec2{-along.y, along.x};
}

// Finds where `element` is drawn: its own node, or for a text content element inside a text, the text's node, whose
// user space is the element's. Returns false if no tree draws it. The caller ends the query in either case.
bool placedOf(const PrDomElement* element, SvgQuery* query, SvgPlace* place) {
    const PrDomElement* drawn = element;
    if (isTextContent(element)) {
        while (drawn != nullptr && !(drawn->space == PrDomNamespace_Svg && drawn->tag == PrDomTag_Text))
            drawn = domElementFromNode(drawn->parent);
    }
    return drawn != nullptr && svgQueryBegin(query, drawn) && svgQueryPlace(*query, place);
}

// Returns true if the node is a shape whose equivalent path SVGGeometryElement measures.
bool isShape(const SvgNode& node) {
    return node.kind == SvgNodeKind_Rect || node.kind == SvgNodeKind_Ellipse || node.kind == SvgNodeKind_Line ||
        node.kind == SvgNodeKind_Path;
}

// Calls `fn` with the tree, place and node of the geometry element `element`. An element that is not rendered is built
// as if it were drawn (svgQueryBeginGeometry). Returns InvalidState if the element is not a shape.
template <typename Fn>
PrStatus withShape(PrSvgElement* element, Fn fn) {
    SvgQuery query{};
    SvgPlace place{};
    bool found = prSvgInterfaceIsGeometry(prSvgElementInterface(element)) && svgQueryBeginGeometry(&query, element) &&
        svgQueryPlace(query, &place) && isShape(query.tree->nodes.data[query.node]);
    if (found)
        fn(query.tree, place, query.node);
    svgQueryEnd(&query);
    return found ? PrStatus_Ok : PrStatus_InvalidState;
}

PrDomMatrix matrixOf(PrAffine2 m) {
    PrDomMatrix matrix = prDomMatrixIdentity();
    matrix.m[0] = double(m.a);
    matrix.m[1] = double(m.b);
    matrix.m[4] = double(m.c);
    matrix.m[5] = double(m.d);
    matrix.m[12] = double(m.e);
    matrix.m[13] = double(m.f);
    return matrix;
}

PrDomRect domRectOf(PrRect rect) {
    return PrDomRect{double(rect.left), double(rect.top), double(prRectWidth(rect)), double(prRectHeight(rect))};
}

// Returns the bounds of a character's cell: its advance along its baseline, by the ascent and descent of its font,
// turned by its rotation.
PrRect cellOf(const SvgTextCharacter& c) {
    PrVec2 along = alongOf(c);
    PrVec2 across = acrossOf(c);
    PrRect bounds{kPrF32Infinity, kPrF32Infinity, -kPrF32Infinity, -kPrF32Infinity};
    float alongs[] = {0, c.advance};
    float acrosses[] = {-c.ascent, c.descent};
    for (float a : alongs) {
        for (float b : acrosses) {
            float x = c.position.x + along.x * a + across.x * b;
            float y = c.position.y + along.y * a + across.y * b;
            bounds = PrRect{
                prF32Min(bounds.left, x), prF32Min(bounds.top, y), prF32Max(bounds.right, x),
                prF32Max(bounds.bottom, y),
            };
        }
    }
    return bounds;
}

// Calls `fn` with the character at `index` of `element`. Returns OutOfRange if there is none.
template <typename Fn>
PrStatus withCharacter(PrSvgElement* element, uint32_t index, Fn fn) {
    TextQuery q{};
    textOf(element, &q);
    uint32_t at = q.characters != nullptr ? characterAt(q, index) : kArrayMissing;
    if (at != kArrayMissing)
        fn(q.characters[at]);
    svgQueryEnd(&q.query);
    return at != kArrayMissing ? PrStatus_Ok : PrStatus_OutOfRange;
}

// Returns true if the element is a graphics element (SVG2/struct.html#TermGraphicsElement): one that draws itself, as a
// shape, text, an image or a foreign object does, rather than a container.
bool isGraphics(const PrSvgElement* element) {
    PrSvgInterface interface = prSvgElementInterface(element);
    return prSvgInterfaceIsGeometry(interface) || interface == PrSvgInterface_Text ||
        interface == PrSvgInterface_TSpan || interface == PrSvgInterface_TextPath ||
        interface == PrSvgInterface_Image || interface == PrSvgInterface_ForeignObject;
}

// The rectangle of a hit query (SVGSVGElement.getIntersectionList), as an area in the space that the tree of the
// query's svg element is drawn in.
struct HitQuery {
    const PrDomElement* PR_NULLABLE outermost;  // the outermost svg element of the tree, or null if none draws it
    PrPathPolyline rect;
    bool enclosure;                     // true if an element must lie within the rectangle, not only overlap it
};

// The least area, in square units of the tree's space, that counts as an overlap. Flattened curves and the edges of the
// rectangle may touch without covering any area.
constexpr float kMinHitArea = 1e-3f;

// Returns a query of `rect`, in the user space of `svg`. The caller ends it with hitQueryEnd.
HitQuery hitQueryBegin(PrSvgElement* svg, PrDomRect rect, bool enclosure) {
    HitQuery q{.enclosure = enclosure};
    SvgQuery query{};
    SvgPlace place{};
    if (placedOf(svg, &query, &place)) {
        q.outermost = query.outermost;
        // A negative width or height extends the rectangle the other way.
        PrVec2 a{float(rect.x), float(rect.y)};
        PrVec2 b{float(rect.x + rect.width), float(rect.y + rect.height)};
        prPathPolylineMoveTo(&q.rect, prAffine2MapPoint(place.toTree, a));
        prPathPolylineLineTo(&q.rect, prAffine2MapPoint(place.toTree, PrVec2{b.x, a.y}));
        prPathPolylineLineTo(&q.rect, prAffine2MapPoint(place.toTree, b));
        prPathPolylineLineTo(&q.rect, prAffine2MapPoint(place.toTree, PrVec2{a.x, b.y}));
        prPathPolylineClose(&q.rect);
    }
    svgQueryEnd(&query);
    return q;
}

void hitQueryEnd(HitQuery& q) {
    prPathPolylineReset(&q.rect);
}

// Returns the area that the contours of `polyline` enclose, each counted as positive.
float areaOf(const PrPathPolyline& polyline) {
    float total = 0;
    for (uint32_t c = 0; c < polyline.contourCount; ++c) {
        uint32_t start = polyline.contours[c];
        uint32_t end = prPathPolylineContourEnd(&polyline, c);
        float twice = 0;
        for (uint32_t i = start; i < end; ++i) {
            PrVec2 p = polyline.points[i];
            PrVec2 q = polyline.points[i + 1 < end ? i + 1 : start];
            twice += p.x * q.y - q.x * p.y;
        }
        total += prF32Abs(twice) / 2;
    }
    return total;
}

// Returns true if the graphics element `element` hits the query's rectangle
// (SVG2/struct.html#TermFindIntersectingOrEnclosedDescendants).
bool hits(const HitQuery& q, PrSvgElement* element) {
    SvgQuery query{};
    SvgPlace place{};
    PrPathPolyline area{};
    if (placedOf(element, &query, &place) && query.outermost == q.outermost)
        svgNodeHitArea(query.tree, query.viewport, query.node, element, &area);
    svgQueryEnd(&query);
    bool hit = false;
    if (areaOf(area) > kMinHitArea) {
        PrPathPolyline part{};
        PrPathPolylineOp op = q.enclosure ? PrPathPolylineOp_Difference : PrPathPolylineOp_Intersect;
        prPathPolylineCombine(&area, PrPathFillRule_NonZero, &q.rect, PrPathFillRule_NonZero, op, &part);
        hit = q.enclosure ? !(areaOf(part) > kMinHitArea) : areaOf(part) > kMinHitArea;
        prPathPolylineReset(&part);
    }
    prPathPolylineReset(&area);
    return hit;
}

// Returns true if the node is an svg or a g element, whose children a hit query searches.
bool searchesChildren(const PrDomNode* node) {
    const PrDomElement* element = domElementFromNode(node);
    return element != nullptr && (svgIs(element, PrDomTag_Svg) || svgIs(element, PrDomTag_G));
}

// Appends to `out`, in tree order, the elements among the children of `parent` and what they hold that hit the query's
// rectangle (SVG2/struct.html#TermFindIntersectingOrEnclosedDescendants). An element that is not displayed has no tree
// node, and so hits nothing.
void findHits(const HitQuery& q, PrDomNode* parent, RawArray<PrDomNode*>* out) {
    for (PrDomNode* child = parent->first; child != nullptr; child = child->next) {
        PrSvgElement* element = prSvgElementQuery(prDomNodeAsEventTarget(child));
        if (element == nullptr)
            continue;
        if (searchesChildren(child)) {
            findHits(q, child, out);
        } else if (svgIs(element, PrDomTag_Use)) {
            // A use element hits in place of its instances, which are not reported.
            PrDomShadowRoot* shadow = domShadowRootOfHost(element);
            RawArray<PrDomNode*> instances;
            if (shadow != nullptr)
                findHits(q, shadow, &instances);
            if (instances.count != 0)
                out->push(child);
            instances.reset();
        } else if (isGraphics(element) && hits(q, element)) {
            out->push(child);
        }
    }
}

// Appends to `out` the graphics elements that `node` is or holds through svg and g elements, in tree order
// (SVG2/struct.html#TermFindNonContainerGraphicsElements).
void findGraphics(PrDomNode* node, RawArray<PrDomNode*>* out) {
    if (searchesChildren(node)) {
        for (PrDomNode* child = node->first; child != nullptr; child = child->next)
            findGraphics(child, out);
        return;
    }
    PrSvgElement* element = prSvgElementQuery(prDomNodeAsEventTarget(node));
    if (element != nullptr && isGraphics(element))
        out->push(node);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrSvgElement* PR_NULLABLE prSvgElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    PrDomElement* element = domElementFromNode(domNodeFromTarget(object));
    return element != nullptr && element->space == PrDomNamespace_Svg ? static_cast<PrSvgElement*>(element) : nullptr;
}

PrSvgElement* PR_NULLABLE prSvgElementOwnerSvgElement(PrSvgElement* svgElement) {
    for (PrDomNode* node = domNodeParent(svgElement); node != nullptr; node = domNodeParent(node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element == nullptr || element->space != PrDomNamespace_Svg || element->tag == PrDomTag_ForeignObject)
            return nullptr;
        if (element->tag == PrDomTag_Svg)
            return static_cast<PrSvgElement*>(element);
    }
    return nullptr;
}

float prSvgElementCurrentScale(const PrSvgElement* svg) {
    float zoom = svgIsMagnified(svg) ? svg->state->svgZoom : 0;
    return zoom != 0 ? zoom : 1;
}

void prSvgElementSetCurrentScale(PrSvgElement* svg, float scale) {
    if (!svgIsOutermostSvg(svg))
        return;
    domElementState(svg)->svgZoom = scale;
    svgContentMagnified(svg);
}

PrVec2 prSvgElementCurrentTranslate(const PrSvgElement* svg) {
    return svgIsMagnified(svg) ? svg->state->svgPan : PrVec2{};
}

void prSvgElementSetCurrentTranslate(PrSvgElement* svg, PrVec2 translate) {
    if (!svgIsOutermostSvg(svg))
        return;
    DomElementState* state = domElementState(svg);
    if (state->svgZoom == 0)
        state->svgZoom = 1;
    state->svgPan = translate;
    svgContentMagnified(svg);
}

PrSvgInterface prSvgElementInterface(const PrSvgElement* element) {
    switch (element->tag) {
        case PrDomTag_A: return PrSvgInterface_A;
        case PrDomTag_Animate: return PrSvgInterface_Animate;
        case PrDomTag_AnimateMotion: return PrSvgInterface_AnimateMotion;
        case PrDomTag_AnimateTransform: return PrSvgInterface_AnimateTransform;
        case PrDomTag_Circle: return PrSvgInterface_Circle;
        case PrDomTag_ClipPath: return PrSvgInterface_ClipPath;
        case PrDomTag_Defs: return PrSvgInterface_Defs;
        case PrDomTag_Desc: return PrSvgInterface_Desc;
        case PrDomTag_Ellipse: return PrSvgInterface_Ellipse;
        case PrDomTag_FeBlend: return PrSvgInterface_FeBlend;
        case PrDomTag_FeColorMatrix: return PrSvgInterface_FeColorMatrix;
        case PrDomTag_FeComponentTransfer: return PrSvgInterface_FeComponentTransfer;
        case PrDomTag_FeComposite: return PrSvgInterface_FeComposite;
        case PrDomTag_FeConvolveMatrix: return PrSvgInterface_FeConvolveMatrix;
        case PrDomTag_FeDiffuseLighting: return PrSvgInterface_FeDiffuseLighting;
        case PrDomTag_FeDisplacementMap: return PrSvgInterface_FeDisplacementMap;
        case PrDomTag_FeDistantLight: return PrSvgInterface_FeDistantLight;
        case PrDomTag_FeDropShadow: return PrSvgInterface_FeDropShadow;
        case PrDomTag_FeFlood: return PrSvgInterface_FeFlood;
        case PrDomTag_FeFuncA: return PrSvgInterface_FeFuncA;
        case PrDomTag_FeFuncB: return PrSvgInterface_FeFuncB;
        case PrDomTag_FeFuncG: return PrSvgInterface_FeFuncG;
        case PrDomTag_FeFuncR: return PrSvgInterface_FeFuncR;
        case PrDomTag_FeGaussianBlur: return PrSvgInterface_FeGaussianBlur;
        case PrDomTag_FeImage: return PrSvgInterface_FeImage;
        case PrDomTag_FeMerge: return PrSvgInterface_FeMerge;
        case PrDomTag_FeMergeNode: return PrSvgInterface_FeMergeNode;
        case PrDomTag_FeMorphology: return PrSvgInterface_FeMorphology;
        case PrDomTag_FeOffset: return PrSvgInterface_FeOffset;
        case PrDomTag_FePointLight: return PrSvgInterface_FePointLight;
        case PrDomTag_FeSpecularLighting: return PrSvgInterface_FeSpecularLighting;
        case PrDomTag_FeSpotLight: return PrSvgInterface_FeSpotLight;
        case PrDomTag_FeTile: return PrSvgInterface_FeTile;
        case PrDomTag_FeTurbulence: return PrSvgInterface_FeTurbulence;
        case PrDomTag_Filter: return PrSvgInterface_Filter;
        case PrDomTag_ForeignObject: return PrSvgInterface_ForeignObject;
        case PrDomTag_G: return PrSvgInterface_G;
        case PrDomTag_Image: return PrSvgInterface_Image;
        case PrDomTag_Line: return PrSvgInterface_Line;
        case PrDomTag_LinearGradient: return PrSvgInterface_LinearGradient;
        case PrDomTag_Marker: return PrSvgInterface_Marker;
        case PrDomTag_Mask: return PrSvgInterface_Mask;
        case PrDomTag_Metadata: return PrSvgInterface_Metadata;
        case PrDomTag_Mpath: return PrSvgInterface_MPath;
        case PrDomTag_Path: return PrSvgInterface_Path;
        case PrDomTag_Pattern: return PrSvgInterface_Pattern;
        case PrDomTag_Polygon: return PrSvgInterface_Polygon;
        case PrDomTag_Polyline: return PrSvgInterface_Polyline;
        case PrDomTag_RadialGradient: return PrSvgInterface_RadialGradient;
        case PrDomTag_Rect: return PrSvgInterface_Rect;
        case PrDomTag_Script: return PrSvgInterface_Script;
        case PrDomTag_Set: return PrSvgInterface_Set;
        case PrDomTag_Stop: return PrSvgInterface_Stop;
        case PrDomTag_Style: return PrSvgInterface_Style;
        case PrDomTag_Svg: return PrSvgInterface_Svg;
        case PrDomTag_Switch: return PrSvgInterface_Switch;
        case PrDomTag_Symbol: return PrSvgInterface_Symbol;
        case PrDomTag_Text: return PrSvgInterface_Text;
        case PrDomTag_TextPath: return PrSvgInterface_TextPath;
        case PrDomTag_Title: return PrSvgInterface_Title;
        case PrDomTag_Tspan: return PrSvgInterface_TSpan;
        case PrDomTag_Use: return PrSvgInterface_Use;
        case PrDomTag_View: return PrSvgInterface_View;
        default: return PrSvgInterface_Element;
    }
}

bool prSvgInterfaceIsGeometry(PrSvgInterface interface) {
    return interface >= PrSvgInterface_Path && interface <= PrSvgInterface_Polygon;
}

PrSvgBoundingBoxOptions prSvgBoundingBoxOptionsDefault(void) {
    return PrSvgBoundingBoxOptions{.fill = true};
}

PrDomRect prSvgGraphicsElementGetBBox(PrSvgElement* element, const PrSvgBoundingBoxOptions* PR_NULLABLE options) {
    PrSvgBoundingBoxOptions own = options != nullptr ? *options : prSvgBoundingBoxOptionsDefault();
    PrRect box{};
    if (isTextContent(element) && !(element->tag == PrDomTag_Text)) {
        // A tspan or textPath measures the cells of its characters, in the text's user space.
        TextQuery q{};
        textOf(element, &q);
        bool any = false;
        for (uint32_t i = q.start; q.characters != nullptr && i < q.end; ++i) {
            if (q.characters[i].units == 0 || q.characters[i].hidden)
                continue;
            PrRect rect = cellOf(q.characters[i]);
            box = any ? prRectUnion(box, rect) : rect;
            any = true;
        }
        svgQueryEnd(&q.query);
        return domRectOf(box);
    }
    SvgQuery query{};
    SvgPlace place{};
    if (placedOf(element, &query, &place))
        box = svgNodeBox(query.tree, place, query.node, own);
    svgQueryEnd(&query);
    return domRectOf(box);
}

bool prSvgGraphicsElementGetCtm(PrSvgElement* element, PrDomMatrix* out) {
    SvgQuery query{};
    SvgPlace place{};
    bool found = placedOf(element, &query, &place);
    if (found)
        *out = matrixOf(place.ctm);
    svgQueryEnd(&query);
    return found;
}

bool prSvgGraphicsElementGetScreenCtm(PrSvgElement* element, PrDomMatrix* out) {
    SvgQuery query{};
    SvgPlace place{};
    bool found = placedOf(element, &query, &place);
    if (found) {
        // The user space's origin and unit vectors, mapped to the viewport, give the affine transform.
        PrVec2 user[3] = {{0, 0}, {1, 0}, {0, 1}};
        PrDVec2 points[3];
        for (uint32_t i = 0; i < 3; ++i) {
            PrVec2 mapped = prAffine2MapPoint(place.toTree, user[i]);
            points[i] = PrDVec2{double(mapped.x), double(mapped.y)};
        }
        found = svgQueryToClient(query, points, 3);
        if (found) {
            PrDomMatrix matrix = prDomMatrixIdentity();
            matrix.m[0] = points[1].x - points[0].x;
            matrix.m[1] = points[1].y - points[0].y;
            matrix.m[4] = points[2].x - points[0].x;
            matrix.m[5] = points[2].y - points[0].y;
            matrix.m[12] = points[0].x;
            matrix.m[13] = points[0].y;
            *out = matrix;
        }
    }
    svgQueryEnd(&query);
    return found;
}

PrStatus prSvgGeometryElementIsPointInFill(PrSvgElement* element, PrDomPoint point, bool* out) {
    *out = false;
    PrVec2 at{float(point.x), float(point.y)};
    return withShape(element, [&](const SvgTree* tree, const SvgPlace& place, SvgNodeIndex node) {
        *out = prF64IsFinite(point.x) && prF64IsFinite(point.y) && svgShapeFillContains(tree, place, node, at);
    });
}

PrStatus prSvgGeometryElementIsPointInStroke(PrSvgElement* element, PrDomPoint point, bool* out) {
    *out = false;
    PrVec2 at{float(point.x), float(point.y)};
    return withShape(element, [&](const SvgTree* tree, const SvgPlace& place, SvgNodeIndex node) {
        *out = prF64IsFinite(point.x) && prF64IsFinite(point.y) && svgShapeStrokeContains(tree, place, node, at);
    });
}

float prSvgGeometryElementTotalLength(PrSvgElement* element) {
    PR_ASSERT(prSvgInterfaceIsGeometry(prSvgElementInterface(element)));
    // A path whose data is empty or in error from its start has no shape node, and no length.
    float length = 0;
    withShape(element, [&](const SvgTree* tree, const SvgPlace& place, SvgNodeIndex node) {
        length = svgShapeTotalLength(tree, place, node);
    });
    return length;
}

PrStatus prSvgGeometryElementGetPointAtLength(PrSvgElement* element, float distance, PrDomPoint* out) {
    *out = PrDomPoint{0, 0, 0, 1};
    bool found = false;
    PrStatus status = withShape(element, [&](const SvgTree* tree, const SvgPlace& place, SvgNodeIndex node) {
        PrVec2 point{};
        found = svgShapePointAtLength(tree, place, node, distance, &point);
        *out = PrDomPoint{double(point.x), double(point.y), 0, 1};
    });
    return status == PrStatus_Ok && !found ? PrStatus(PrStatus_InvalidState) : status;
}

uint32_t prSvgTextContentElementGetNumberOfChars(PrSvgElement* element) {
    TextQuery q{};
    textOf(element, &q);
    uint32_t units = 0;
    for (uint32_t i = q.start; i < q.end; ++i)
        units += q.characters[i].units;
    svgQueryEnd(&q.query);
    return units;
}

float prSvgTextContentElementGetComputedTextLength(PrSvgElement* element) {
    TextQuery q{};
    textOf(element, &q);
    float length = 0;
    for (uint32_t i = q.start; i < q.end; ++i)
        length += q.characters[i].advance;
    svgQueryEnd(&q.query);
    return length;
}

PrStatus prSvgTextContentElementGetSubStringLength(PrSvgElement* element, uint32_t first, uint32_t count, float* out) {
    TextQuery q{};
    textOf(element, &q);
    uint32_t from = q.characters != nullptr ? characterAt(q, first) : kArrayMissing;
    *out = 0;
    if (from != kArrayMissing) {
        // The characters whose first code units are in the range.
        uint32_t end = count > UINT32_MAX - first ? UINT32_MAX : first + count;
        uint32_t units = 0;
        for (uint32_t i = q.start; i < q.end; ++i) {
            if (units >= first && units < end)
                *out += q.characters[i].advance;
            units += q.characters[i].units;
        }
    }
    svgQueryEnd(&q.query);
    return from != kArrayMissing ? PrStatus_Ok : PrStatus_OutOfRange;
}

PrStatus prSvgTextContentElementGetStartPositionOfChar(PrSvgElement* element, uint32_t index, PrDomPoint* out) {
    return withCharacter(element, index, [&](const SvgTextCharacter& c) {
        *out = PrDomPoint{double(c.position.x), double(c.position.y), 0, 1};
    });
}

PrStatus prSvgTextContentElementGetEndPositionOfChar(PrSvgElement* element, uint32_t index, PrDomPoint* out) {
    return withCharacter(element, index, [&](const SvgTextCharacter& c) {
        PrVec2 along = alongOf(c);
        *out = PrDomPoint{double(c.position.x + along.x * c.advance), double(c.position.y + along.y * c.advance), 0, 1};
    });
}

PrStatus prSvgTextContentElementGetExtentOfChar(PrSvgElement* element, uint32_t index, PrDomRect* out) {
    return withCharacter(element, index, [&](const SvgTextCharacter& c) { *out = domRectOf(cellOf(c)); });
}

PrStatus prSvgTextContentElementGetRotationOfChar(PrSvgElement* element, uint32_t index, float* out) {
    return withCharacter(element, index, [&](const SvgTextCharacter& c) { *out = c.rotate; });
}

int32_t prSvgTextContentElementGetCharNumAtPosition(PrSvgElement* element, PrDomPoint point) {
    TextQuery q{};
    textOf(element, &q);
    int32_t found = -1;
    uint32_t units = 0;
    for (uint32_t i = q.start; i < q.end && found < 0; ++i) {
        const SvgTextCharacter& c = q.characters[i];
        PrVec2 along = alongOf(c);
        PrVec2 across = acrossOf(c);
        float x = float(point.x) - c.position.x;
        float y = float(point.y) - c.position.y;
        float a = x * along.x + y * along.y;
        float b = x * across.x + y * across.y;
        if (!c.hidden && a >= 0 && a <= c.advance && b >= -c.ascent && b <= c.descent)
            found = int32_t(units);
        units += c.units;
    }
    svgQueryEnd(&q.query);
    return found;
}

PrStatus prSvgTextContentElementSelectSubString(PrSvgElement* element, uint32_t index, uint32_t count) {
    if (index >= prSvgTextContentElementGetNumberOfChars(element))
        return PrStatus_OutOfRange;
    PrDomRange* range = prDomRangeCreate(element->document);
    uint64_t end = uint64_t(index) + count;
    uint64_t at = 0;
    bool started = false;
    PrDomNode* last = nullptr;
    for (PrDomNode* node = domNodeNext(element, element); node != nullptr; node = domNodeNext(element, node)) {
        if (prDomNodeType(node) != PrDomNodeType_Text)
            continue;
        PrDomCharacterData* text = prDomCharacterDataQuery(prDomNodeAsEventTarget(node));
        uint32_t length = prDomCharacterDataLength(text);
        if (!started && index < at + length) {
            prDomRangeSetStart(range, node, uint32_t(index - at));
            started = true;
        }
        if (started && end <= at + length) {
            prDomRangeSetEnd(range, node, uint32_t(end - at));
            last = nullptr;
            break;
        }
        at += length;
        last = node;
    }
    // A count past the end selects to the end of the element's text.
    if (started && last != nullptr)
        prDomRangeSetEnd(range, last, prDomCharacterDataLength(prDomCharacterDataQuery(prDomNodeAsEventTarget(last))));
    PrDomSelection* selection = prDomDocumentSelection(element->document);
    prDomSelectionRemoveAllRanges(selection);
    if (started)
        prDomSelectionAddRange(selection, range);
    prDomRangeRelease(range);
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// SVGSVGElement's time container and SVGAnimationElement

void prSvgSvgElementPauseAnimations(PrSvgElement* svgElement) {
    svgAnimationsPause(svgElement, true);
}

void prSvgSvgElementUnpauseAnimations(PrSvgElement* svgElement) {
    svgAnimationsPause(svgElement, false);
}

bool prSvgSvgElementAnimationsPaused(PrSvgElement* svgElement) {
    return svgAnimationsPaused(svgElement);
}

float prSvgSvgElementGetCurrentTime(PrSvgElement* svgElement) {
    return float(svgAnimationsTime(svgElement));
}

void prSvgSvgElementSetCurrentTime(PrSvgElement* svgElement, float seconds) {
    svgAnimationsSeek(svgElement, double(seconds));
}

bool prSvgSvgElementCheckHit(PrSvgElement* svgElement, PrSvgElement* element, PrDomRect rect, bool enclosure) {
    RawArray<PrDomNode*> elements;
    findGraphics(element, &elements);
    HitQuery q = hitQueryBegin(svgElement, rect, enclosure);
    RawArray<PrDomNode*> found;
    if (q.outermost != nullptr)
        findHits(q, svgElement, &found);
    bool hit = elements.count != 0;
    for (PrDomNode* node : elements.span())
        hit = hit && found.find(node) != kArrayMissing;
    hitQueryEnd(q);
    found.reset();
    elements.reset();
    return hit;
}

void prSvgSvgElementGetHitList(PrSvgElement* svgElement, PrDomRect rect, PrSvgElement* PR_NULLABLE reference,
        bool enclosure, PrDomNodeList* out) {
    HitQuery q = hitQueryBegin(svgElement, rect, enclosure);
    RawArray<PrDomNode*> found;
    if (q.outermost != nullptr)
        findHits(q, svgElement, &found);
    hitQueryEnd(q);
    if (reference != nullptr) {
        uint32_t kept = 0;
        for (PrDomNode* node : found.span()) {
            if (node != reference && prDomNodeContains(reference, node))
                found.data[kept++] = node;
        }
        found.count = kept;
    }
    domNodeListAssign(out, found.data, found.count);
    found.reset();
}

PrSvgElement* PR_NULLABLE prSvgAnimationElementTargetElement(PrSvgElement* animation) {
    PrDomElement* target = svgAnimationTarget(animation);
    return target != nullptr && target->space == PrDomNamespace_Svg ? static_cast<PrSvgElement*>(target) : nullptr;
}

PrStatus prSvgAnimationElementGetStartTime(PrSvgElement* animation, float* out) {
    double time = 0;
    if (!svgAnimationStartTime(animation, &time))
        return PrStatus_InvalidState;
    *out = float(time);
    return PrStatus_Ok;
}

float prSvgAnimationElementGetCurrentTime(PrSvgElement* animation) {
    return float(svgAnimationTime(animation));
}

PrStatus prSvgAnimationElementGetSimpleDuration(PrSvgElement* animation, float* out) {
    double duration = 0;
    if (!svgAnimationSimpleDuration(animation, &duration))
        return PrStatus_NotSupported;
    *out = float(duration);
    return PrStatus_Ok;
}

void prSvgAnimationElementBeginElementAt(PrSvgElement* animation, float offset) {
    svgAnimationAddTime(animation, false, double(offset));
}

void prSvgAnimationElementEndElementAt(PrSvgElement* animation, float offset) {
    svgAnimationAddTime(animation, true, double(offset));
}
