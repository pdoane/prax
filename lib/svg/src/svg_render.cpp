// Rendering SVG: the content of the outermost svg elements in a presentation, and of the documents that have no
// presentation. Both build an SVG tree from computed styles and draw it. A presentation's styles come from its own
// cascade, and a document that has no presentation is styled by a cascade of its own (svg_cascade.h).

#include "pr/svg_render.h"
#include "svg/src/svg_render.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/float.h"

#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_root.h"
#include "dom/src/dom_flat.h"
#include "svg/src/svg_cascade.h"
#include "svg/src/svg_draw.h"
#include "svg/src/svg_filter.h"
#include "svg/src/svg_image.h"
#include "svg/src/svg_query.h"

namespace pr_impl {

// The content of an outermost svg element in a presentation: its SVG tree, built from the presentation's styles, and
// the changes that its next update brings it up to date with.
struct SvgContent {
    PrCssPresentation* presentation;
    const PrDomElement* svg;
    SvgTree tree;
    SvgChanges changes;
    PrVec2 extent;              // the viewport that the tree was built in
};

struct SvgReferenceSource;

// The clipPaths, masks and filters that the effects of a presentation's boxes name by URL: the resources of a tree that
// holds no node, built from the presentation's styles, the changes that bring them up to date, and the sources that the
// boxes' groups take them as, by resource.
struct SvgReferences {
    PrCssPresentation* presentation;
    SvgTree tree;
    SvgChanges changes;
    Array<SvgReferenceSource*> sources;     // by SvgResourceIndex, or null

