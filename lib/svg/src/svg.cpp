// SVG API entry points.

#include "pr/svg.h"

#include "pr/alloc.h"
#include "pr/css.h"
#include "pr/float.h"

#include "css/src/css_boxes.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_tag.h"
#include "svg/src/svg_document.h"
#include "svg/src/svg_hints.h"
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
    float radians = c.direction * (kPiF32 / 180);
    return PrVec2{cosF32(radians), sinF32(radians)};
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

// Calls `fn` with the tree, place and node of the geometry element `element`. Returns InvalidState if no tree draws it
// as a shape.
template <typename Fn>
PrStatus withShape(PrSvgElement* element, Fn fn) {
    SvgQuery query{};
    SvgPlace place{};
    bool found = prSvgInterfaceIsGeometry(prSvgElementInterface(element)) && placedOf(element, &query, &place) &&
        isShape(query.tree->nodes.data[query.node]);
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
    PrRect bounds{kInfinityF32, kInfinityF32, -kInfinityF32, -kInfinityF32};
    float alongs[] = {0, c.advance};
    float acrosses[] = {-c.ascent, c.descent};
    for (float a : alongs) {
        for (float b : acrosses) {
            float x = c.position.x + along.x * a + across.x * b;
            float y = c.position.y + along.y * a + across.y * b;
            bounds = PrRect{
                minF32(bounds.left, x), minF32(bounds.top, y), maxF32(bounds.right, x),
                maxF32(bounds.bottom, y),
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

// What SVG's elements give their boxes (css_boxes.h): an outermost svg element is replaced by its drawing
// (SVG2/render.html#TermReplacedElement).
constexpr CssBoxProvider kBoxes = {
    .tags = kDomTagsSvgBoxes,
    .traits = [](const PrDomElement* element, CssBoxTraits* out) {
        if (domIsOuterSvg(element))
            out->kind = CssBoxKind_Replaced;
    },
    .reads = [](const PrDomElement*, PrStringView) { return false; },
};

} // namespace

// A string attribute of an element.
struct SvgAnimatedString {
    PrSvgElement* element;      // holds a reference
    PrStringView name;          // a string literal, which outlives the object
};

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrSvgAnimatedString : SvgAnimatedString {};

PrSvgElement* PR_NULLABLE prSvgElementQuery(PrDomEventTarget* PR_NULLABLE object) {
    PrDomElement* element = domElementFromNode(domNodeFromTarget(object));
    return element != nullptr && element->space == PrDomNamespace_Svg ? static_cast<PrSvgElement*>(element) : nullptr;
}

PrSvgElement* PR_NULLABLE prSvgElementOwnerSvgElement(PrSvgElement* svgElement) {
    for (PrDomNode* node = domNodeParent(svgElement); node != nullptr; node = domNodeParent(node)) {
        PrDomElement* element = domElementFromNode(node);
        if (element != nullptr && element->space == PrDomNamespace_Svg && element->tag == PrDomTag_Svg)
            return static_cast<PrSvgElement*>(element);
    }
    return nullptr;
}

PrSvgInterface prSvgElementInterface(const PrSvgElement* element) {
    switch (element->tag) {
        case PrDomTag_A: return PrSvgInterface_A;
        case PrDomTag_Animate: return PrSvgInterface_Animate;
        case PrDomTag_AnimateMotion: return PrSvgInterface_AnimateMotion;
        case PrDomTag_AnimateTransform: return PrSvgInterface_AnimateTransform;
        case PrDomTag_Circle: return PrSvgInterface_Circle;
        case PrDomTag_Ellipse: return PrSvgInterface_Ellipse;
        case PrDomTag_Line: return PrSvgInterface_Line;
        case PrDomTag_Path: return PrSvgInterface_Path;
        case PrDomTag_Polygon: return PrSvgInterface_Polygon;
        case PrDomTag_Polyline: return PrSvgInterface_Polyline;
        case PrDomTag_Rect: return PrSvgInterface_Rect;
        case PrDomTag_ForeignObject: return PrSvgInterface_ForeignObject;
        case PrDomTag_G: return PrSvgInterface_Graphics;
        case PrDomTag_Image: return PrSvgInterface_Image;
        case PrDomTag_Script: return PrSvgInterface_Script;
        case PrDomTag_Set: return PrSvgInterface_Set;
        case PrDomTag_Svg: return PrSvgInterface_Svg;
        case PrDomTag_Switch: return PrSvgInterface_Switch;
        case PrDomTag_Symbol: return PrSvgInterface_Symbol;
        case PrDomTag_Use: return PrSvgInterface_Use;
        case PrDomTag_View: return PrSvgInterface_View;
        default: return isTextContent(element) ? PrSvgInterface_TextContent : PrSvgInterface_Element;
    }
}

bool prSvgInterfaceIsGeometry(PrSvgInterface interface) {
    return interface >= PrSvgInterface_Rect && interface <= PrSvgInterface_Polygon;
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
        *out = isFiniteF64(point.x) && isFiniteF64(point.y) && svgShapeFillContains(tree, place, node, at);
    });
}

PrStatus prSvgGeometryElementIsPointInStroke(PrSvgElement* element, PrDomPoint point, bool* out) {
    *out = false;
    PrVec2 at{float(point.x), float(point.y)};
    return withShape(element, [&](const SvgTree* tree, const SvgPlace& place, SvgNodeIndex node) {
        *out = isFiniteF64(point.x) && isFiniteF64(point.y) && svgShapeStrokeContains(tree, place, node, at);
    });
}

PrStatus prSvgGeometryElementGetTotalLength(PrSvgElement* element, float* out) {
    *out = 0;
    return withShape(element, [&](const SvgTree* tree, const SvgPlace& place, SvgNodeIndex node) {
        *out = svgShapeTotalLength(tree, place, node);
    });
}

PrStatus prSvgGeometryElementGetPointAtLength(PrSvgElement* element, float distance, PrDomPoint* out) {
    *out = PrDomPoint{0, 0, 0, 1};
    return withShape(element, [&](const SvgTree* tree, const SvgPlace& place, SvgNodeIndex node) {
        PrVec2 point = svgShapePointAtLength(tree, place, node, distance);
        *out = PrDomPoint{double(point.x), double(point.y), 0, 1};
    });
}

PrSvgAnimatedString* PR_NULLABLE prSvgAnimatedStringCreate(PrSvgElement* element, PrStringView name) {
    bool href = false;
    switch (element->tag) {
        case PrDomTag_A:
        case PrDomTag_Use:
        case PrDomTag_Image:
        case PrDomTag_Script:
        case PrDomTag_TextPath:
        case PrDomTag_LinearGradient:
        case PrDomTag_RadialGradient:
        case PrDomTag_Pattern:
        case PrDomTag_Filter:
        case PrDomTag_FeImage:
        case PrDomTag_Mpath: href = true; break;
        default: break;
    }
    PrStringView literal{};
    if (href && name == "href")
        literal = "href";
    else if (element->tag == PrDomTag_A && name == "target")
        literal = "target";
    if (literal.data == nullptr)
        return nullptr;
    PrSvgAnimatedString* string = allocZeroed<PrSvgAnimatedString>();
    prDomNodeRetain(prSvgElementAsNode(element));
    string->element = element;
    string->name = literal;
    return string;
}

void prSvgAnimatedStringDestroy(PrSvgAnimatedString* PR_NULLABLE string) {
    if (string == nullptr)
        return;
    prDomNodeRelease(prSvgElementAsNode(string->element));
    prMemoryFree(string);
}

PrStringView prSvgAnimatedStringBaseVal(const PrSvgAnimatedString* animated) {
    PrDomElement* element = animated->element;
    PrStringView name = animated->name;
    PrStringView value = name == "href" ? svgHref(element) : svgAttribute(element, name);
    return value.data != nullptr ? value : "";
}

void prSvgAnimatedStringSetBaseVal(PrSvgAnimatedString* animated, PrStringView value) {
    domSetAttribute(animated->element, PrDomNamespace_None, animated->name, value);
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

void prSvgEnable(PrDomDocument* document) {
    prCssEnable(document);
    if (!svgDocumentFollow(document))
        return;
    cssHintsProvide(document, PrDomNamespace_Svg, svgHints());
    cssBoxesProvide(document, PrDomNamespace_Svg, &kBoxes);
}
