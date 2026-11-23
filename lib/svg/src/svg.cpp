// SVG: the documents that it is enabled for, and the events of its animations

#include "pr/svg.h"

#include "pr/alloc.h"
#include "pr/css.h"
#include "pr/float.h"
#include "pr/mathml.h"
#include "pr/memory.h"

#include "css/src/css_boxes.h"
#include "dom/src/dom_event.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_hooks.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_tag.h"
#include "svg/src/svg_animate.h"
#include "svg/src/svg_document.h"
#include "svg/src/svg_hints.h"
#include "svg/src/svg_render.h"
#include "svg/src/svg_text.h"

struct PrSvgTimeEvent : PrDomEvent {};

namespace pr_impl {

namespace {
// The detail of a TimeEvent.
struct TimeEventData : DomEventExtra {
    int32_t detail;
};

TimeEventData* timeDataOf(const PrSvgTimeEvent* event) {
    return static_cast<TimeEventData*>(event->extra);
}

// Appends the text of the text elements under `node` that are rendered. No container that is never rendered holds them
// (SVG2/render.html#TermNeverRenderedElement). A text element's white space is stripped and collapsed.
void appendRenderedText(const PrDomNode* node, bool inText, PrTextBuffer* out) {
    for (const PrDomNode* child = node->first; child != nullptr; child = child->next) {
        if (domIsText(child)) {
            if (inText)
                prTextBufferAppend(out, prRawStringView(&static_cast<const PrDomCharacterData*>(child)->data));
            continue;
        }
        const PrDomElement* element = domElementFromNode(child);
        if (element == nullptr || element->space != PrDomNamespace_Svg)
            continue;
        switch (element->tag) {
            case PrDomTag_Defs:
            case PrDomTag_ClipPath:
            case PrDomTag_Mask:
            case PrDomTag_Marker:
            case PrDomTag_Pattern:
            case PrDomTag_Symbol:
                break;
            default:
                appendRenderedText(element, inText || element->tag == PrDomTag_Text, out);
        }
    }
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
    .usedSize = svgElementUsedSize,
    .replacedText = [](void* PR_NULLABLE element) {
        PrTextBuffer text = {};
        appendRenderedText(static_cast<const PrDomElement*>(element), false, &text);
        return prTextBufferTake(&text);
    },
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// TimeEvent

PrSvgTimeEvent* prSvgTimeEventCreate(PrStringView type, const PrSvgTimeEventInit* init) {
    PrSvgTimeEvent* event = domEventCreate<PrSvgTimeEvent>(type, init->event, PrDomEventKind_Time);
    TimeEventData* data = allocZeroed<TimeEventData>();
    data->destroy = [](DomEventExtra* extra) { prMemoryFree(extra); };
    data->detail = init->detail;
    event->extra = data;
    return event;
}

PrDomEvent* prSvgTimeEventAsEvent(PrSvgTimeEvent* event) {
    return event;
}

PrSvgTimeEvent* PR_NULLABLE prSvgTimeEventFromEvent(PrDomEvent* event) {
    return event->kind == PrDomEventKind_Time ? static_cast<PrSvgTimeEvent*>(event) : nullptr;
}

int32_t prSvgTimeEventDetail(const PrSvgTimeEvent* event) {
    return timeDataOf(event)->detail;
}

void prSvgTimeEventInitTimeEvent(PrSvgTimeEvent* event, PrStringView type, int32_t detail) {
    if (event->dispatching)
        return;
    domEventInitialize(event, type, false, false);
    timeDataOf(event)->detail = detail;
}

void prSvgEnable(PrDomDocument* document) {
    prCssEnable(document);
    if (!svgDocumentFollow(document))
        return;
    cssHintsProvide(document, PrDomNamespace_Svg, svgHints());
    cssBoxesProvide(document, PrDomNamespace_Svg, &kBoxes);
    // MathML reaches SVG content through foreignObject.
    prMathMlEnable(document);
}
