// The content of outermost svg elements in a presentation

#include "css/src/css_presentation_svg.h"

#include "css/src/css_presentation.h"
#include "dom/src/dom_node.h"

namespace pr_impl {

CssPresentationSvg* PR_NONNULL const* PR_NULLABLE cssPresentationSvgs(const PrCssPresentation* presentation,
                                                                      uint32_t* count) {
    *count = presentation->svgs.count;
    return presentation->svgs.data;
}

CssPresentationSvg* PR_NULLABLE cssPresentationSvgOf(const PrCssPresentation* presentation, const PrDomElement* svg) {
    for (uint32_t i = 0; i < presentation->svgs.count; ++i) {
        if (presentation->svgs.data[i]->element == svg)
            return presentation->svgs.data[i];
    }
    return nullptr;
}

void cssPresentationSvgSetNatural(PrCssPresentation* presentation, CssPresentationSvg* svg,
                                  PrLayoutNaturalSize natural) {
    if (natural.width == svg->natural.width && natural.height == svg->natural.height
        && natural.ratio == svg->natural.ratio)
        return;
    svg->natural = natural;
    uint32_t at = cssPresentationNodeEntry(presentation, svg->element);
    if (at == kHashMissing || !presentation->entries.data[at].boxed
        || presentation->entries.data[at].layoutNode == kPrLayoutNodeNone)
        return;
    prLayoutNodeSetNaturalSize(presentation->tree, presentation->entries.data[at].layoutNode, natural);
    presentation->layoutStale = true;
}

PrVec2 cssPresentationSvgExtent(const PrCssPresentation* presentation, const CssPresentationSvg* svg) {
    uint32_t at = cssPresentationNodeEntry(presentation, svg->element);
    if (at == kHashMissing || presentation->entries.data[at].layoutNode == kPrLayoutNodeNone)
        return PrVec2{};
    PrLayoutNode box = presentation->entries.data[at].layoutNode;
    // Before its first layout, the box's extent is its natural size if the element has a width and a height. The
    // painter draws at the extent that layout gives, whatever the content was built for.
    PrLayoutVec extent = prLayoutRectExtent(prLayoutNodeReplacedContent(presentation->tree, box));
    PrLayoutNaturalSize size = svg->natural;
    if (extent.x.raw <= 0 && extent.y.raw <= 0 && size.width.raw > 0 && size.height.raw > 0)
        extent = PrLayoutVec{size.width, size.height};
    return prLayoutVecToPx(extent);
}

void cssPresentationSvgUpdated(PrCssPresentation* presentation, CssPresentationSvg* svg,
                               const CssPresentationSvgUpdate& update) {
    svg->stale = false;
    presentation->accrued.svgNodesBuilt += update.built;
    uint32_t at = cssPresentationNodeEntry(presentation, svg->element);
    if (at == kHashMissing || !presentation->entries.data[at].boxed
        || presentation->entries.data[at].layoutNode == kPrLayoutNodeNone)
        return;
    PrLayoutNode box = presentation->entries.data[at].layoutNode;
    PrLayoutTree* tree = presentation->tree;
    svg->extent = prLayoutRectExtent(prLayoutNodeReplacedContent(tree, box));
    bool laidOut = false;
    bool foreign = update.foreignObjectCount != svg->foreignObjects.count;
    for (uint32_t i = 0; !foreign && i < update.foreignObjectCount; ++i) {
        const CssPresentationSvgForeignObject& was = svg->foreignObjects.data[i];
        const CssPresentationSvgForeignObject& now = update.foreignObjects[i];
        foreign = was.element != now.element || was.root != now.root || was.extent.x != now.extent.x
                  || was.extent.y != now.extent.y;
    }
    if (foreign) {
        svg->foreignObjects.clear();
        for (uint32_t i = 0; i < update.foreignObjectCount; ++i)
            svg->foreignObjects.push(update.foreignObjects[i]);
        laidOut = true;
    }
    PrLayoutContent current = prLayoutNodeContent(tree, box);
    if (current.kind != PrLayoutContentKind_Painter || current.painter.context != svg->content) {
        PrLayoutContent content{.kind = PrLayoutContentKind_Painter,
                                .painter = {.paint = svg->functions->paint, .context = svg->content}};
        prLayoutNodeSetContent(tree, box, &content);
        laidOut = true;
    } else if (update.changed) {
        prLayoutNodeInvalidateContent(tree, box);
        laidOut = true;
    }
    if (laidOut)
        presentation->layoutStale = true;
}

void cssPresentationSvgStale(PrCssPresentation* presentation, CssPresentationSvg* svg) {
    svg->stale = true;
    presentation->changed = true;
}

const CssPresentationSvgNote* PR_NULLABLE cssPresentationSvgNotes(const PrCssPresentation* presentation,
                                                                  uint32_t* count) {
    *count = presentation->svgNotes.count;
    return presentation->svgNotes.data;
}

void cssPresentationClearSvgNotes(PrCssPresentation* presentation) {
    for (uint32_t i = 0; i < presentation->svgNotes.count; ++i)
        domNodeUnhold(presentation->svgNotes.data[i].element);
    presentation->svgNotes.clear();
}

bool cssPresentationTakeSvgInvalid(PrCssPresentation* presentation) {
    bool invalid = presentation->svgInvalid;
    presentation->svgInvalid = false;
    return invalid;
}

void cssPresentationSetReferences(PrCssPresentation* presentation,
                                  const CssPresentationReferences* PR_NULLABLE references) {
    presentation->references = references;
}

void cssPresentationSetSvgState(PrCssPresentation* presentation, void* PR_NULLABLE state) {
    presentation->svgState = state;
}

void* PR_NULLABLE cssPresentationSvgState(const PrCssPresentation* presentation) {
    return presentation->svgState;
}

} // namespace pr_impl