    // The number of times that the resources were built again, which a source takes as its version, so that a group
    // that names it compares unequal once what it draws has changed.
    uint32_t version;
    bool named;                             // a box has named one of them, so their changes are followed
};

// A resource of the references as a layout source. The groups that name it hold references to it. Once none does, the
// next update frees it and its resource.
struct SvgReferenceSource {
    SvgReferences* references;
    SvgResourceIndex resource;
    Atomic<uint32_t> count;
};

// What lib/svg keeps for a presentation (cssPresentationSvgState): the references that its boxes name, and what
// resolves them for the presentation. The content of its outermost svg elements is in its records (CssPresentationSvg).
struct SvgPresentation {
    PrCssPresentation* presentation;
    PrDomDocument* document;
    bool follows;                           // the state subscribes to the document's changes
    SvgReferences references;
    CssPresentationReferences resolver;
    bool reprojected;                       // the references projected boxes again during the current update
    bool updating;                          // svgPresentationUpdate is bringing the content up to date
};

namespace {

const CssComputedStyle* PR_NULLABLE presentationStyle(void* PR_NULLABLE context, const PrDomElement* element) {
    return cssPresentationStyle(static_cast<PrCssPresentation*>(context), element);
}

const CssComputedStyle* presentationStyleUnder(void* PR_NULLABLE context, const PrDomElement* element,
        const CssComputedStyle* PR_NULLABLE parent) {
    return cssPresentationStyleUnder(static_cast<PrCssPresentation*>(context), element, parent);
}

const CssComputedStyle* PR_NULLABLE presentationPseudoStyleUnder(void* PR_NULLABLE context,
        const PrDomElement* element, CssPseudo pseudo, const CssComputedStyle* PR_NULLABLE parent) {
    return cssPresentationPseudoStyleUnder(static_cast<PrCssPresentation*>(context), element, pseudo, parent);
}

const CssComputedStyle* PR_NULLABLE cascadeStyle(void* PR_NULLABLE context, const PrDomElement* element) {
    return svgCascadeStyle(static_cast<SvgCascade*>(context), element);
}

const CssComputedStyle* cascadeStyleUnder(void* PR_NULLABLE context, const PrDomElement* element,
        const CssComputedStyle* PR_NULLABLE parent) {
    return svgCascadeStyleUnder(static_cast<SvgCascade*>(context), element, parent);
}

const CssComputedStyle* PR_NULLABLE cascadePseudoStyleUnder(void* PR_NULLABLE context, const PrDomElement* element,
        CssPseudo pseudo, const CssComputedStyle* PR_NULLABLE parent) {
    return svgCascadePseudoStyleUnder(static_cast<SvgCascade*>(context), element, pseudo, parent);
}

// Writes the image that an image or feImage element of a presented document shows: the image of its current request,
// which its browsing context loads (domEmbeds).
bool loadedImage(void* PR_NULLABLE, const PrDomElement* element, PrStringView, SvgPicture* out) {
    const DomElementState* state = element->state;
    if (state == nullptr || state->image != PrDomImageState_CompletelyAvailable)
        return false;
    if (state->picture != nullptr) {
        prDrawImageRetain(state->picture);
        out->raster = state->picture;
        return true;
    }
    const SvgImage* svg = svgImageOfPainter(state->vector);
    if (svg == nullptr)
        return false;
    svgImageRetain(svg);
    out->svg = svg;
    return true;
}

// Returns the layout root that the box of a foreignObject's content is, in the presentation of its document.
PrLayoutNode presentationForeignRoot(void* PR_NULLABLE context, const PrDomElement* element) {
    return prCssPresentationLayoutNode(static_cast<PrCssPresentation*>(context), element);
}

// Draws a layout root of the presentation's tree, which the presentation's paint drew before the content that draws it.
void drawPresentationRoot(void* PR_NULLABLE context, PrLayoutNode root, PrDrawList* list) {
    prLayoutNodeDraw(static_cast<PrCssPresentation*>(context)->tree, root, list);
}

// Writes the image that an image element of a document that has no presentation names, as the host's loader in the
// options that `context` points to loads it.
bool hostImage(void* PR_NULLABLE context, const PrDomElement* element, PrStringView url, SvgPicture* out) {
    const PrSvgDrawOptions* options = static_cast<const PrSvgDrawOptions*>(context);
    PrSvgLoadedImage loaded{};
    if (options->loadImage == nullptr ||
            !options->loadImage(options->loadContext, const_cast<PrDomDocument*>(element->document), url, &loaded))
        return false;
    out->raster = loaded.raster;
    out->svg = loaded.svg;
    return out->raster != nullptr || out->svg != nullptr;
}

// Returns the source of the images of a document that has no presentation, which `options` loads.
SvgImageSource hostImages(const PrSvgDrawOptions* options) {
    return SvgImageSource{.load = hostImage, .context = const_cast<PrSvgDrawOptions*>(options)};
}

// Returns the natural dimensions of an outermost svg element whose style is `style` (SVG2/coords.html#SizingSVGInCSS):
// its width and height attributes if they are absolute lengths, and the ratio of its width and height if it has both,
// or else of its viewBox. The attributes give them even where a style sets the properties that they are presentation
// attributes of to auto, so that such an svg keeps the size that its markup gives it.
PrSvgNaturalSize naturalSizeOf(const PrDomElement* svg, const CssComputedStyle* PR_NULLABLE style) {
    PrSvgNaturalSize size{-1, -1, 0};
    SvgLengthPercentage none{-1, 0};
    SvgLengthPercentage width = svgAttributeLength(svg, "width", none, style);
    SvgLengthPercentage height = svgAttributeLength(svg, "height", none, style);
    if (width.percent == 0 && width.px >= 0)
        size.width = width.px;
    if (height.percent == 0 && height.px >= 0)
        size.height = height.px;
    PrRect viewBox{};
    if (size.width > 0 && size.height > 0)
        size.ratio = size.width / size.height;
    else if (svgParseViewBox(svgAttribute(svg, "viewBox"), &viewBox) && viewBox.right > viewBox.left &&
            viewBox.bottom > viewBox.top)
        size.ratio = prRectWidth(viewBox) / prRectHeight(viewBox);
    return size;
}

SvgContent* contentCreate(PrCssPresentation* presentation, const PrDomElement* svg) {
    SvgContent* content = allocZeroed<SvgContent>();
    content->presentation = presentation;
    content->svg = svg;
    return content;
}

void contentRelease(void* content) {
    SvgContent* own = static_cast<SvgContent*>(content);
    svgTreeReset(&own->tree);
    svgChangesReset(&own->changes);
    prMemoryFree(own);
}

// Returns true if the content has changes that its next update brings in.
bool contentPending(const SvgContent* content) {
    return content->changes.all || content->changes.ids || content->changes.dirt.count != 0;
}

// Returns what a build of the content reads: the presentation's styles, its colors, and its fonts, which `options`
// holds.
SvgBuild buildOf(const SvgContent* content, PrSvgDrawOptions* options) {
    PrCssPresentation* presentation = content->presentation;
    *options = prSvgDrawOptionsDefault();
    options->fonts = presentation->fonts;
    options->shaper = presentation->shaper;
    return SvgBuild{
        .source = {presentationStyle, presentationStyleUnder, presentationPseudoStyleUnder, presentation},
        .images = {loadedImage, nullptr},
        .foreign = {presentationForeignRoot, presentation},
        .environment = cssPresentationColorEnvironment(presentation),
        .options = options,
        .viewport = content->extent,
    };
}

// Returns true if a viewBox gives the root of the tree a user space of its own, in which what the tree builds reads
// nothing of the extent that it is drawn at.
bool sizedByViewBox(const SvgTree& tree) {
    return tree.nodes.count != 0 && tree.nodes.data[0].kind == SvgNodeKind_Viewport &&
        tree.nodes.data[0].viewport.hasViewBox;
}

// Brings the content up to date for a box of `extent`, and writes to `foreign` the foreignObject elements whose content
// it draws. A foreignObject with no layout root, or whose rectangle has no area, is not among them.
CssPresentationSvgUpdate contentUpdate(SvgContent* content, PrVec2 extent, Array<CssPresentationSvgForeignObject>* foreign) {
    // The tree's text resolves its percentages against the extent, unless a viewBox gives the root a size of its own.
    const SvgTree& tree = content->tree;
    if (extent.x != content->extent.x || extent.y != content->extent.y) {
        if (!sizedByViewBox(tree))
            content->changes.all = true;
        content->extent = extent;
    }
    PrSvgDrawOptions options{};
    SvgBuild build = buildOf(content, &options);
    SvgUpdate update = svgTreeUpdate(&content->tree, content->svg, &content->changes, build);
    for (uint32_t i = 0; i < tree.nodes.count; ++i) {
        const SvgNode& node = tree.nodes.data[i];
        if (node.kind == SvgNodeKind_ForeignObject && node.foreign.root != kPrLayoutNodeNone &&
                node.foreign.extent.x > 0 && node.foreign.extent.y > 0)
            foreign->push(CssPresentationSvgForeignObject{node.element, node.foreign.root, node.foreign.extent});
    }
    return CssPresentationSvgUpdate{
        .changed = update.changed,
        .built = update.built,
        .foreignObjects = foreign->data,
        .foreignObjectCount = foreign->count,
    };
}

void contentPaint(void* PR_NULLABLE context, PrVec2 extent, PrDrawList* list) {
    const SvgContent* content = static_cast<const SvgContent*>(context);
    SvgDrawHost host{.drawRoot = drawPresentationRoot, .context = content->presentation};
    svgTreeDraw(&content->tree, extent, &host, list);
}

uint32_t contentHitTest(const void* content, PrVec2 point, CssPresentationSvgHit* PR_NULLABLE out, uint32_t capacity) {
    const SvgContent* own = static_cast<const SvgContent*>(content);
    Array<CssPresentationSvgHit> hits;
    hits.resize(svgTreeHitTest(&own->tree, own->extent, nullptr, point, nullptr, 0));
    svgTreeHitTest(&own->tree, own->extent, nullptr, point, hits.data, hits.count);
    for (uint32_t i = 0; i < hits.count && i < capacity; ++i)
        out[i] = hits.data[i];
    uint32_t count = hits.count;
    hits.reset();
    return count;
}

bool contentForeignToClient(const void*, const PrDomElement* foreignObject, PrDVec2* points, uint32_t count) {
    return svgForeignToClient(foreignObject, points, count);
}

bool contentClientRect(const void*, const PrDomElement* element, PrDomRect* out) {
    PrRect rect{};
    bool found = svgClientBounds(element, &rect);
    *out = PrDomRect{double(rect.left), double(rect.top), double(prRectWidth(rect)), double(prRectHeight(rect))};
    return found;
}

// Returns natural dimensions in layout's units.
PrLayoutNaturalSize layoutNaturalOf(PrSvgNaturalSize natural) {
    PrLayoutNaturalSize size = prLayoutNaturalSizeNone();
    if (natural.width >= 0)
        size.width = prLayoutUnitFromPx(natural.width);
    if (natural.height >= 0)
        size.height = prLayoutUnitFromPx(natural.height);
    size.ratio = natural.ratio;
    return size;
}

bool contentVerify(void* content, PrString* report) {
    SvgContent* own = static_cast<SvgContent*>(content);
    // Changes noted since the last update are the next update's to bring in.
    if (contentPending(own))
        return true;
    PrSvgDrawOptions options{};
    SvgBuild build = buildOf(own, &options);
    SvgTree expected{};
    svgTreeBuild(&expected, own->svg, SvgRoot_Box, build);
    TextBuffer difference{};
    bool same = svgTreeSame(&own->tree, &expected, &difference);
    if (!same) {
        prStringReset(report);
        *report = textCopy(&difference);
    }
    textReset(&difference);
    svgTreeReset(&expected);
    return same;
}

constexpr CssPresentationSvgContentFunctions kContentFunctions = {
    .paint = contentPaint,
    .hitTest = contentHitTest,
    .clientRect = contentClientRect,
    .foreignToClient = contentForeignToClient,
    .verify = contentVerify,
    .release = contentRelease,
};

// ---------------------------------------------------------------------------------------------------------------------
// References

// Returns the viewport that the percentages of a resource's user space are of, for a box whose border box is `box`.
PrVec2 viewportOf(PrRect box) {
    return PrVec2{prRectWidth(box), prRectHeight(box)};
}

bool sourceDraw(void* PR_NULLABLE context, PrRect box, PrDrawList* list, PrLayoutSourceMask* out) {
    const SvgReferenceSource* source = static_cast<const SvgReferenceSource*>(context);
    const SvgTree* tree = &source->references->tree;
    const SvgResource& resource = tree->resources.data[source->resource];
    if (resource.kind == SvgResourceKind_ClipPath) {
        bool drawn = svgDrawClipCoverage(tree, source->resource, box, viewportOf(box), list);
        *out = PrLayoutSourceMask{
            .area = prDrawListBounds(list),
            .mode = PrDrawMaskMode_Alpha,
            .space = PrColorSpace_Srgb,
        };
        return drawn;
    }
    return svgDrawMaskContent(tree, source->resource, box, viewportOf(box), list, &out->area, &out->mode, &out->space);
}

bool sourceFilter(void* PR_NULLABLE context, PrRect box, PrRect bounds, PrDrawFilterInput input,
        PrDrawFilterBuilder* builder, PrLayoutSourceFilter* out) {
    const SvgReferenceSource* source = static_cast<const SvgReferenceSource*>(context);
    return svgAppendFilter(&source->references->tree, source->resource, bounds, viewportOf(box), input, builder,
        &out->result, &out->region);
}

bool sourceContains(void* PR_NULLABLE context, PrRect box, PrVec2 point) {
    const SvgReferenceSource* source = static_cast<const SvgReferenceSource*>(context);
    return svgClipContains(&source->references->tree, source->resource, box, viewportOf(box), point);
}

void sourceRetain(void* PR_NULLABLE context) {
    static_cast<SvgReferenceSource*>(context)->count.fetchAdd(1);
}

void sourceRelease(void* PR_NULLABLE context) {
    static_cast<SvgReferenceSource*>(context)->count.fetchSub(1);
}

constexpr PrLayoutSourceFunctions kClipSource = {
    .draw = sourceDraw,
    .contains = sourceContains,
    .retain = sourceRetain,
    .release = sourceRelease,
};
constexpr PrLayoutSourceFunctions kMaskSource = {.draw = sourceDraw, .retain = sourceRetain, .release = sourceRelease};
constexpr PrLayoutSourceFunctions kFilterSource = {
    .filter = sourceFilter,
    .retain = sourceRetain,
    .release = sourceRelease,
};

// Returns what a build of the references reads: the presentation's styles, its colors, and its fonts, which `options`
// holds.
SvgBuild referencesBuildOf(const SvgReferences* references, PrSvgDrawOptions* options) {
    PrCssPresentation* presentation = references->presentation;
    *options = prSvgDrawOptionsDefault();
    options->fonts = presentation->fonts;
    options->shaper = presentation->shaper;
    return SvgBuild{
        .source = {presentationStyle, presentationStyleUnder, presentationPseudoStyleUnder, presentation},
        .images = {loadedImage, nullptr},
        .foreign = {presentationForeignRoot, presentation},
        .environment = cssPresentationColorEnvironment(presentation),
        .options = options,
        .viewport = PrVec2{0, 0},
    };
}

// Frees the sources that no group names any longer, with their resources.
void sweepSources(SvgReferences* references) {
    for (uint32_t i = 0; i < references->sources.count; ++i) {
        SvgReferenceSource* source = references->sources.data[i];
        if (source == nullptr || source->count.load() != 0)
            continue;
        svgTreeFreeResource(&references->tree, source->resource);
        prMemoryFree(source);
        references->sources.data[i] = nullptr;
    }
}

void referencesReset(SvgReferences* references) {
    for (uint32_t i = 0; i < references->sources.count; ++i)
        prMemoryFree(references->sources.data[i]);
    references->sources.reset();
    svgTreeReset(&references->tree);
    svgChangesReset(&references->changes);
}

void follow(SvgPresentation* state);

// Resolves a URL that an effect of the box of `element` names (CssPresentationReferences).
bool referencesReference(void* PR_NULLABLE context, const PrDomElement* element, PrStringView url,
        CssPresentationReferenceKind kind, PrLayoutSource* out) {
    SvgPresentation* state = static_cast<SvgPresentation*>(context);
    SvgReferences* references = &state->references;
    *out = PrLayoutSource{};
    references->named = true;
    follow(state);
    PrSvgDrawOptions options{};
    SvgBuild build = referencesBuildOf(references, &options);
    bool pending = references->changes.all || references->changes.ids || references->changes.dirt.count != 0;
    if (pending) {
        sweepSources(references);
        if (svgTreeUpdateResources(&references->tree, references->presentation->document, &references->changes, build))
            ++references->version;
    }
    SvgResourceKind resourceKind = SvgResourceKind_Filter;
    if (kind == CssPresentationReferenceKind_ClipPath)
        resourceKind = SvgResourceKind_ClipPath;
    else if (kind == CssPresentationReferenceKind_Mask)
        resourceKind = SvgResourceKind_Mask;
    SvgResourceIndex index = svgTreeResourceFor(&references->tree, element, url, resourceKind, build);
    if (index == kSvgResourceNone)
        return false;
    while (references->sources.count <= index)
        references->sources.push(nullptr);
    SvgReferenceSource*& source = references->sources.data[index];
    if (source == nullptr) {
        source = allocZeroed<SvgReferenceSource>();
        source->references = references;
        source->resource = index;
    }
    source->count.fetchAdd(1);
    const PrLayoutSourceFunctions* functions = &kFilterSource;
    if (kind == CssPresentationReferenceKind_ClipPath)
        functions = &kClipSource;
    else if (kind == CssPresentationReferenceKind_Mask)
        functions = &kMaskSource;
    *out = PrLayoutSource{.functions = functions, .context = source, .version = references->version};
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Changes

// Returns what lib/svg keeps for the presentation of the document, or null if it keeps nothing.
SvgPresentation* PR_NULLABLE presentationState(const PrDomDocument* document) {
    PrCssPresentation* presentation = cssPresentationOf(document);
    return presentation != nullptr ? static_cast<SvgPresentation*>(cssPresentationSvgState(presentation)) : nullptr;
}

// Notes that the content of `svg` needs bringing up to date. A change that the update itself takes in needs no other
// update.
void stale(SvgPresentation* state, CssPresentationSvg* svg) {
    if (!state->updating)
        cssPresentationSvgStale(state->presentation, svg);
}

// Projects again the boxes that name a source of the references.
void reproject(SvgPresentation* state) {
    cssPresentationReprojectReferencing(state->presentation);
    state->reprojected = true;
}

// Notes a change at `element` in the references and in the content of every outermost svg element, any of which may
// depend on it. `name` is the attribute's local name for an attribute.
void note(SvgPresentation* state, const PrDomElement* element, SvgChange change, PrStringView name) {
    if (element->space != PrDomNamespace_Svg)
        return;
    SvgReferences& references = state->references;
    if (references.named && svgTreeNoteChange(&references.tree, element, change, name, false, &references.changes))
        reproject(state);
    uint32_t count = 0;
    CssPresentationSvg* const* svgs = cssPresentationSvgs(state->presentation, &count);
    if (count == 0)
        return;
    const PrDomElement* outer = nullptr;
    for (const PrDomElement* at = element; at != nullptr && at->space == PrDomNamespace_Svg && outer == nullptr;
            at = domElementFromNode(domFlatParent(at))) {
        if (domIsOuterSvg(at))
            outer = at;
    }
    for (uint32_t i = 0; i < count; ++i) {
        SvgContent* content = static_cast<SvgContent*>(svgs[i]->content);
        if (content != nullptr &&
                svgTreeNoteChange(&content->tree, element, change, name, outer == content->svg, &content->changes))
            stale(state, svgs[i]);
    }
}

// Notes a change to the children or the text of `parent`: of `parent` if it is an element, or of the host of a shadow
// root, whose children are its host's in the flat tree.
void noteChildren(SvgPresentation* state, PrDomNode* PR_NULLABLE parent, SvgChange change) {
    const PrDomShadowRoot* shadow = parent != nullptr ? domNodeShadowRoot(parent) : nullptr;
    if (const PrDomElement* element = shadow != nullptr ? shadow->host : domElementFromNode(parent))
        note(state, element, change, PrStringView{});
}

// Returns true if anything follows what IDs name: the references once a box has named one, or the content of an
// outermost svg element.
bool followsIds(const SvgPresentation* state) {
    uint32_t count = 0;
    cssPresentationSvgs(state->presentation, &count);
    return count != 0 || state->references.named;
}

// Returns true if `node` or an element under it has an ID or an xml:id, which an SVG reference may name.
bool holdsId(const PrDomNode* node) {
    for (const PrDomNode* at = node; at != nullptr; at = domNodeNext(node, at)) {
        const PrDomElement* element = domElementFromNode(at);
        if (element != nullptr && (domAttributeValue(element, "id").data != nullptr ||
                domAttribute(element, PrDomNamespace_Xml, "id") != nullptr))
            return true;
    }
    return false;
}

// Notes that what an ID names may have changed.
void idsChanged(SvgPresentation* state) {
    // A box that names nothing may name an element that an ID now names, so every box takes its sources again.
    if (state->references.named) {
        state->references.changes.ids = true;
        reproject(state);
    }
    uint32_t count = 0;
    CssPresentationSvg* const* svgs = cssPresentationSvgs(state->presentation, &count);
    for (uint32_t i = 0; i < count; ++i) {
        SvgContent* content = static_cast<SvgContent*>(svgs[i]->content);
        if (content != nullptr && svgTreeNoteIds(&content->tree, &content->changes))
            stale(state, svgs[i]);
    }
}

// Notes that everything that the references and the content read may have changed.
void invalidate(SvgPresentation* state) {
    if (state->references.named) {
        state->references.changes.all = true;
        reproject(state);
    }
    uint32_t count = 0;
    CssPresentationSvg* const* svgs = cssPresentationSvgs(state->presentation, &count);
    for (uint32_t i = 0; i < count; ++i) {
        if (SvgContent* content = static_cast<SvgContent*>(svgs[i]->content))
            content->changes.all = true;
        stale(state, svgs[i]);
    }
}

void followInserted(void* PR_NULLABLE context, PrDomNode* node) {
    SvgPresentation* state = static_cast<SvgPresentation*>(context);
    if (!domIsConnected(node))
        return;
    noteChildren(state, node->parent, SvgChange_Children);
    if (followsIds(state) && holdsId(node))
        idsChanged(state);
}

void followRemoving(void* PR_NULLABLE context, PrDomNode* node) {
    SvgPresentation* state = static_cast<SvgPresentation*>(context);
    if (!domIsConnected(node))
        return;
    if (const PrDomElement* element = domElementFromNode(node))
        note(state, element, SvgChange_Removing, PrStringView{});
    else
        noteChildren(state, node->parent, SvgChange_Text);
    if (followsIds(state) && holdsId(node))
        idsChanged(state);
}

void followAttribute(void* PR_NULLABLE context, PrDomElement* element, DomNamespaceId space, PrStringView name,
        PrStringView, PrStringView) {
    SvgPresentation* state = static_cast<SvgPresentation*>(context);
    // An ID that an element of another namespace takes may name what an SVG reference names.
    bool id = name == "id" && (space == PrDomNamespace_None || space == PrDomNamespace_Xml);
    if ((element->space != PrDomNamespace_Svg && !id) || !domIsConnected(element))
        return;
    note(state, element, SvgChange_Attribute, name);
    if (id && followsIds(state))
        idsChanged(state);
}

void followData(void* PR_NULLABLE context, PrDomNode* node) {
    if (domIsConnected(node))
        noteChildren(static_cast<SvgPresentation*>(context), node->parent, SvgChange_Text);
}

void followResourceLoaded(void* PR_NULLABLE context, PrDomDocument*) {
    SvgPresentation* state = static_cast<SvgPresentation*>(context);
    if (followsIds(state))
        idsChanged(state);
}

// The changes to the document that the state follows once it draws or resolves anything.
constexpr DomDocumentHooks kFollowHooks = {
    .inserted = followInserted,
    .removing = followRemoving,
    .attributeChanged = followAttribute,
    .dataChanged = followData,
    .resourceLoaded = followResourceLoaded,
};

// Subscribes the state to the document's changes, if it has not yet.
void follow(SvgPresentation* state) {
    if (!state->follows)
        state->follows = domDocumentSubscribe(state->document, &kFollowHooks, state);
}

// Draws `element` of a document that has no presentation, as `root` says, into a viewport of `viewport`.
void drawStatic(const PrDomElement* element, SvgRoot root, PrVec2 viewport,
        const PrSvgDrawOptions* PR_NULLABLE options, PrDrawList* list) {
    PrSvgDrawOptions own = options != nullptr ? *options : prSvgDrawOptionsDefault();
    SvgCascade cascade{};
    svgCascadeInit(&cascade, const_cast<PrDomDocument*>(element->document), own, viewport);
    CssColorEnvironment environment = kCssDefaultColorEnvironment;
    environment.headroom = own.headroom;
    SvgBuild build{
        .source = {cascadeStyle, cascadeStyleUnder, cascadePseudoStyleUnder, &cascade},
        .images = hostImages(&own),
        .environment = environment,
        .options = &own,
        .viewport = viewport,
    };
    SvgTree tree{};
    svgTreeBuild(&tree, element, root, build);
    svgTreeDraw(&tree, viewport, nullptr, list);
    svgTreeReset(&tree);
    svgCascadeReset(&cascade);
}

// The size that an image with no natural dimensions is drawn at, where something must be decided before the image is
// shown: what the viewport units of its styles and the percentages of its text are of
// (css-images-3/#default-object-size).
constexpr PrVec2 kDefaultObjectSize{300, 150};

} // namespace

SvgPresentation* svgPresentationCreate(PrCssPresentation* presentation) {
    SvgPresentation* state = allocZeroed<SvgPresentation>();
    state->presentation = presentation;
    state->references.presentation = presentation;
    state->document = presentation->document;
    state->resolver = CssPresentationReferences{.reference = referencesReference, .context = state};
    cssPresentationSetSvgState(presentation, state);
    cssPresentationSetReferences(presentation, &state->resolver);
    return state;
}

void svgPresentationDestroy(SvgPresentation* PR_NULLABLE state) {
    if (state == nullptr)
        return;
    if (state->follows)
        domDocumentUnsubscribe(state->document, &kFollowHooks);
    referencesReset(&state->references);
    prMemoryFree(state);
}

bool svgPresentationUpdate(PrCssPresentation* presentation) {
    if (cssPresentationSvgIdle(presentation))
        return false;
    SvgPresentation* state = static_cast<SvgPresentation*>(presentation->svgState);
    state->reprojected = false;
    state->updating = true;
    uint32_t noteCount = 0;
    const CssPresentationSvgNote* notes = cssPresentationSvgNotes(presentation, &noteCount);
    for (uint32_t i = 0; i < noteCount; ++i) {
        SvgChange change = notes[i].kind == CssPresentationSvgNoteKind_Style ? SvgChange_Style : SvgChange_Box;
        note(state, notes[i].element, change, PrStringView{});
    }
    cssPresentationClearSvgNotes(presentation);
    if (cssPresentationTakeSvgInvalid(presentation))
        invalidate(state);
    uint32_t count = 0;
    CssPresentationSvg* const* svgs = cssPresentationSvgs(presentation, &count);
    Array<CssPresentationSvgForeignObject> foreign;
    for (uint32_t i = 0; i < count; ++i) {
        CssPresentationSvg* svg = svgs[i];
        if (svg->content == nullptr) {
            svg->content = contentCreate(presentation, svg->element);
            svg->functions = &kContentFunctions;
            follow(state);
        }
        // The natural size comes first, since the box's extent falls back on it before the box is laid out.
        const CssComputedStyle* style = cssPresentationStyle(presentation, svg->element);
        cssPresentationSvgSetNatural(presentation, svg, layoutNaturalOf(naturalSizeOf(svg->element, style)));
        SvgContent* content = static_cast<SvgContent*>(svg->content);
        if (!svg->stale && !contentPending(content))
            continue;
        foreign.clear();
        CssPresentationSvgUpdate update = contentUpdate(content, cssPresentationSvgExtent(presentation, svg), &foreign);
        cssPresentationSvgUpdated(presentation, svg, update);
    }
    foreign.reset();
    state->updating = false;
    return state->reprojected;
}

void svgContentImageChanged(PrDomElement* element) {
    SvgPresentation* state = domIsConnected(element) ? presentationState(element->document) : nullptr;
    if (state != nullptr)
        note(state, element, SvgChange_Image, PrStringView{});
}

bool svgQueryBegin(SvgQuery* query, const PrDomElement* element) {
    *query = SvgQuery{.node = kSvgNodeNone};
    PrCssPresentation* presentation = domIsConnected(element) ? cssPresentationOf(element->document) : nullptr;
    if (presentation != nullptr) {
        cssPresentationFlush(presentation);
        // The content of the nearest outermost svg element around the element that holds a node of it.
        for (const PrDomNode* n = element; n != nullptr; n = n->parent) {
            const PrDomElement* e = domElementFromNode(n);
            const CssPresentationSvg* svg = e != nullptr ? cssPresentationSvgOf(presentation, e) : nullptr;
            SvgContent* content = svg != nullptr ? static_cast<SvgContent*>(svg->content) : nullptr;
            SvgNodeIndex node = content != nullptr ? svgTreeNodeFromElement(&content->tree, element) : kSvgNodeNone;
            if (node != kSvgNodeNone) {
                query->tree = &content->tree;
                query->node = node;
                query->outermost = e;
                query->viewport = content->extent;
                query->presentation = presentation;
                return true;
            }
        }
        return false;
    }
    // A document that has no presentation draws its outermost svg element at its natural size.
    const PrDomElement* outermost = nullptr;
    for (const PrDomElement* e = element; e != nullptr && e->space == PrDomNamespace_Svg;
            e = domElementFromNode(e->parent)) {
        if (e->tag == PrDomTag_Svg)
            outermost = e;
    }
    if (outermost == nullptr)
        return false;
    PrSvgNaturalSize natural = prSvgElementNaturalSize(static_cast<const PrSvgElement*>(outermost));
    PrVec2 viewport{
        natural.width > 0 ? natural.width : kDefaultObjectSize.x,
        natural.height > 0 ? natural.height : kDefaultObjectSize.y,
    };
    PrSvgDrawOptions options = prSvgDrawOptionsDefault();
    svgCascadeInit(&query->cascade, const_cast<PrDomDocument*>(element->document), options, viewport);
    SvgBuild build{
        .source = {cascadeStyle, cascadeStyleUnder, cascadePseudoStyleUnder, &query->cascade},
        .images = hostImages(&options),
        .environment = kCssDefaultColorEnvironment,
        .options = &options,
        .viewport = viewport,
    };
    svgTreeBuild(&query->own, outermost, SvgRoot_Outermost, build);
    query->owned = true;
    query->outermost = outermost;
    query->viewport = viewport;
    query->node = svgTreeNodeFromElement(&query->own, element);
    query->tree = query->node != kSvgNodeNone ? &query->own : nullptr;
    return query->tree != nullptr;
}

void svgQueryEnd(SvgQuery* query) {
    if (query->owned) {
        svgTreeReset(&query->own);
        svgCascadeReset(&query->cascade);
    }
    *query = SvgQuery{.node = kSvgNodeNone};
}

bool svgQueryPlace(const SvgQuery& query, SvgPlace* out) {
    return query.tree != nullptr && svgNodePlace(query.tree, query.viewport, query.node, out);
}

bool svgQueryToClient(const SvgQuery& query, PrDVec2* points, uint32_t count) {
    if (query.presentation == nullptr || query.outermost == nullptr)
        return false;
    return cssPresentationMapFromContent(query.presentation, query.outermost, points, count);
}

bool svgElementLengthBasis(const PrDomElement* element, SvgLengthBasis* out) {
    *out = SvgLengthBasis{.fontSize = float(kCssMediumFontSize)};
    SvgQuery query{};
    SvgPlace place{};
    bool found = svgQueryBegin(&query, element) && svgQueryPlace(query, &place);
    if (found)
        out->viewport = place.viewport;
    PrCssPresentation* presentation = cssPresentationOf(element->document);
    const CssComputedStyle* computed = nullptr;
    if (query.owned)
        computed = svgCascadeStyle(&query.cascade, element);
    else if (presentation != nullptr)
        computed = cssPresentationStyle(presentation, element);
    if (computed != nullptr)
        out->fontSize = float(cssStyleFontSize(computed));
    svgQueryEnd(&query);
    return found;
}

bool svgForeignToClient(const PrDomElement* foreignObject, PrDVec2* points, uint32_t count) {
    SvgQuery query{};
    SvgPlace place{};
    bool found = svgQueryBegin(&query, foreignObject) && svgQueryPlace(query, &place) &&
        query.tree->nodes.data[query.node].kind == SvgNodeKind_ForeignObject;
    if (found) {
        // The content is drawn from the top left of the positioning rectangle, in the foreignObject's user space.
        PrRect box = svgNodeBox(query.tree, place, query.node, PrSvgBoundingBoxOptions{.fill = true});
        for (uint32_t i = 0; i < count; ++i) {
            PrVec2 mapped = prAffine2MapPoint(place.toTree, PrVec2{
                box.left + float(points[i].x),
                box.top + float(points[i].y),
            });
            points[i] = PrDVec2{double(mapped.x), double(mapped.y)};
        }
        found = svgQueryToClient(query, points, count);
    }
    svgQueryEnd(&query);
    return found;
}

bool svgClientBounds(const PrDomElement* element, PrRect* out) {
    // A tspan or a textPath is drawn by the text that holds it, in the text's user space.
    const PrDomElement* drawn = element;
    bool inText = element->space == PrDomNamespace_Svg &&
        (element->tag == PrDomTag_Tspan || element->tag == PrDomTag_TextPath);
    while (inText && drawn != nullptr && !svgIs(drawn, PrDomTag_Text))
        drawn = domElementFromNode(drawn->parent);
    SvgQuery query{};
    SvgPlace place{};
    bool found = drawn != nullptr && svgQueryBegin(&query, drawn) && svgQueryPlace(query, &place);
    if (found) {
        // The bounding box in the element's user space, mapped through every transform to the viewport, as drawn.
        PrRect box = svgNodeBox(query.tree, place, query.node, PrSvgBoundingBoxOptions{.fill = true});
        if (drawn != element) {
            PrDomRect own = prSvgGraphicsElementGetBBox(static_cast<PrSvgElement*>(const_cast<PrDomElement*>(element)), nullptr);
            box = PrRect{float(own.x), float(own.y), float(own.x + own.width), float(own.y + own.height)};
        }
        PrVec2 points[4] = {{box.left, box.top}, {box.right, box.top}, {box.right, box.bottom}, {box.left, box.bottom}};
        PrDVec2 corners[4];
        for (uint32_t i = 0; i < 4; ++i) {
            PrVec2 mapped = prAffine2MapPoint(place.toTree, points[i]);
            corners[i] = PrDVec2{double(mapped.x), double(mapped.y)};
        }
        found = svgQueryToClient(query, corners, 4);
        if (found) {
            PrRect bounds{float(corners[0].x), float(corners[0].y), float(corners[0].x), float(corners[0].y)};
            for (const PrDVec2& corner : corners) {
                bounds = PrRect{
                    minF32(bounds.left, float(corner.x)), minF32(bounds.top, float(corner.y)),
                    maxF32(bounds.right, float(corner.x)), maxF32(bounds.bottom, float(corner.y)),
                };
            }
            *out = bounds;
        }
    }
    svgQueryEnd(&query);
    return found;
}

} // namespace pr_impl

using namespace pr_impl;

PrSvgDrawOptions prSvgDrawOptionsDefault(void) {
    return PrSvgDrawOptions{};
}

void prSvgElementDraw(PrSvgElement* svg, PrVec2 viewport, const PrSvgDrawOptions* PR_NULLABLE options, PrDrawList* list) {
    PR_ASSERT(viewport.x >= 0 && viewport.y >= 0);
    drawStatic(svg, SvgRoot_Outermost, viewport, options, list);
}

void prSvgElementDrawBox(PrSvgElement* svg, PrVec2 extent, const PrSvgDrawOptions* PR_NULLABLE options, PrDrawList* list) {
    PR_ASSERT(extent.x >= 0 && extent.y >= 0);
    drawStatic(svg, SvgRoot_Box, extent, options, list);
}

PrSvgNaturalSize prSvgElementNaturalSize(const PrSvgElement* svg) {
    SvgCascade cascade{};
    PrSvgDrawOptions options = prSvgDrawOptionsDefault();
    svgCascadeInit(&cascade, const_cast<PrDomDocument*>(svg->document), options, PrVec2{0, 0});
    PrSvgNaturalSize size = naturalSizeOf(svg, svgCascadeStyle(&cascade, svg));
    svgCascadeReset(&cascade);
    return size;
}

const PrSvgImage* prSvgImageCreate(PrSvgElement* svg, PrStringView fragment, const PrSvgDrawOptions* PR_NULLABLE options) {
    PrSvgDrawOptions own = options != nullptr ? *options : prSvgDrawOptionsDefault();
    PrSvgImage* picture = allocZeroed<PrSvgImage>();
    picture->references.storeRelaxed(1);
    // The root's own view, which the fragment may replace in part. The root's viewport clips, whatever its overflow, as
    // nothing outside the image is drawn.
    SvgRootView& view = picture->view;
    view.hasViewBox = svgParseViewBox(svgAttribute(svg, "viewBox"), &view.viewBox);
    svgParseAspect(svgAttribute(svg, "preserveAspectRatio"), &view.aspect);
    view.clips = true;
    // The element that the fragment names is the document's target while the image is built, which :target matches.
    PrDomDocument* document = const_cast<PrDomDocument*>(svg->document);
    const PrDomElement* target = svgImageView(fragment, svg, &view);
    PrDomElement* savedTarget = document->target;
    document->target = const_cast<PrDomElement*>(target);
    // An active view gives the ratio of an image that its width and height do not give one.
    PrSvgNaturalSize natural = prSvgElementNaturalSize(svg);
    bool sized = natural.width > 0 && natural.height > 0;
    if (!sized && view.hasViewBox && view.viewBox.right > view.viewBox.left && view.viewBox.bottom > view.viewBox.top)
        natural.ratio = prRectWidth(view.viewBox) / prRectHeight(view.viewBox);
    picture->natural = natural;
    PrVec2 viewport{
        natural.width > 0 ? natural.width : kDefaultObjectSize.x,
        natural.height > 0 ? natural.height : kDefaultObjectSize.y,
    };
    SvgCascade cascade{};
    svgCascadeInit(&cascade, document, own, viewport);
    CssColorEnvironment environment = kCssDefaultColorEnvironment;
    environment.headroom = own.headroom;
    SvgBuild build{
        .source = {cascadeStyle, cascadeStyleUnder, cascadePseudoStyleUnder, &cascade},
        .images = hostImages(&own),
        .environment = environment,
        .options = &own,
        .viewport = viewport,
    };
    svgTreeBuild(&picture->tree, svg, SvgRoot_Image, build);
    const CssComputedStyle* style = svgCascadeStyle(&cascade, svg);
    if (style != nullptr) {
        // The root's background propagates to the canvas, which is the whole image
        // (css-backgrounds-3/#root-background).
        picture->background = svgStyleColor(style, PrCssProperty_BackgroundColor, environment);
        PrCssKeyword overflow = svgStyleKeyword(style, PrCssProperty_OverflowX);
        picture->overflows = overflow == PrCssKeyword_Visible || overflow == PrCssKeyword_Auto;
    }
    if (!view.hasViewBox && natural.width > 0 && natural.height > 0)
        picture->scaledFrom = PrVec2{natural.width, natural.height};
    document->target = savedTarget;
    svgTreeDetach(&picture->tree);
    svgCascadeReset(&cascade);
    return picture;
}

void prSvgImageRetain(const PrSvgImage* image) {
    svgImageRetain(image);
}

void prSvgImageRelease(const PrSvgImage* PR_NULLABLE image) {
    svgImageRelease(image);
}

PrSvgNaturalSize prSvgImageNaturalSize(const PrSvgImage* image) {
    return image->natural;
}

PrLayoutPainter prSvgImagePainter(const PrSvgImage* image) {
    return svgImagePainter(image);
}

void prSvgImageDraw(const PrSvgImage* image, PrVec2 extent, PrDrawList* list) {
    svgImageDraw(image, extent, list);
}

void prSvgElementDrawInPlace(PrSvgElement* element, PrVec2 viewport, const PrSvgDrawOptions* PR_NULLABLE options,
        PrDrawList* list) {
    PR_ASSERT(viewport.x >= 0 && viewport.y >= 0);
    drawStatic(element, SvgRoot_InPlace, viewport, options, list);
}
