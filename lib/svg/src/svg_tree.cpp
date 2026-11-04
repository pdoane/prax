// The SVG tree: building it from the document and the computed styles of its elements, and bringing it up to date as
// they change.

#include "svg/src/svg_tree.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/number_list.h"
#include "pr/path_data.h"
#include "pr/sort.h"
#include "pr/string_ops.h"

#include "css/src/css_animation.h"
#include "css/src/css_gradient.h"
#include "css/src/css_shape.h"
#include "css/src/css_unit.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_resource.h"
#include "svg/src/svg_cascade.h"
#include "svg/src/svg_filter.h"
#include "svg/src/svg_image.h"
#include "svg/src/svg_text.h"

namespace pr_impl {

namespace {

// The number of links that a chain of references follows before it is treated as a cycle.
constexpr uint32_t kMaxReferences = 16;

// Returns true if the element's conditional processing attributes are satisfied
// (SVG2/struct.html#ConditionalProcessing). SVG2 dropped requiredFeatures, which is ignored. The user's language is
// English, so systemLanguage holds if it lists a tag of the language en.
bool conditionsHold(const PrDomElement* e) {
    // The extensions are the namespaces of HTML and MathML, which a foreignObject renders (w3c/svgwg#138). An empty
    // list evaluates to false (SVG2/struct.html#ConditionalProcessingRequiredExtensionsAttribute).
    PrStringView extensions = svgAttribute(e, "requiredExtensions");
    if (extensions.data != nullptr) {
        if (extensions.size == 0)
            return false;
        for (size_t start = 0; start < extensions.size;) {
            while (start < extensions.size && prAsciiIsSpace(extensions.data[start]))
                ++start;
            size_t end = start;
            while (end < extensions.size && !prAsciiIsSpace(extensions.data[end]))
                ++end;
            PrStringView uri{extensions.data + start, end - start};
            if (uri.size != 0 && uri != "http://www.w3.org/1999/xhtml" && uri != "http://www.w3.org/1998/Math/MathML")
                return false;
            start = end;
        }
    }
    PrStringView languages = domAttributeValue(e, "systemLanguage");
    if (languages.data == nullptr)
        return true;
    size_t start = 0;
    for (size_t i = 0; i <= languages.size; ++i) {
        if (i < languages.size && languages.data[i] != ',')
            continue;
        PrStringView tag{languages.data + start, i - start};
        while (tag.size != 0 && tag.data[0] == ' ') {
            ++tag.data;
            --tag.size;
        }
        if (tag.size >= 2 && (tag.data[0] | 0x20) == 'e' && (tag.data[1] | 0x20) == 'n' && (tag.size == 2 || tag.data[2] == '-'))
            return true;
        start = i + 1;
    }
    return false;
}

// Returns true if a child is built where it is: an element in the SVG namespace whose conditions hold.
bool builds(const PrDomElement* e) {
    return e->space == PrDomNamespace_Svg && conditionsHold(e);
}

// Returns true if an element never renders where it is (SVG2/render.html#Rendering): a resource that others reference,
// a container of such resources, or an element that holds no graphics. A symbol renders only as a use element's
// instance.
bool neverRenders(const PrDomElement* e) {
    switch (e->tag) {
        case PrDomTag_Defs:
        case PrDomTag_ClipPath:
        case PrDomTag_Mask:
        case PrDomTag_Marker:
        case PrDomTag_Pattern:
        case PrDomTag_LinearGradient:
        case PrDomTag_RadialGradient:
        case PrDomTag_Symbol:
        case PrDomTag_Filter:
        case PrDomTag_Style:
        case PrDomTag_Script:
        case PrDomTag_Title:
        case PrDomTag_Desc:
        case PrDomTag_Metadata:
        case PrDomTag_Font:
        case PrDomTag_FontFace:
            return true;
        default:
            return false;
    }
}

// Returns true if any function of the list moves by a percentage of the reference box.
bool readsBox(const PrLayoutTransformList& list) {
    for (uint32_t i = 0; i < list.count; ++i) {
        const PrLayoutTransformFunction& f = list.functions[i];
        if (f.kind == PrLayoutTransformKind_Translate && (f.values[3] != 0 || f.values[4] != 0))
            return true;
    }
    return false;
}

// Returns true if an attribute of an ancestor decides how the text inside it is laid out: SVG Tiny 1.2's alignments of
// text areas, which text inherits from its ancestors' attributes rather than their styles.
bool laysOutText(PrStringView name) {
    return name == "text-align" || name == "display-align" || name == "line-increment";
}

// ---------------------------------------------------------------------------------------------------------------------
// Storage

// Adds `item` to `items` in a free slot if there is one, and returns its index.
template <typename T>
uint32_t take(RawArray<T>& items, RawArray<uint32_t>& free, const T& item) {
    if (free.count == 0) {
        items.push(item);
        return items.count - 1;
    }
    uint32_t index = free.last();
    free.pop();
    items.data[index] = item;
    return index;
}

uint32_t hashOf(const PrDomElement* element) {
    return prHashPointer(element, 0);
}

// Returns the bit of SvgTree::watched for `element`.
uint64_t watchBit(const PrDomElement* element) {
    return uint64_t(1) << (hashOf(element) & 63);
}

bool isLive(const SvgTree* tree, SvgNodeIndex index) {
    return index < tree->nodes.count && tree->nodes.data[index].kind != SvgNodeKind_Free;
}

// Frees the references from `first` on, a dependent's list.
void freeReferences(SvgTree* tree, uint32_t first) {
    for (uint32_t at = first; at != kArrayMissing;) {
        SvgReference& reference = tree->references.data[at];
        uint32_t next = reference.next;
        prRawStringReset(&reference.id);
        prRawStringReset(&reference.document);
        domNodeRelease(const_cast<PrDomElement*>(reference.target));
        domNodeRelease(const_cast<PrDomNode*>(reference.scope));
        reference = SvgReference{.node = kSvgNodeNone, .resource = kSvgResourceNone, .next = kArrayMissing};
        tree->freeReferences.push(at);
        at = next;
    }
}

// Records a reference to `target`, which `id` named in the document of `document`'s URL, or in the tree's own document
// for the null view, for what the build is building. A reference without an ID or a document is to the element that a
// resource is built from, whose subtree the resource reads.
void record(SvgBuilder& b, PrStringView id, const PrDomElement* PR_NULLABLE target, const PrDomNode* PR_NULLABLE scope,
        bool content, PrStringView document = PrStringView{}) {
    SvgTree* tree = b.tree;
    if (b.ownerResource == kSvgResourceNone && b.owner == kSvgNodeNone)
        return;
    SvgReference reference{
        .id = id.data != nullptr ? prRawStringCopy(id) : PrRawString{},
        .document = document.data != nullptr ? prRawStringCopy(document) : PrRawString{},
        .target = target,
        .scope = scope,
        .node = b.ownerResource == kSvgResourceNone ? b.owner : kSvgNodeNone,
        .resource = b.ownerResource,
        .content = content,
    };
    if (target != nullptr)
        domNodeRetain(const_cast<PrDomElement*>(target));
    if (scope != nullptr)
        domNodeRetain(const_cast<PrDomNode*>(scope));
    if (target != nullptr && content)
        tree->watched |= watchBit(target);
    if (b.ownerResource != kSvgResourceNone)
        reference.next = tree->resources.data[b.ownerResource].firstReference;
    else
        reference.next = tree->nodes.data[b.owner].firstReference;
    uint32_t index = take(tree->references, tree->freeReferences, reference);
    if (b.ownerResource != kSvgResourceNone)
        tree->resources.data[b.ownerResource].firstReference = index;
    else
        tree->nodes.data[b.owner].firstReference = index;
}

// Returns the element of `id` in the shadow tree `scope`, or else in `document`. A reference in a shadow tree names an
// element of that tree (css-shadow-1/#css-tree-scoped-reference), and the instances of a use element's shadow tree
// reference the elements of the document.
const PrDomElement* PR_NULLABLE findInScope(const PrDomNode* PR_NULLABLE scope, const PrDomNode* document, PrStringView id) {
    const PrDomElement* found = scope != nullptr ? domElementById(const_cast<PrDomNode*>(scope), id) : nullptr;
    return found != nullptr ? found : svgFindById(document, id);
}

// Returns true if a reference names another element than the one that it named when it was recorded, in `document`, the
// tree's own document. A reference without an ID or a document names no element by them.
bool retargeted(const SvgReference& reference, const PrDomNode* document) {
    if (!prRawStringIsNull(&reference.document)) {
        DomReference other{.external = true, .url = reference.document, .fragment = prRawStringView(&reference.id)};
        return domReferenceTarget(const_cast<PrDomDocument*>(document->document), other) != reference.target;
    }
    if (prRawStringView(&reference.id).size == 0)
        return false;
    return findInScope(reference.scope, document, prRawStringView(&reference.id)) != reference.target;
}

// Returns the cascade that styles the elements of `document`, another document than the build's, made the first time
// that the build styles one. The document is a resource document, which loads nothing of its own
// (SVG2/linking.html#processingURL-parsing).
SvgCascade* cascadeOf(SvgBuilder& b, PrDomDocument* document) {
    for (uint32_t i = 0; i < b.tree->cascades.count; ++i) {
        if (b.tree->cascades.data[i]->document == document)
            return b.tree->cascades.data[i];
    }
    PrSvgDrawOptions options = *b.build->options;
    options.load = nullptr;
    options.loadImage = nullptr;
    SvgCascade* cascade = allocZeroed<SvgCascade>();
    svgCascadeInit(cascade, document, options, b.build->viewport);
    b.tree->cascades.push(cascade);
    return cascade;
}

// Unlinks a node from its parent's children.
void unlink(SvgTree* tree, SvgNodeIndex index) {
    SvgNode& node = tree->nodes.data[index];
    if (node.parent != kSvgNodeNone) {
        SvgNode& parent = tree->nodes.data[node.parent];
        if (node.prev != kSvgNodeNone)
            tree->nodes.data[node.prev].next = node.next;
        else
            parent.first = node.next;
        if (node.next != kSvgNodeNone)
            tree->nodes.data[node.next].prev = node.prev;
        else
            parent.last = node.prev;
    }
    node.prev = node.next = kSvgNodeNone;
}

// Links an unlinked node into its parent's children after `after`, or first if `after` is kSvgNodeNone.
void linkAfter(SvgTree* tree, SvgNodeIndex index, SvgNodeIndex after) {
    SvgNode& node = tree->nodes.data[index];
    SvgNode& parent = tree->nodes.data[node.parent];
    node.prev = after;
    node.next = after != kSvgNodeNone ? tree->nodes.data[after].next : parent.first;
    if (node.next != kSvgNodeNone)
        tree->nodes.data[node.next].prev = index;
    else
        parent.last = index;
    if (after != kSvgNodeNone)
        tree->nodes.data[after].next = index;
    else
        parent.first = index;
}

// Moves a node that was just built, the last of its parent's children, to after `after`.
void placeAfter(SvgTree* tree, SvgNodeIndex index, SvgNodeIndex after) {
    if (tree->nodes.data[index].prev == after)
        return;
    unlink(tree, index);
    linkAfter(tree, index, after);
}

// Releases the references that a picture holds.
void releasePicture(const SvgPicture& picture) {
    prDrawImageRelease(picture.raster);
    svgImageRelease(picture.svg);
}

// Frees what effects hold.
void resetEffects(SvgEffects& effects) {
    cssStyleValuesRelease(effects.values);
    for (uint32_t i = 0; i < effects.layers.count; ++i) {
        releasePicture(effects.layers.data[i].picture);
        prMemoryFree(effects.layers.data[i].stops);
    }
    effects.layers.reset();
    effects.filters.reset();
    effects = SvgEffects{};
}

// Frees what effects hold and puts their slot on the free list.
void dropEffects(SvgTree* tree, uint32_t index) {
    if (index == kArrayMissing)
        return;
    resetEffects(tree->effects.data[index]);
    tree->freeEffects.push(index);
}

// Frees what one node holds and puts its slot on the free list. Its links and its children are left to the caller.
void freeNode(SvgTree* tree, SvgNodeIndex index) {
    SvgNode& node = tree->nodes.data[index];
    if (node.own)
        tree->elements.erase(hashOf(node.element), index);
    freeReferences(tree, node.firstReference);
    svgTreeFreeStroke(tree, node.paints);
    dropEffects(tree, node.effects);
    if (node.transformStyle != kArrayMissing)
        tree->freeTransforms.push(node.transformStyle);
    if (node.markers != kArrayMissing) {
        tree->markers.data[node.markers].vertices.reset();
        tree->freeMarkers.push(node.markers);
    }
    switch (node.kind) {
        case SvgNodeKind_Path: prPathDestroy(node.path.path); break;
        case SvgNodeKind_Text: svgTextDestroy(tree, node.text); break;
        case SvgNodeKind_Image: releasePicture(node.image.picture); break;
        default: break;
    }
    tree->nodes.data[index] = SvgNode{.kind = SvgNodeKind_Free};
    tree->freeNodes.push(index);
}

// Unlinks a node from its parent and frees it and its descendants.
void freeSubtree(SvgTree* tree, SvgNodeIndex index) {
    unlink(tree, index);
    RawArray<SvgNodeIndex> stack;
    stack.push(index);
    while (stack.count != 0) {
        SvgNodeIndex at = stack.last();
        stack.pop();
        for (SvgNodeIndex child = tree->nodes.data[at].first; child != kSvgNodeNone;
                child = tree->nodes.data[child].next)
            stack.push(child);
        freeNode(tree, at);
    }
    stack.reset();
}

// Returns the group of the content of a marker, a pattern, a clipPath, a mask or a filter, which no node holds, or
// kSvgNodeNone.
SvgNodeIndex contentOf(const SvgResource& resource) {
    switch (resource.kind) {
        case SvgResourceKind_Marker: return resource.marker.content;
        case SvgResourceKind_Pattern: return resource.pattern.content;
        case SvgResourceKind_ClipPath: return resource.clipPath.content;
        case SvgResourceKind_Mask: return resource.mask.content;
        case SvgResourceKind_Filter: return resource.filter.content;
        default: return kSvgNodeNone;
    }
}

// Sets the data of a resource of `kind` to what it holds before it is built: nothing, and no content.
void clearResource(SvgResource& resource, SvgResourceKind kind) {
    resource.kind = kind;
    switch (kind) {
        case SvgResourceKind_Gradient: resource.gradient = SvgGradient{}; break;
        case SvgResourceKind_Marker: resource.marker = SvgMarker{.content = kSvgNodeNone}; break;
        case SvgResourceKind_Pattern: resource.pattern = SvgPattern{.content = kSvgNodeNone}; break;
        case SvgResourceKind_ClipPath:
            resource.clipPath = SvgClipPath{.transform = prAffine2Identity(), .content = kSvgNodeNone};
            break;
        case SvgResourceKind_Mask: resource.mask = SvgMask{.content = kSvgNodeNone}; break;
        case SvgResourceKind_Filter: resource.filter = SvgFilter{.content = kSvgNodeNone}; break;
        default: break;
    }
}

// Frees what the data of a resource's kind holds outside the tree's arrays: a gradient's stops, and a filter's
// primitives.
void freeResourceData(SvgResource& resource) {
    if (resource.kind == SvgResourceKind_Gradient)
        prMemoryFree(resource.gradient.stops);
    else if (resource.kind == SvgResourceKind_Filter)
        svgFilterFree(resource.filter);
}

// Frees what a resource holds, its content's nodes among them, and leaves its slot empty of data.
void resetResource(SvgTree* tree, SvgResourceIndex index) {
    SvgResource& resource = tree->resources.data[index];
    freeReferences(tree, resource.firstReference);
    resource.firstReference = kArrayMissing;
    freeResourceData(resource);
    SvgNodeIndex content = contentOf(resource);
    clearResource(resource, resource.kind);
    if (content != kSvgNodeNone)
        freeSubtree(tree, content);
}

// Frees a resource and puts its slot on the free list.
void freeResource(SvgTree* tree, SvgResourceIndex index) {
    resetResource(tree, index);
    SvgResource& resource = tree->resources.data[index];
    domNodeRelease(const_cast<PrDomElement*>(resource.element));
    resource.element = nullptr;
    tree->freeResources.push(index);
}

// Frees the resources that no node references, once there are twice as many as the last sweep kept. A resource with
// content is used if a node that it does not hold references it, so resources whose content references each other are
// freed together.
void sweepResources(SvgTree* tree) {
    uint32_t live = tree->resources.count - tree->freeResources.count;
    if (live <= 2 * tree->sweptResources + 8)
        return;
    RawArray<bool> used;
    used.resize(tree->resources.count);
    prMemZero(used.data, used.count * sizeof(bool));
    RawArray<SvgResourceIndex> holders;    // the resources found used, whose content is yet to be marked
    auto use = [&](SvgResourceIndex index) {
        if (index == kSvgResourceNone || used.data[index])
            return;
        used.data[index] = true;
        if (contentOf(tree->resources.data[index]) != kSvgNodeNone)
            holders.push(index);
    };
    auto mark = [&](const SvgNode& node) {
        use(node.paints.fill.server);
        use(node.paints.stroke.server);
        if (node.effects != kArrayMissing) {
            const SvgEffects& effects = tree->effects.data[node.effects];
            if (effects.clip == SvgClipKind_Url)
                use(effects.clipPath);
            for (uint32_t i = 0; i < effects.layers.count; ++i) {
                if (effects.layers.data[i].kind == SvgMaskLayerKind_Mask)
                    use(effects.layers.data[i].mask);
            }
            for (uint32_t i = 0; i < effects.filters.count; ++i)
                use(effects.filters.data[i].filter);
        }
        if (node.markers != kArrayMissing) {
            const SvgMarkers& m = tree->markers.data[node.markers];
            use(m.start);
            use(m.mid);
            use(m.end);
        }
        if (node.kind == SvgNodeKind_Text) {
            const SvgPaints* paints = svgTextPaints(node.text);
            for (uint32_t p = 0; p < svgTextPaintsCount(node.text); ++p) {
                use(paints[p].fill.server);
                use(paints[p].stroke.server);
            }
        }
    };
    RawArray<SvgNodeIndex> stack;
    auto markSubtree = [&](SvgNodeIndex root) {
        stack.push(root);
        while (stack.count != 0) {
            SvgNodeIndex at = stack.last();
            stack.pop();
            mark(tree->nodes.data[at]);
            for (SvgNodeIndex child = tree->nodes.data[at].first; child != kSvgNodeNone;
                    child = tree->nodes.data[child].next)
                stack.push(child);
        }
    };
    if (isLive(tree, 0))
        markSubtree(0);
    while (holders.count != 0) {
        SvgResourceIndex index = holders.last();
        holders.pop();
        markSubtree(contentOf(tree->resources.data[index]));
    }
    stack.reset();
    holders.reset();
    for (uint32_t i = 0; i < tree->resources.count; ++i) {
        if (tree->resources.data[i].element != nullptr && !used.data[i])
            freeResource(tree, i);
    }
    used.reset();
    tree->sweptResources = tree->resources.count - tree->freeResources.count;
}

// ---------------------------------------------------------------------------------------------------------------------
// Building

SvgResourceIndex resourceOf(SvgBuilder& b, const PrDomElement* element, SvgResourceKind kind);
SvgNodeIndex buildElement(SvgBuilder& b, const PrDomElement* e, const CssComputedStyle* PR_NULLABLE parentStyle,
    SvgNodeIndex parent, bool instance);

// The values of an element's style that a node takes, beyond those that every node takes. An element's own node takes
// all of them, and an element that builds more than one node, or that applies some elsewhere, divides them.
enum {
    StyleParts_None = 0,
    StyleParts_Transform = 1 << 0,      // the transform property
    StyleParts_Opacity = 1 << 1,
    StyleParts_ClipPath = 1 << 2,
    StyleParts_Mask = 1 << 3,           // the mask layers
    StyleParts_Filter = 1 << 4,
    StyleParts_Compositing = 1 << 5,    // mix-blend-mode and isolation
    StyleParts_Masking = StyleParts_ClipPath | StyleParts_Mask,
    StyleParts_Effects = StyleParts_Opacity | StyleParts_Masking | StyleParts_Filter | StyleParts_Compositing,
    StyleParts_All = StyleParts_Transform | StyleParts_Effects,
};
typedef uint8_t StyleParts;

// Returns true if a URL names a fragment of the document, as a reference to an element does.
bool isFragment(PrStringView url) {
    return url.size != 0 && url.data[0] == '#';
}

// Returns a computed <length-percentage> as a layout length without a tree: a length or a percentage as it is, and of a
// calculation of both, the length alone, since a compound length needs a tree to hold it.
PrLayoutLength treelessLength(void* PR_NULLABLE, CssValueRun run) {
    if (run.count == 0)
        return prLayoutLengthAuto();
    if (run.values[0].kind == PrCssValueKind_Percentage)
        return prLayoutLengthPercent(float(run.values[0].number));
    double none = 0;
    double whole = 0;
    if (!cssResolveLength(run, 0, 0, &none) || !cssResolveLength(run, 0, 100, &whole))
        return prLayoutLengthAuto();
    return none == 0 && whole != 0 ? prLayoutLengthPercent(float(whole)) : prLayoutLengthPx(float(none));
}

// Returns an axis of a computed <position> as a layout length without a tree. An offset from the right or the bottom by
// a length is a calculation, of which the percentage alone is kept.
PrLayoutLength treelessPosition(void* PR_NULLABLE, CssValueRun run, uint32_t index, uint32_t offset) {
    const CssValueNode* value = &run.values[index];
    if (value->kind != PrCssValueKind_Keyword)
        return treelessLength(nullptr, cssValueSubtree(run, index));
    PrCssKeyword keyword = cssValueKeyword(value);
    if (keyword == PrCssKeyword_Center)
        return prLayoutLengthPercent(50);
    bool far = keyword == PrCssKeyword_Right || keyword == PrCssKeyword_Bottom;
    if (offset == kArrayMissing)
        return prLayoutLengthPercent(far ? 100 : 0);
    const CssValueNode* by = &run.values[offset];
    if (!far)
        return treelessLength(nullptr, cssValueSubtree(run, offset));
    return prLayoutLengthPercent(by->kind == PrCssValueKind_Percentage ? 100 - float(by->number) : 100);
}

// Writes the gradient of a computed mask-image item to `layer`, with its colors resolved for `style`.
void readGradient(SvgBuilder& b, const CssComputedStyle* style, CssValueRun image, SvgMaskLayer* layer) {
    RawArray<PrLayoutColorStop> stops;
    CssColorContext colors = cssStyleColorContext(style, b.build->environment);
    cssGradientFromValue(image, 0, colors, 0, CssLayoutLengths{treelessLength, treelessPosition, nullptr}, stops,
        &layer->gradient);
    if (stops.count != 0) {
        layer->stops = static_cast<PrLayoutColorStop*>(prMemoryAlloc(stops.count * sizeof(PrLayoutColorStop)));
        prMemCopy(layer->stops, stops.data, stops.count * sizeof(PrLayoutColorStop));
    }
    layer->gradient.stops = layer->stops;
    layer->gradient.stopCount = stops.count;
    stops.reset();
}

// Reads the items of the filter list that the style gives `element`, with the filter elements that their URLs name
// resolved. A URL that names no filter element ignores the whole list (filter-effects-1/#FilterProperty), which leaves
// `out` empty.
void readFilters(SvgBuilder& b, const PrDomElement* element, const CssComputedStyle* style, CssValueRun filter,
        RawArray<SvgFilterItem>* out) {
    RawArray<CssFilterFunction> functions;
    cssFilterFunctions(filter, cssStyleColorContext(style, b.build->environment), &functions);
    for (uint32_t i = 0; i < functions.count; ++i) {
        SvgFilterItem item{.function = functions.data[i], .filter = kSvgResourceNone};
        if (item.function.filter.kind == PrLayoutFilterKind_Source) {
            const PrDomElement* target = svgBuildReference(b, element, item.function.url, false);
            item.filter = target != nullptr ? resourceOf(b, target, SvgResourceKind_Filter) : kSvgResourceNone;
            if (item.filter == kSvgResourceNone) {
                out->clear();
                break;
            }
        }
        out->push(item);
    }
    functions.reset();
}

// Reads the effects that the style gives the node at `index`, those of `parts`, and resolves the elements that their
// URLs name. A reference to an element that is not a clipPath clips nothing (css-masking-1/#the-clip-path). A mask
// layer that references no mask element, or whose image cannot be displayed, masks nothing.
void readEffects(SvgBuilder& b, SvgNodeIndex index, const CssComputedStyle* style, StyleParts parts) {
    b.tree->nodes.data[index].effects = kArrayMissing;
    SvgTree* tree = b.tree;
    const PrDomElement* element = tree->nodes.data[index].element;
    const CssStyleValues* values = style->groups[CssStyleGroup_Compositing];
    CssValueRun clip = cssComputedValue(style, PrCssProperty_ClipPath);
    CssValueRun images = cssComputedValue(style, PrCssProperty_MaskImage);
    CssValueRun filter = cssComputedValue(style, PrCssProperty_Filter);
    PrCssKeyword blend = svgStyleKeyword(style, PrCssProperty_MixBlendMode);
    // A 3D transform isolates the group of the node that takes the transform, as isolation: isolate does
    // (compositing-1/#csscompositingrules_SVG).
    bool isolated = svgStyleIs(style, PrCssProperty_Isolation, PrCssKeyword_Isolate) ||
        ((parts & StyleParts_Transform) != 0 && svgStyleTransforms3d(style));
    // Most elements have no effect: their clip-path and filter are none, their mask-image a single none, their
    // mix-blend-mode normal, their isolation auto and their transform two-dimensional.
    bool compositing = (parts & StyleParts_Compositing) != 0;
    bool clips = (parts & StyleParts_ClipPath) != 0 && clip.count != 0 &&
        !cssValueIsKeyword(clip.values, PrCssKeyword_None);
    bool masks = (parts & StyleParts_Mask) != 0 &&
        !(images.count == 1 && cssValueIsKeyword(images.values, PrCssKeyword_None));
    bool filters = (parts & StyleParts_Filter) != 0 && filter.count != 0 &&
        !cssValueIsKeyword(filter.values, PrCssKeyword_None);
    bool composites = compositing && (blend != PrCssKeyword_Normal || isolated);
    if (!clips && !masks && !filters && !composites)
        return;
    SvgEffects effects{.values = values, .clipPath = kSvgResourceNone};
    if (clips) {
        PrStringView url = svgRunUrl(clip);
        if (url.data != nullptr) {
            const PrDomElement* target = svgBuildReference(b, element, url, false);
            effects.clipPath = target != nullptr ? resourceOf(b, target, SvgResourceKind_ClipPath) : kSvgResourceNone;
            effects.clip = effects.clipPath != kSvgResourceNone ? SvgClipKind_Url : SvgClipKind_None;
        } else {
            effects.clip = SvgClipKind_Box;
            for (uint32_t i = 0; i < clip.count; i += clip.values[i].next) {
                if (cssIsBasicShape(clip, i))
                    effects.clip = SvgClipKind_Shape;
            }
        }
    }
    CssListItems items = cssListItems(images);
    bool layered = false;
    for (uint32_t i = 0; masks && i < items.count; ++i)
        layered = layered || !cssValueIsKeyword(&images.values[items.starts[i]], PrCssKeyword_None);
    for (uint32_t i = 0; layered && i < items.count; ++i) {
        CssValueRun image = cssListItem(images, items, i);
        SvgMaskLayer layer{.kind = SvgMaskLayerKind_None, .mask = kSvgResourceNone};
        PrStringView url = svgRunUrl(image);
        if (url.data != nullptr && isFragment(url)) {
            const PrDomElement* target = svgBuildReference(b, element, url, false);
            layer.mask = target != nullptr ? resourceOf(b, target, SvgResourceKind_Mask) : kSvgResourceNone;
            layer.kind = layer.mask != kSvgResourceNone ? SvgMaskLayerKind_Mask : SvgMaskLayerKind_Error;
        } else if (url.data != nullptr) {
            // An image of another document, which masks once it has loaded.
            layer.kind = SvgMaskLayerKind_Image;
            const SvgImageSource& source = b.build->images;
            if (source.load != nullptr)
                source.load(source.context, element, url, &layer.picture);
        } else if (cssIsGradient(image)) {
            layer.kind = SvgMaskLayerKind_Gradient;
            readGradient(b, style, image, &layer);
        } else if (!cssValueIsKeyword(image.values, PrCssKeyword_None)) {
            // An image that the engine does not draw, such as image-set(), is one that cannot be displayed.
            layer.kind = SvgMaskLayerKind_Error;
        }
        effects.layers.push(layer);
    }
    if (filters)
        readFilters(b, element, style, filter, &effects.filters);
    effects.blend = PrDrawBlendMode_Normal;
    effects.composite = PrDrawComposite_SourceOver;
    if (compositing && blend == PrCssKeyword_PlusLighter)
        effects.composite = PrDrawComposite_PlusLighter;
    else if (compositing)
        cssBlendModeFromKeyword(blend, &effects.blend);
    effects.isolated = compositing && isolated;
    bool none = effects.clip == SvgClipKind_None && effects.layers.count == 0 && effects.filters.count == 0 &&
        effects.blend == PrDrawBlendMode_Normal && effects.composite == PrDrawComposite_SourceOver && !effects.isolated;
    if (none) {
        effects.layers.reset();
        effects.filters.reset();
        return;
    }
    cssStyleValuesRetain(values);
    uint32_t at = take(tree->effects, tree->freeEffects, effects);
    tree->nodes.data[index].effects = at;
}

// Reads the values of a node that come from its element's style: its visibility, fill and clip rules, decorations,
// paints, and those of `parts`.
void readStyle(SvgBuilder& b, SvgNodeIndex index, const CssComputedStyle* style, StyleParts parts) {
    SvgTree* tree = b.tree;
    SvgNode& node = tree->nodes.data[index];
    PrCssKeyword visibility = svgStyleKeyword(style, PrCssProperty_Visibility);
    node.visible = visibility != PrCssKeyword_Hidden && visibility != PrCssKeyword_Collapse;
    node.fillRule = svgStyleFillRule(style, PrCssProperty_FillRule);
    node.clipRule = svgStyleFillRule(style, PrCssProperty_ClipRule);
    node.lines = svgStyleLines(style);
    node.opacity = (parts & StyleParts_Opacity) != 0 ? prF32Clamp(svgStyleNumber(style, PrCssProperty_Opacity, 1), 0, 1) : 1;
    node.linear = svgStyleKeyword(style, PrCssProperty_ColorInterpolation) == PrCssKeyword_Linearrgb;
    node.pointerEvents = svgStylePointerEvents(style);
    node.vectorEffects = svgStyleVectorEffects(style);
    node.transform = prAffine2Identity();
    node.transformStyle = kArrayMissing;
    if ((parts & StyleParts_Transform) != 0) {
        SvgTransformStyle transform = svgStyleTransform(style);
        bool fixed = transform.any && transform.origin[0].percent == 0 && transform.origin[1].percent == 0 &&
            !readsBox(transform.list) && (transform.box == PrCssKeyword_ViewBox || transform.box == PrCssKeyword_Count);
        if (fixed)
            node.transform = svgTransformMatrix(transform, PrRect{});
        else if (transform.any)
            node.transformStyle = take(tree->transforms, tree->freeTransforms, transform);
    }
    SvgPaints paints = svgBuildPaints(b, style, tree->nodes.data[index].element);
    tree->nodes.data[index].paints = paints;
    readEffects(b, index, style, parts);
}

// Adds a node of `kind` for `element` under `parent`, with the values that it takes from `style`, those of `parts`
// among them, and returns its index. `own` says whether the node is the element's own.
SvgNodeIndex addNode(SvgBuilder& b, SvgNodeKind kind, const PrDomElement* element, const CssComputedStyle* style,
        SvgNodeIndex parent, StyleParts parts, bool own) {
    SvgNode node{};
    node.kind = kind;
    node.element = element;
    node.transformStyle = kArrayMissing;
    node.paints.strokeStyle = kArrayMissing;
    node.markers = kArrayMissing;
    node.effects = kArrayMissing;
    SvgNodeIndex index = svgBuildNode(b, node, parent, own);
    readStyle(b, index, style, parts);
    return index;
}

// Reads the vertex markers of a shape's node from its style (SVG2/painting.html#VertexMarkerProperties). A reference to
// an element that is not a marker draws no marker.
void readMarkers(SvgBuilder& b, SvgNodeIndex index, const CssComputedStyle* style) {
    const PrDomElement* element = b.tree->nodes.data[index].element;
    auto markerOf = [&](PrCssProperty property) {
        PrStringView url = svgStyleUrl(style, property);
        const PrDomElement* target = url.data != nullptr ? svgBuildReference(b, element, url, false) : nullptr;
        return target != nullptr ? resourceOf(b, target, SvgResourceKind_Marker) : kSvgResourceNone;
    };
    SvgMarkers markers{
        .start = markerOf(PrCssProperty_MarkerStart),
        .mid = markerOf(PrCssProperty_MarkerMid),
        .end = markerOf(PrCssProperty_MarkerEnd),
        .strokeWidth = svgStyleLength(style, PrCssProperty_StrokeWidth),
    };
    if (markers.start == kSvgResourceNone && markers.mid == kSvgResourceNone && markers.end == kSvgResourceNone)
        return;
    b.tree->nodes.data[index].markers = take(b.tree->markers, b.tree->freeMarkers, markers);
}

// Adds a node of a shape, as addNode does, with its vertex markers.
SvgNodeIndex addShape(SvgBuilder& b, SvgNodeKind kind, const PrDomElement* element, const CssComputedStyle* style,
        SvgNodeIndex parent, bool own) {
    SvgNodeIndex index = addNode(b, kind, element, style, parent, StyleParts_All, own);
    readMarkers(b, index, style);
    return index;
}

bool isGradient(const PrDomElement* e) {
    return svgIs(e, PrDomTag_LinearGradient) || svgIs(e, PrDomTag_RadialGradient);
}

bool isPattern(const PrDomElement* e) {
    return svgIs(e, PrDomTag_Pattern);
}

// Returns true if an element is a descriptive element, which does not count as a paint server's content
// (SVG2/pservers.html#PaintServerTemplates).
bool isDescriptive(const PrDomElement* e) {
    return svgIs(e, PrDomTag_Desc) || svgIs(e, PrDomTag_Title) || svgIs(e, PrDomTag_Metadata);
}

// Writes the paint server's chain of templates through href to `chain`: the server, then each one that it references,
// until one is not of a kind that `templates` accepts or a cycle stops the chain
// (SVG2/pservers.html#PaintServerTemplates).
uint32_t templateChain(SvgBuilder& b, const PrDomElement* server, bool (*templates)(const PrDomElement*),
        const PrDomElement** chain) {
    uint32_t count = 0;
    for (const PrDomElement* e = server; e != nullptr && templates(e) && count < kMaxReferences; e = svgBuildHref(b, e, true)) {
        for (uint32_t i = 0; i < count; ++i) {
            if (chain[i] == e)
                return count;
        }
        chain[count++] = e;
    }
    return count;
}

// Returns the first gradient of the chain that has the attribute `name`, or null. A coordinate is taken only from a
// gradient of the same kind as the first, and the attributes that both kinds have from either.
const PrDomElement* PR_NULLABLE chainWith(const PrDomElement* const* chain, uint32_t count, PrStringView name,
        bool sameKind) {
    for (uint32_t i = 0; i < count; ++i) {
        if (sameKind && chain[i]->tag != chain[0]->tag)
            continue;
        if (svgAttribute(chain[i], name).data != nullptr)
            return chain[i];
    }
    return nullptr;
}

// Returns the transform of the first server of the chain whose transform property is not none, which the server's
// gradientTransform or patternTransform is the presentation attribute of. An attribute that does not parse declares
// nothing, and leaves the transform to the templates. Returns the identity if no server has one.
PrAffine2 chainTransform(SvgBuilder& b, const PrDomElement* const* chain, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const CssComputedStyle* style = svgBuildStyle(b, chain[i]);
        SvgTransformStyle transform = style != nullptr ? svgStyleTransform(style) : SvgTransformStyle{};
        if (transform.any)
            return svgTransformMatrix(transform, PrRect{});
    }
    return prAffine2Identity();
}

// Appends the stops of the first gradient of the chain that has any to `stops`. Offsets are clamped to [0, 1] and made
// nondecreasing, and colors are multiplied by their stop-opacity (SVG2/pservers.html#StopElement). The stops of a
// template are the gradient's shadow tree, whose clones inherit from the gradient, not from the template.
void addStops(SvgBuilder& b, const PrDomElement* const* chain, uint32_t count, const CssComputedStyle* host,
        RawArray<PrDrawColorStop>* stops) {
    for (uint32_t i = 0; i < count && stops->count == 0; ++i) {
        float last = 0;
        for (const PrDomNode* child = chain[i]->first; child != nullptr; child = child->next) {
            const PrDomElement* stop = domElementFromNode(child);
            if (stop == nullptr || !svgIs(stop, PrDomTag_Stop))
                continue;
            bool clone = i != 0;
            const CssComputedStyle* style = clone ? svgBuildStyleUnder(b, stop, host) : svgBuildStyle(b, stop);
            if (style == nullptr)
                continue;
            SvgLengthPercentage offset = svgAttributeLength(stop, "offset", SvgLengthPercentage{}, nullptr);
            float value = prF32Max(prF32Clamp(offset.px + offset.percent / 100, 0, 1), last);
            last = value;
            PrColor color = svgStyleColor(style, PrCssProperty_StopColor, b.build->environment);
            color.a *= prF32Clamp(svgStyleNumber(style, PrCssProperty_StopOpacity, 1), 0, 1);
            stops->push(PrDrawColorStop{.offset = value, .color = color});
            if (clone)
                cssComputedStyleRelease(style);
        }
    }
}

// Builds a gradient from its element.
void buildGradient(SvgBuilder& b, SvgResourceIndex index, const PrDomElement* element) {
    const PrDomElement* chain[kMaxReferences];
    uint32_t count = templateChain(b, element, isGradient, chain);
    const CssComputedStyle* own = svgBuildStyle(b, element);
    SvgGradient g{};
    g.radial = svgIs(element, PrDomTag_RadialGradient);
    const PrDomElement* units = chainWith(chain, count, "gradientUnits", false);
    g.userSpace = units != nullptr && svgAttribute(units, "gradientUnits") == "userSpaceOnUse";
    const PrDomElement* spread = chainWith(chain, count, "spreadMethod", false);
    PrStringView method = spread != nullptr ? svgAttribute(spread, "spreadMethod") : PrStringView{};
    g.extend = method == "reflect" ? PrDrawExtend_Reflect : method == "repeat" ? PrDrawExtend_Repeat : PrDrawExtend_Pad;
    g.transform = chainTransform(b, chain, count);
    // The gradient that the paint references gives the space, whichever gradient gives the stops
    // (SVG2/painting.html#ColorInterpolation).
    bool linear = own != nullptr && svgStyleKeyword(own, PrCssProperty_ColorInterpolation) == PrCssKeyword_Linearrgb;
    g.space = linear ? PrColorSpace_SrgbLinear : PrColorSpace_Srgb;
    auto coordinate = [&](PrStringView name, SvgLengthPercentage fallback) {
        const PrDomElement* from = chainWith(chain, count, name, true);
        return from != nullptr ? svgAttributeLength(from, name, fallback, own) : fallback;
    };
    if (g.radial) {
        g.cx = coordinate("cx", SvgLengthPercentage{0, 50});
        g.cy = coordinate("cy", SvgLengthPercentage{0, 50});
        g.r = coordinate("r", SvgLengthPercentage{0, 50});
        g.fr = coordinate("fr", SvgLengthPercentage{});
        g.hasFx = chainWith(chain, count, "fx", true) != nullptr;
        g.hasFy = chainWith(chain, count, "fy", true) != nullptr;
        g.fx = coordinate("fx", g.cx);
        g.fy = coordinate("fy", g.cy);
    } else {
        g.x1 = coordinate("x1", SvgLengthPercentage{});
        g.y1 = coordinate("y1", SvgLengthPercentage{});
        g.x2 = coordinate("x2", SvgLengthPercentage{0, 100});
        g.y2 = coordinate("y2", SvgLengthPercentage{});
    }
    RawArray<PrDrawColorStop> stops;
    addStops(b, chain, count, own, &stops);
    if (stops.count != 0) {
        g.stops = static_cast<PrDrawColorStop*>(prMemoryAlloc(stops.count * sizeof(PrDrawColorStop)));
        prMemCopy(g.stops, stops.data, stops.count * sizeof(PrDrawColorStop));
        g.stopCount = stops.count;
    }
    stops.reset();
    b.tree->resources.data[index].gradient = g;
}

void buildChildren(SvgBuilder& b, const PrDomElement* element, const CssComputedStyle* style, SvgNodeIndex parent, bool instance);

// Parses an angle: a number in degrees, or a number with an angle's unit. Returns false if `text` is not an angle.
bool parseAngle(PrStringView text, float* radians) {
    size_t pos = 0;
    float number = 0;
    if (!numberListRead(text, &pos, &number))
        return false;
    PrStringView unit = prStringTrimEnd(prStringSlice(text, pos, text.size - pos));
    double scale = kPrF64Pi / 180;
    if (unit.size != 0) {
        PrCssUnit css = cssUnitFromText(unit);
        if (css == PrCssUnit_Unknown || cssUnitCategory(css) != CssUnitCategory_Angle)
            return false;
        scale *= cssUnitScale(css);
    }
    *radians = float(double(number) * scale);
    return true;
}

// Writes the reference point that refX or refY gives to `out`: a length or a number in the content's user space, or a
// keyword, which is a percentage (SVG2/painting.html#MarkerElementRefXAttribute). `start` and `end` are the keywords
// for 0% and 100% along the attribute's axis. Returns false, and writes zero, if the attribute is missing or does not
// parse.
bool referenceOf(const PrDomElement* e, PrStringView name, PrStringView start, PrStringView end,
        const CssComputedStyle* PR_NULLABLE style, SvgLengthPercentage* out) {
    PrStringView value = svgAttribute(e, name);
    *out = SvgLengthPercentage{};
    if (value.data == nullptr)
        return false;
    if (value == start)
        return true;
    if (value == "center")
        *out = SvgLengthPercentage{0, 50};
    else if (value == end)
        *out = SvgLengthPercentage{0, 100};
    else
        return svgParseLength(value, style, out);
    return true;
}

// Adds a group that no node holds, for the content of the resource `e`, and returns its index.
SvgNodeIndex addContentGroup(SvgBuilder& b, const PrDomElement* e) {
    SvgNode group{.kind = SvgNodeKind_Group, .visible = true, .opacity = 1, .element = e};
    group.transform = prAffine2Identity();
    group.transformStyle = kArrayMissing;
    group.paints.strokeStyle = kArrayMissing;
    group.markers = kArrayMissing;
    group.effects = kArrayMissing;
    return svgBuildNode(b, group, kSvgNodeNone, false);
}

// Builds a marker from its element (SVG2/painting.html#MarkerElement). Its children are built as the content, which
// inherits from the marker, under a group that no node holds. A marker with no style draws nothing.
void buildMarker(SvgBuilder& b, SvgResourceIndex index, const PrDomElement* e) {
    const CssComputedStyle* style = svgBuildStyle(b, e);
    SvgMarker m{};
    m.content = kSvgNodeNone;
    // A negative size is an error, which renders nothing, as a size of zero does.
    m.width = svgAttributeLength(e, "markerWidth", SvgLengthPercentage{3, 0}, style);
    m.height = svgAttributeLength(e, "markerHeight", SvgLengthPercentage{3, 0}, style);
    m.hasViewBox = svgParseViewBox(svgAttribute(e, "viewBox"), &m.viewBox);
    svgParseAspect(svgAttribute(e, "preserveAspectRatio"), &m.aspect);
    m.strokeUnits = svgAttribute(e, "markerUnits") != "userSpaceOnUse";
    PrStringView orient = svgAttribute(e, "orient");
    if (orient == "auto")
        m.orient = SvgOrient_Auto;
    else if (orient == "auto-start-reverse")
        m.orient = SvgOrient_AutoStartReverse;
    else if (orient.data == nullptr || !parseAngle(orient, &m.angle))
        m.angle = 0;
    referenceOf(e, "refX", "left", "right", style, &m.refX);
    referenceOf(e, "refY", "top", "bottom", style, &m.refY);
    m.clips = true;
    if (style != nullptr) {
        PrCssKeyword overflow = svgStyleKeyword(style, PrCssProperty_OverflowX);
        m.clips = overflow != PrCssKeyword_Visible && overflow != PrCssKeyword_Auto;
        m.content = addContentGroup(b, e);
        // Text's percentages are of the content's user space, as far as the build can tell it.
        PrVec2 saved = b.viewport;
        if (m.hasViewBox)
            b.viewport = PrVec2{prRectWidth(m.viewBox), prRectHeight(m.viewBox)};
        else
            b.viewport = PrVec2{svgLengthResolve(m.width, saved.x), svgLengthResolve(m.height, saved.y)};
        buildChildren(b, e, style, m.content, false);
        b.viewport = saved;
    }
    b.tree->resources.data[index].marker = m;
}

// Returns true if an element has children other than descriptive elements, which a pattern takes as its content.
bool hasContent(const PrDomElement* e) {
    for (const PrDomNode* child = e->first; child != nullptr; child = child->next) {
        const PrDomElement* element = domElementFromNode(child);
        if (element != nullptr && !isDescriptive(element))
            return true;
    }
    return false;
}

// Builds a pattern from its element and the chain of its templates (SVG2/pservers.html#PatternElement). Each attribute
// comes from the first pattern of the chain that has it. The content is the children of the first that has any, and
// inherits from the pattern: a template's children are the pattern's shadow tree. A pattern with no style draws
// nothing.
void buildPattern(SvgBuilder& b, SvgResourceIndex index, const PrDomElement* e) {
    const PrDomElement* chain[kMaxReferences];
    uint32_t count = templateChain(b, e, isPattern, chain);
    const CssComputedStyle* style = svgBuildStyle(b, e);
    SvgPattern p{};
    p.content = kSvgNodeNone;
    auto from = [&](PrStringView name) { return chainWith(chain, count, name, false); };
    const PrDomElement* units = from("patternUnits");
    p.userSpace = units != nullptr && svgAttribute(units, "patternUnits") == "userSpaceOnUse";
    const PrDomElement* contentUnits = from("patternContentUnits");
    p.contentBoundingBox = contentUnits != nullptr &&
        svgAttribute(contentUnits, "patternContentUnits") == "objectBoundingBox";
    const PrDomElement* viewBox = from("viewBox");
    p.hasViewBox = viewBox != nullptr && svgParseViewBox(svgAttribute(viewBox, "viewBox"), &p.viewBox);
    const PrDomElement* aspect = from("preserveAspectRatio");
    svgParseAspect(aspect != nullptr ? svgAttribute(aspect, "preserveAspectRatio") : PrStringView{}, &p.aspect);
    auto length = [&](PrStringView name) {
        const PrDomElement* with = from(name);
        return with != nullptr ? svgAttributeLength(with, name, SvgLengthPercentage{}, style) : SvgLengthPercentage{};
    };
    p.x = length("x");
    p.y = length("y");
    p.width = length("width");
    p.height = length("height");
    p.transform = chainTransform(b, chain, count);
    const PrDomElement* source = nullptr;
    for (uint32_t i = 0; i < count && source == nullptr; ++i)
        source = hasContent(chain[i]) ? chain[i] : nullptr;
    if (style != nullptr && source != nullptr) {
        p.content = addContentGroup(b, e);
        buildChildren(b, source, style, p.content, source != e);
    }
    b.tree->resources.data[index].pattern = p;
}

// Returns true if an element is a shape, whose geometry a clipPath takes as a silhouette.
bool isShape(const PrDomElement* e) {
    switch (e->space == PrDomNamespace_Svg ? e->tag : PrDomTag_Count) {
        case PrDomTag_Rect:
        case PrDomTag_Circle:
        case PrDomTag_Ellipse:
        case PrDomTag_Line:
        case PrDomTag_Polyline:
        case PrDomTag_Polygon:
        case PrDomTag_Path:
            return true;
        default:
            return false;
    }
}

// Returns true if a child of a clipPath contributes to its clip path: a shape, a text element, or a use element whose
// shadow tree is one (css-masking-1/#ClipPathElement). A use element that references any other element is an error,
// whose silhouette is empty.
bool contributesToClip(const PrDomElement* e) {
    if (isShape(e) || svgIs(e, PrDomTag_Text))
        return true;
    if (!svgIs(e, PrDomTag_Use))
        return false;
    const PrDomShadowRoot* shadow = domShadowRootOfHost(e);
    const PrDomElement* instance = shadow != nullptr ? domElementFromNode(shadow->first) : nullptr;
    return instance != nullptr && (isShape(instance) || svgIs(instance, PrDomTag_Text));
}

// Builds a clipPath from its element (css-masking-1/#ClipPathElement). The children that contribute are built under a
// group that takes the clipPath's own clip-path. A clipPath with no style clips everything away, as an empty one does.
void buildClipPath(SvgBuilder& b, SvgResourceIndex index, const PrDomElement* e) {
    const CssComputedStyle* style = svgBuildStyle(b, e);
    SvgClipPath c{.transform = prAffine2Identity(), .content = kSvgNodeNone};
    c.boundingBox = svgAttribute(e, "clipPathUnits") == "objectBoundingBox";
    if (style != nullptr) {
        c.transform = svgTransformMatrix(svgStyleTransform(style), PrRect{});
        c.content = addContentGroup(b, e);
        readEffects(b, c.content, style, StyleParts_ClipPath);
        for (const PrDomNode* child = e->first; child != nullptr; child = child->next) {
            const PrDomElement* element = domElementFromNode(child);
            if (element != nullptr && builds(element) && contributesToClip(element))
                buildElement(b, element, style, c.content, false);
        }
    }
    b.tree->resources.data[index].clipPath = c;
}

// Builds a mask from its element (css-masking-1/#MaskElement). Its region defaults to -10% and 120% of the bounding box
// in each axis. Its children are built under a group that takes the mask's own clip-path and mask. A mask with no style
// masks everything away, as an empty one does.
void buildMask(SvgBuilder& b, SvgResourceIndex index, const PrDomElement* e) {
    const CssComputedStyle* style = svgBuildStyle(b, e);
    SvgMask m{.content = kSvgNodeNone};
    m.userSpace = svgAttribute(e, "maskUnits") == "userSpaceOnUse";
    m.contentBoundingBox = svgAttribute(e, "maskContentUnits") == "objectBoundingBox";
    m.x = svgAttributeLength(e, "x", SvgLengthPercentage{0, -10}, style);
    m.y = svgAttributeLength(e, "y", SvgLengthPercentage{0, -10}, style);
    m.width = svgAttributeLength(e, "width", SvgLengthPercentage{0, 120}, style);
    m.height = svgAttributeLength(e, "height", SvgLengthPercentage{0, 120}, style);
    m.mode = PrDrawMaskMode_Luminance;
    m.space = PrColorSpace_Srgb;
    if (style != nullptr) {
        bool alpha = svgStyleIs(style, PrCssProperty_MaskType, PrCssKeyword_Alpha);
        m.mode = alpha ? PrDrawMaskMode_Alpha : PrDrawMaskMode_Luminance;
        bool linear = svgStyleKeyword(style, PrCssProperty_ColorInterpolation) == PrCssKeyword_Linearrgb;
        m.space = linear ? PrColorSpace_SrgbLinear : PrColorSpace_Srgb;
        m.content = addContentGroup(b, e);
        readEffects(b, m.content, style, StyleParts_Masking);
        buildChildren(b, e, style, m.content, false);
    }
    b.tree->resources.data[index].mask = m;
}

// Builds a resource from its element, and the references that it follows depend on the resource. The resource reads its
// element's attributes and subtree.
void buildResource(SvgBuilder& b, SvgResourceIndex index) {
    SvgNodeIndex savedOwner = b.owner;
    SvgResourceIndex savedResource = b.ownerResource;
    b.owner = kSvgNodeNone;
    b.ownerResource = index;
    const PrDomElement* element = b.tree->resources.data[index].element;
    record(b, PrStringView{}, element, nullptr, true);
    switch (b.tree->resources.data[index].kind) {
        case SvgResourceKind_Marker: buildMarker(b, index, element); break;
        case SvgResourceKind_Pattern: buildPattern(b, index, element); break;
        case SvgResourceKind_ClipPath: buildClipPath(b, index, element); break;
        case SvgResourceKind_Mask: buildMask(b, index, element); break;
        case SvgResourceKind_Filter: svgBuildFilter(b, index, element); break;
        default: buildGradient(b, index, element); break;
    }
    b.owner = savedOwner;
    b.ownerResource = savedResource;
}

// Builds a resource again from its element.
void rebuildResource(SvgBuilder& b, SvgResourceIndex index) {
    resetResource(b.tree, index);
    buildResource(b, index);
}

// Returns the resource of `element`, built the first time that a node references it, or kSvgResourceNone if the element
// is not a resource of `kind`.
SvgResourceIndex resourceOf(SvgBuilder& b, const PrDomElement* element, SvgResourceKind kind) {
    SvgTree* tree = b.tree;
    for (uint32_t i = 0; i < tree->resources.count; ++i) {
        if (tree->resources.data[i].element == element)
            return tree->resources.data[i].kind == kind ? i : kSvgResourceNone;
    }
    bool matches = false;
    switch (kind) {
        case SvgResourceKind_Marker: matches = svgIs(element, PrDomTag_Marker); break;
        case SvgResourceKind_Pattern: matches = isPattern(element); break;
        case SvgResourceKind_ClipPath: matches = svgIs(element, PrDomTag_ClipPath); break;
        case SvgResourceKind_Mask: matches = svgIs(element, PrDomTag_Mask); break;
        case SvgResourceKind_Filter: matches = svgIs(element, PrDomTag_Filter); break;
        default: matches = isGradient(element); break;
    }
    if (!matches)
        return kSvgResourceNone;
    domNodeRetain(const_cast<PrDomElement*>(element));
    SvgResource resource{.element = element, .firstReference = kArrayMissing};
    clearResource(resource, kind);
    SvgResourceIndex index = take(tree->resources, tree->freeResources, resource);
    buildResource(b, index);
    return index;
}

// Returns the paint of `paint`, with the server that its URL names resolved.
SvgPaint resolvePaint(SvgBuilder& b, const SvgPaintStyle& paint, const PrDomElement* element) {
    SvgPaint resolved{.kind = paint.kind, .fallback = paint.fallback, .color = paint.color, .server = kSvgResourceNone};
    if (paint.kind != SvgPaintKind_Url)
        return resolved;
    const PrDomElement* server = svgBuildReference(b, element, paint.url, false);
    if (server != nullptr)
        resolved.server = resourceOf(b, server, isPattern(server) ? SvgResourceKind_Pattern : SvgResourceKind_Gradient);
    return resolved;
}

// Sets the builder's viewport to the one that `v` establishes, as far as the build can tell it, records it in `v`, and
// returns the one it replaces. A viewBox gives its size. Otherwise percentages of the width and height are of the
// enclosing viewport.
PrVec2 enterViewport(SvgBuilder& b, SvgViewportShape* v) {
    PrVec2 saved = b.viewport;
    if (v->hasViewBox)
        b.viewport = PrVec2{prRectWidth(v->viewBox), prRectHeight(v->viewBox)};
    else if (!v->fills)
        b.viewport = PrVec2{svgLengthResolve(v->width, saved.x), svgLengthResolve(v->height, saved.y)};
    v->userExtent = b.viewport;
    return saved;
}

// Builds the children of `element`, whose style is `style`. Children in another namespace, and children whose
// conditional processing attributes fail, are skipped. A switch builds only its first child that passes.
void buildChildren(SvgBuilder& b, const PrDomElement* element, const CssComputedStyle* style, SvgNodeIndex parent,
        bool instance) {
    bool isSwitch = svgIs(element, PrDomTag_Switch);
    for (const PrDomNode* child = element->first; child != nullptr; child = child->next) {
        const PrDomElement* e = domElementFromNode(child);
        if (e == nullptr || !builds(e))
            continue;
        buildElement(b, e, style, parent, instance);
        if (isSwitch)
            return;
    }
}

// Returns the viewport that an svg or a symbol element establishes, at the position and size given.
SvgViewportShape viewportOf(const PrDomElement* e, const CssComputedStyle* style, SvgLengthPercentage x,
        SvgLengthPercentage y, SvgLengthPercentage width, SvgLengthPercentage height) {
    SvgViewportShape v{};
    v.x = x;
    v.y = y;
    v.width = width;
    v.height = height;
    v.hasViewBox = svgParseViewBox(svgAttribute(e, "viewBox"), &v.viewBox);
    svgParseAspect(svgAttribute(e, "preserveAspectRatio"), &v.aspect);
    PrCssKeyword overflow = svgStyleKeyword(style, PrCssProperty_OverflowX);
    v.clips = overflow != PrCssKeyword_Visible && overflow != PrCssKeyword_Auto;
    return v;
}

// Returns the viewport that an svg element of the document establishes inside another. Its x and y are geometry
// properties (SVG2/geometry.html#X). Its width and height are attributes alone, which CSS does not size
// (w3c/svgwg#1057), and auto or a missing attribute is 100%.
SvgViewportShape innerViewportOf(const PrDomElement* e, const CssComputedStyle* style) {
    SvgLengthPercentage full{0, 100};
    return viewportOf(e, style, svgStyleLength(style, PrCssProperty_X), svgStyleLength(style, PrCssProperty_Y),
        svgAttributeLength(e, "width", full, style), svgAttributeLength(e, "height", full, style));
}

// Returns the viewport of the root of a tree, an svg element in a viewport of its own: the box's content box, or the
// viewport that its width and height size.
SvgViewportShape rootViewportOf(const PrDomElement* e, const CssComputedStyle* style, bool box) {
    SvgLengthPercentage width = svgStyleLength(style, PrCssProperty_Width);
    SvgLengthPercentage height = svgStyleLength(style, PrCssProperty_Height);
    if (svgStyleIs(style, PrCssProperty_Width, PrCssKeyword_Auto))
        width = SvgLengthPercentage{0, 100};
    if (svgStyleIs(style, PrCssProperty_Height, PrCssKeyword_Auto))
        height = SvgLengthPercentage{0, 100};
    SvgViewportShape v = viewportOf(e, style, SvgLengthPercentage{}, SvgLengthPercentage{}, width, height);
    // The outermost viewport clips, whatever its overflow, since nothing outside it is drawn.
    v.clips = true;
    v.fills = box;
    return v;
}

// Builds an svg or a symbol as a viewport of `width` by `height`, as a use element sizes it. Its x and y are its
// computed geometry properties (SVG2/struct.html#SymbolElement). A symbol's width and height are attributes, which CSS
// does not set (w3c/svgwg#1057), and its refX and refY place a point of its content at x and y
// (SVG2/struct.html#SymbolAttributes).
void buildInstanceViewport(SvgBuilder& b, const PrDomElement* e, const CssComputedStyle* style,
        SvgNodeIndex parent, SvgLengthPercentage width, SvgLengthPercentage height) {
    bool svg = svgIs(e, PrDomTag_Svg);
    SvgLengthPercentage x = svgStyleLength(style, PrCssProperty_X);
    SvgLengthPercentage y = svgStyleLength(style, PrCssProperty_Y);
    SvgNodeIndex node = addNode(b, SvgNodeKind_Viewport, e, style, parent, StyleParts_All, svgBuildsOwn(b, false));
    SvgViewportShape& v = b.tree->nodes.data[node].viewport;
    v = viewportOf(e, style, x, y, width, height);
    if (!svg) {
        v.hasRefX = referenceOf(e, "refX", "left", "right", style, &v.refX);
        v.hasRefY = referenceOf(e, "refY", "top", "bottom", style, &v.refY);
    }
    PrVec2 saved = enterViewport(b, &b.tree->nodes.data[node].viewport);
    buildChildren(b, e, style, node, false);
    b.viewport = saved;
}

// Builds a use element's content, its shadow tree (SVG2/struct.html#UseShadowTree), under `shift`, its second node,
// which moves it by the use's x and y. The DOM keeps the shadow tree up to date with what the use references, and
// leaves it empty for a reference in error (dom_use.h). The instance root is styled as any element, inheriting from the
// use. A symbol or an svg takes the use element's width and height. An instance whose display computes to none draws
// nothing, a symbol's included, though SVG2/struct.html#SymbolNotes would display a symbol's instance inline.
void buildUse(SvgBuilder& b, const PrDomElement* e, const CssComputedStyle* style, SvgNodeIndex shift) {
    // A change to what the href names builds the use again, before the DOM's next synchronization reports the new
    // instances.
    svgBuildHref(b, e, false);
    const PrDomShadowRoot* shadow = domShadowRootOfHost(e);
    const PrDomElement* instance = shadow != nullptr ? domElementFromNode(shadow->first) : nullptr;
    if (instance == nullptr)
        return;
    if (svgIs(instance, PrDomTag_Symbol) || svgIs(instance, PrDomTag_Svg)) {
        const CssComputedStyle* instanceStyle = svgBuildStyle(b, instance);
        if (instanceStyle != nullptr && !svgStyleIs(instanceStyle, PrCssProperty_Display, PrCssKeyword_None)) {
            SvgLengthPercentage full{0, 100};
            SvgLengthPercentage width =
                svgAttributeLength(e, "width", svgAttributeLength(instance, "width", full, style), style);
            SvgLengthPercentage height =
                svgAttributeLength(e, "height", svgAttributeLength(instance, "height", full, style), style);
            buildInstanceViewport(b, instance, instanceStyle, shift, width, height);
        }
    } else {
        buildElement(b, instance, style, shift, false);
    }
}

// Returns how image-rendering asks for an image to be resampled (css-images-3/#the-image-rendering).
PrDrawSampling samplingOf(const CssComputedStyle* style) {
    switch (svgStyleKeyword(style, PrCssProperty_ImageRendering)) {
        case PrCssKeyword_Pixelated:
        case PrCssKeyword_CrispEdges:
        case PrCssKeyword_Optimizespeed: return PrDrawSampling_Nearest;
        default: return PrDrawSampling_Linear;
    }
}

// Returns how a computed object-fit fits an image in its positioning rectangle (css-images-4/#the-object-fit).
// scale-down combined with contain or cover behaves as scale-down alone.
PrLayoutObjectFit objectFitOf(const CssComputedStyle* style) {
    CssValueRun run = cssComputedValue(style, PrCssProperty_ObjectFit);
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (cssValueIsKeyword(&run.values[i], PrCssKeyword_ScaleDown))
            return PrLayoutObjectFit_ScaleDown;
    }
    switch (svgStyleKeyword(style, PrCssProperty_ObjectFit)) {
        case PrCssKeyword_Contain: return PrLayoutObjectFit_Contain;
        case PrCssKeyword_Cover: return PrLayoutObjectFit_Cover;
        case PrCssKeyword_None: return PrLayoutObjectFit_None;
        default: return PrLayoutObjectFit_Fill;
    }
}

// Reads an image element's positioning rectangle and placement from its style and attributes, and loads the image that
// its href names (SVG2/embedded.html#ImageElement). An image that has not loaded yet shows nothing until it has.
SvgImageShape imageShapeOf(SvgBuilder& b, const PrDomElement* e, const CssComputedStyle* style) {
    SvgImageShape shape{};
    shape.x = svgStyleLength(style, PrCssProperty_X);
    shape.y = svgStyleLength(style, PrCssProperty_Y);
    shape.width = svgStyleLength(style, PrCssProperty_Width);
    shape.height = svgStyleLength(style, PrCssProperty_Height);
    shape.widthAuto = svgStyleIs(style, PrCssProperty_Width, PrCssKeyword_Auto);
    shape.heightAuto = svgStyleIs(style, PrCssProperty_Height, PrCssKeyword_Auto);
    shape.limits = svgStyleSizeLimits(style);
    PrCssKeyword overflow = svgStyleKeyword(style, PrCssProperty_OverflowX);
    shape.clips = overflow != PrCssKeyword_Visible && overflow != PrCssKeyword_Auto;
    svgParseAspect(svgAttribute(e, "preserveAspectRatio"), &shape.aspect);
    shape.fit = objectFitOf(style);
    svgStylePosition(style, PrCssProperty_ObjectPosition, shape.position);
    shape.sampling = samplingOf(style);
    PrStringView href = svgHref(e);
    const SvgImageSource& images = b.build->images;
    if (href.size != 0 && images.load != nullptr)
        images.load(images.context, e, href, &shape.picture);
    return shape;
}

// Reads a foreignObject's positioning rectangle from its style, resolved in the nearest viewport as far as the build
// can tell it, and the layout root of its content (SVG2/embedded.html#Placement).
SvgForeignShape foreignShapeOf(SvgBuilder& b, const PrDomElement* e, const CssComputedStyle* style) {
    SvgForeignShape shape{};
    shape.x = svgStyleLength(style, PrCssProperty_X);
    shape.y = svgStyleLength(style, PrCssProperty_Y);
    shape.width = svgStyleLength(style, PrCssProperty_Width);
    shape.height = svgStyleLength(style, PrCssProperty_Height);
    shape.limits = svgStyleSizeLimits(style);
    PrCssKeyword overflow = svgStyleKeyword(style, PrCssProperty_OverflowX);
    shape.clips = overflow != PrCssKeyword_Visible && overflow != PrCssKeyword_Auto;
    shape.extent = svgLimitExtent(shape.limits, PrVec2{
        svgLengthResolve(shape.width, b.viewport.x),
        svgLengthResolve(shape.height, b.viewport.y),
    }, b.viewport);
    const SvgForeignSource& foreign = b.build->foreign;
    shape.root = foreign.root != nullptr ? foreign.root(foreign.context, e) : PrLayoutNode(kPrLayoutNodeNone);
    return shape;
}

// Builds `e` under `parent`, and returns the node that it added, or kSvgNodeNone if it adds none. `instance` says
// whether the element is in a use element's shadow tree, whose elements inherit from `parentStyle`. The first node of
// an element of the document is its own.
SvgNodeIndex buildElement(SvgBuilder& b, const PrDomElement* e, const CssComputedStyle* PR_NULLABLE parentStyle,
        SvgNodeIndex parent, bool instance) {
    if (neverRenders(e))
        return kSvgNodeNone;
    const CssComputedStyle* style = instance ? svgBuildStyleUnder(b, e, parentStyle) : svgBuildStyle(b, e);
    if (style == nullptr)
        return kSvgNodeNone;
    SvgNodeIndex savedOwner = b.owner;
    bool own = svgBuildsOwn(b, instance);
    SvgNodeIndex node = kSvgNodeNone;
    if (!svgStyleIs(style, PrCssProperty_Display, PrCssKeyword_None)) {
        switch (e->tag) {
            case PrDomTag_G:
            case PrDomTag_A:
            case PrDomTag_Switch:
                node = addNode(b, SvgNodeKind_Group, e, style, parent, StyleParts_All, own);
                buildChildren(b, e, style, node, instance);
                break;
            case PrDomTag_Svg: {
                node = addNode(b, SvgNodeKind_Viewport, e, style, parent, StyleParts_All, own);
                b.tree->nodes.data[node].viewport = innerViewportOf(e, style);
                PrVec2 saved = enterViewport(b, &b.tree->nodes.data[node].viewport);
                buildChildren(b, e, style, node, instance);
                b.viewport = saved;
                break;
            }
            case PrDomTag_Use: {
                // The use element's x and y are a translation after its own transform, which its clip path, mask and
                // filter apply in (SVG2/struct.html#UseLayout). The element's own node takes its transform, opacity and
                // blending, and a second node under it the translation and the effects that apply in the space that the
                // translation makes. Its bounding box and its CTM are its own node's: getBBox includes the translation,
                // and getCTM does not, which is what content on the web measures.
                StyleParts translated = StyleParts_Masking | StyleParts_Filter;
                node = addNode(b, SvgNodeKind_Group, e, style, parent, StyleParts_All & ~translated, own);
                b.tree->nodes.data[node].context = true;
                SvgLengthPercentage x = svgAttributeLength(e, "x", SvgLengthPercentage{}, style);
                SvgLengthPercentage y = svgAttributeLength(e, "y", SvgLengthPercentage{}, style);
                SvgNodeIndex shift = addNode(b, SvgNodeKind_Group, e, style, node, translated, false);
                SvgNode& moved = b.tree->nodes.data[shift];
                moved.transform = prAffine2Translate(PrVec2{x.px, y.px});
                // A percentage of x or y is of the viewport, which a viewport node resolves.
                if (x.percent != 0 || y.percent != 0) {
                    moved.kind = SvgNodeKind_Viewport;
                    moved.transform = prAffine2Identity();
                    moved.viewport = SvgViewportShape{
                        .x = x,
                        .y = y,
                        .width = SvgLengthPercentage{0, 100},
                        .height = SvgLengthPercentage{0, 100},
                        .fills = false,
                    };
                    moved.viewport.aspect = SvgAspect{};
                }
                buildUse(b, e, style, shift);
                break;
            }
            case PrDomTag_Rect:
            case PrDomTag_Circle:
            case PrDomTag_Ellipse:
            case PrDomTag_Line:
            case PrDomTag_Polyline:
            case PrDomTag_Polygon:
            case PrDomTag_Path: {
                SvgNode shape{};
                RawArray<PrPathDataCommand> commands;
                if (!svgReadShape(e, style, &shape, &commands)) {
                    commands.reset();
                    break;
                }
                node = addShape(b, shape.kind, e, style, parent, own);
                SvgNode& added = b.tree->nodes.data[node];
                switch (shape.kind) {
                    case SvgNodeKind_Rect: added.rect = shape.rect; break;
                    case SvgNodeKind_Ellipse: added.ellipse = shape.ellipse; break;
                    case SvgNodeKind_Line: added.line = shape.line; break;
                    default: {
                        PrPath* path = prPathCreate();
                        prPathAppendCommands(path, commands.data, commands.count);
                        added.path.path = path;
                        added.path.bounds = prPathTightBounds(path);
                        if (added.markers != kArrayMissing)
                            svgMarkerVertices(commands.data, commands.count,
                                &b.tree->markers.data[added.markers].vertices);
                        break;
                    }
                }
                commands.reset();
                break;
            }
            case PrDomTag_Text:
            case PrDomTag_TextArea:
                node = svgBuildText(b, e, style, parent, instance);
                break;
            case PrDomTag_Image: {
                SvgImageShape shape = imageShapeOf(b, e, style);
                node = addNode(b, SvgNodeKind_Image, e, style, parent, StyleParts_All, own);
                b.tree->nodes.data[node].image = shape;
                break;
            }
            case PrDomTag_ForeignObject: {
                // The content of an instance in a use element's shadow tree has no layout root, and draws nothing.
                SvgForeignShape shape = foreignShapeOf(b, e, style);
                if (instance)
                    shape.root = kPrLayoutNodeNone;
                node = addNode(b, SvgNodeKind_ForeignObject, e, style, parent, StyleParts_All, own);
                b.tree->nodes.data[node].foreign = shape;
                break;
            }
            default:
                break;
        }
    }
    b.owner = savedOwner;
    if (instance)
        cssComputedStyleRelease(style);
    return node;
}

// Returns what the root's node takes from the style of its svg element. The box of an outermost svg element applies its
// transform, opacity, clip path, mask, filter and blending (SVG2/render.html#TermReplacedElement), and an SVG image's
// placement applies its transform.
StyleParts rootParts(SvgRoot root) {
    switch (root) {
        case SvgRoot_Box: return StyleParts_None;
        case SvgRoot_Image: return StyleParts_Effects;
        default: return StyleParts_All;
    }
}

// Builds the root of a tree from an svg element, and its descendants.
void buildRoot(SvgBuilder& b, const PrDomElement* element, const CssComputedStyle* style, SvgRoot root) {
    bool box = root == SvgRoot_Box || root == SvgRoot_Image;
    SvgNodeIndex node = addNode(b, SvgNodeKind_Viewport, element, style, kSvgNodeNone, rootParts(root), true);
    b.tree->nodes.data[node].viewport = rootViewportOf(element, style, box);
    enterViewport(b, &b.tree->nodes.data[node].viewport);
    buildChildren(b, element, style, node, false);
}

// Frees what a build kept between elements.
void finish(SvgBuilder& b) {
    svgTextFontsDestroy(b.fonts);
    b.fonts = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Updating

// Returns the size of the user space that the children of `index` are built in, as the build recorded it in the nearest
// viewport.
PrVec2 userExtentOf(const SvgTree* tree, SvgNodeIndex index, PrVec2 fallback) {
    for (SvgNodeIndex at = index; at != kSvgNodeNone; at = tree->nodes.data[at].parent) {
        const SvgNode& node = tree->nodes.data[at];
        if (node.kind == SvgNodeKind_Viewport && node.own)
            return node.viewport.userExtent;
    }
    return fallback;
}

// Returns true if a node is a group whose own values are all that its element's style and attributes decide: a g or an
// a. Its children are its elements' own.
bool isPlainGroup(const SvgNode& node) {
    return node.kind == SvgNodeKind_Group && node.own &&
        (svgIs(node.element, PrDomTag_G) || svgIs(node.element, PrDomTag_A));
}

// Returns true if a node holds the children of its element, each of which is the child's own: a plain group, or the
// viewport of an svg element.
bool holdsChildren(const SvgNode& node) {
    return isPlainGroup(node) || (node.kind == SvgNodeKind_Viewport && node.own);
}

// Builds a node and its descendants again in place. Returns false if the node is the root, which only a build of the
// whole tree builds again.
bool rebuild(SvgBuilder& b, SvgNodeIndex index) {
    if (index == 0)
        return false;
    SvgTree* tree = b.tree;
    const PrDomElement* element = tree->nodes.data[index].element;
    SvgNodeIndex parent = tree->nodes.data[index].parent;
    SvgNodeIndex prev = tree->nodes.data[index].prev;
    b.viewport = userExtentOf(tree, parent, b.build->viewport);
    freeSubtree(tree, index);
    SvgNodeIndex top = builds(element) ? buildElement(b, element, nullptr, parent, false) : kSvgNodeNone;
    if (top != kSvgNodeNone)
        placeAfter(tree, top, prev);
    return true;
}

// Brings the children of a node that holds its element's children up to date with the element's: builds those that lack
// a node, and frees the nodes of those that left or stopped rendering. The nodes of the children are in their elements'
// order, so one pass over both lists suffices.
void reconcile(SvgBuilder& b, SvgNodeIndex index) {
    SvgTree* tree = b.tree;
    b.viewport = userExtentOf(tree, index, b.build->viewport);
    SvgNodeIndex cursor = kSvgNodeNone;
    SvgNodeIndex old = tree->nodes.data[index].first;
    for (const PrDomNode* child = tree->nodes.data[index].element->first; child != nullptr; child = child->next) {
        const PrDomElement* e = domElementFromNode(child);
        if (e == nullptr || !builds(e))
            continue;
        SvgNodeIndex kept = svgTreeNodeFromElement(tree, e);
        if (kept != kSvgNodeNone && tree->nodes.data[kept].parent == index) {
            while (old != kept && old != kSvgNodeNone) {
                SvgNodeIndex next = tree->nodes.data[old].next;
                freeSubtree(tree, old);
                old = next;
            }
            if (old == kSvgNodeNone)
                placeAfter(tree, kept, cursor);
            else
                old = tree->nodes.data[kept].next;
            cursor = kept;
            continue;
        }
        SvgNodeIndex top = buildElement(b, e, nullptr, index, false);
        if (top == kSvgNodeNone)
            continue;
        placeAfter(tree, top, cursor);
        cursor = top;
    }
    while (old != kSvgNodeNone) {
        SvgNodeIndex next = tree->nodes.data[old].next;
        freeSubtree(tree, old);
        old = next;
    }
}

const SvgStrokeStyle* PR_NULLABLE strokeOf(const SvgTree* tree, const SvgPaints& paints) {
    return paints.strokeStyle != kArrayMissing ? &tree->strokes.data[paints.strokeStyle] : nullptr;
}

// A node's own values as drawing reads them: the node, and the stroke and the transform that it refers to, which may be
// copies of what it held before a refresh. Servers are read from the tree.
struct NodeValues {
    const SvgTree* tree;
    const SvgNode* node;
    const SvgStrokeStyle* PR_NULLABLE stroke;
    const SvgTransformStyle* PR_NULLABLE transform;
};

NodeValues valuesOf(const SvgTree* tree, const SvgNode& node) {
    const SvgTransformStyle* transform =
        node.transformStyle != kArrayMissing ? &tree->transforms.data[node.transformStyle] : nullptr;
    return NodeValues{tree, &node, strokeOf(tree, node.paints), transform};
}

bool sameValues(const NodeValues& values, const NodeValues& expected, PrTextBuffer* difference);

// What a refresh did.
enum {
    Refreshed_Same,             // the node's values did not change
    Refreshed_Changed,
    Refreshed_Whole,            // the node is the root, and the whole tree must be built again
};
typedef uint8_t Refreshed;

// Reads a node's own values again from its element, if the node is a plain group or a viewport whose children are built
// as they were. Builds the node again otherwise. The effects that the node held stay until the new values are compared
// with the old.
Refreshed refresh(SvgBuilder& b, SvgNodeIndex index) {
    SvgTree* tree = b.tree;
    const SvgNode& node = tree->nodes.data[index];
    const PrDomElement* element = node.element;
    if (!holdsChildren(node))
        return rebuild(b, index) ? Refreshed_Changed : Refreshed_Whole;
    const CssComputedStyle* style = svgBuildStyle(b, element);
    if (style == nullptr || svgStyleIs(style, PrCssProperty_Display, PrCssKeyword_None) || !conditionsHold(element))
        return rebuild(b, index) ? Refreshed_Changed : Refreshed_Whole;
    // The values as they were, to tell whether the drawing changed. Their stroke and transform are copied, since the
    // slots that held them may be taken again.
    SvgNode before = node;
    NodeValues old = valuesOf(tree, before);
    SvgStrokeStyle beforeStroke = old.stroke != nullptr ? *old.stroke : SvgStrokeStyle{};
    SvgTransformStyle beforeTransform = old.transform != nullptr ? *old.transform : SvgTransformStyle{};
    old.stroke = old.stroke != nullptr ? &beforeStroke : nullptr;
    old.transform = old.transform != nullptr ? &beforeTransform : nullptr;
    svgTreeFreeStroke(tree, before.paints);
    if (before.transformStyle != kArrayMissing)
        tree->freeTransforms.push(before.transformStyle);
    freeReferences(tree, before.firstReference);
    tree->nodes.data[index].firstReference = kArrayMissing;
    SvgNodeIndex savedOwner = b.owner;
    b.owner = index;
    bool root = index == 0;
    readStyle(b, index, style, root ? rootParts(SvgRoot_Box) : StyleParts_All);
    b.owner = savedOwner;
    SvgNode& now = tree->nodes.data[index];
    if (now.kind == SvgNodeKind_Viewport) {
        now.viewport = root ? rootViewportOf(element, style, true) : innerViewportOf(element, style);
        b.viewport = root ? b.build->viewport : userExtentOf(tree, now.parent, b.build->viewport);
        enterViewport(b, &now.viewport);
    }
    // Text inside the node reads its decorations and its user space.
    bool textChanged = now.lines != before.lines || (now.kind == SvgNodeKind_Viewport &&
        !prMemEquals(&now.viewport.userExtent, &before.viewport.userExtent, sizeof(PrVec2)));
    if (textChanged) {
        dropEffects(tree, before.effects);
        return rebuild(b, index) ? Refreshed_Changed : Refreshed_Whole;
    }
    PrTextBuffer unused{};
    bool same = sameValues(valuesOf(tree, now), old, &unused);
    prTextBufferReset(&unused);
    // Frees the effects that the node held before, which the comparison read.
    dropEffects(tree, before.effects);
    return same ? Refreshed_Same : Refreshed_Changed;
}

// Notes the work on what depends on a reference.
void noteDependent(SvgChanges* changes, const SvgReference& reference) {
    if (reference.resource != kSvgResourceNone)
        changes->dirt.push(SvgDirt{SvgWork_Resource, reference.resource});
    else
        changes->dirt.push(SvgDirt{SvgWork_Rebuild, reference.node});
}

// Notes work on a node, unless it is the work last noted.
bool noteWork(SvgChanges* changes, SvgWork work, SvgNodeIndex index) {
    if (changes->dirt.count == 0 || changes->dirt.last().work != work || changes->dirt.last().index != index)
        changes->dirt.push(SvgDirt{work, index});
    return true;
}

// Notes the work that a change at `element`, under the element that built the tree, asks of the element's own node, or
// of the nearest node that is an ancestor's own. Returns true if it noted any.
bool noteLocal(const SvgTree* tree, const PrDomElement* element, SvgChange change, PrStringView name, SvgChanges* changes) {
    // A tree without a root may gain one from any change under its element.
    if (!isLive(tree, 0)) {
        changes->all = true;
        return true;
    }
    SvgNodeIndex found = kSvgNodeNone;
    for (const PrDomElement* at = element; at != nullptr && at->space == PrDomNamespace_Svg && found == kSvgNodeNone;
            at = domElementFromNode(domFlatParent(at))) {
        // An element that never renders where it is may render as a use's instance, as a symbol does, with a node.
        found = svgTreeNodeFromElement(tree, at);
        if (found == kSvgNodeNone && neverRenders(at))
            return false;
    }
    if (found == kSvgNodeNone)
        return false;
    const SvgNode& node = tree->nodes.data[found];
    // An image that loaded, or the layout root that a foreignObject's box is, is read by the build of the node that
    // holds the element, its own or a use element's.
    if (change == SvgChange_Image || change == SvgChange_Box)
        return noteWork(changes, SvgWork_Rebuild, found);
    bool inSwitch = node.parent != kSvgNodeNone && svgIs(tree->nodes.data[node.parent].element, PrDomTag_Switch);
    bool isSwitch = svgIs(node.element, PrDomTag_Switch);
    if (node.element == element) {
        switch (change) {
            case SvgChange_Style:
            case SvgChange_Attribute:
                // A switch renders its first child that may render, so a change of one may change which.
                if (inSwitch)
                    return noteWork(changes, SvgWork_Rebuild, node.parent);
                if (change == SvgChange_Attribute && laysOutText(name))
                    return noteWork(changes, SvgWork_Rebuild, found);
                return noteWork(changes, SvgWork_Refresh, found);
            case SvgChange_Children:
                // A use's content is its shadow tree, whose instance root changed.
                if (node.kind == SvgNodeKind_Text || isSwitch || svgIs(node.element, PrDomTag_Use))
                    return noteWork(changes, SvgWork_Rebuild, found);
                return holdsChildren(node) && noteWork(changes, SvgWork_Children, found);
            case SvgChange_Text:
                return node.kind == SvgNodeKind_Text && noteWork(changes, SvgWork_Rebuild, found);
            case SvgChange_Removing:
                if (inSwitch)
                    return noteWork(changes, SvgWork_Rebuild, node.parent);
                return noteWork(changes, found == 0 ? SvgWork_Rebuild : SvgWork_Remove, found);
        }
        return false;
    }
    // The element is under the node's element but has no node of its own. A text node holds everything inside its
    // element, so the text is built again.
    if (node.kind == SvgNodeKind_Text)
        return noteWork(changes, SvgWork_Rebuild, found);
    // A child that renders nothing may come to render, or change which child of a switch renders. What a child of
    // another element holds is not rendered.
    if (domElementFromNode(domFlatParent(element)) != node.element)
        return false;
    if (isSwitch)
        return noteWork(changes, SvgWork_Rebuild, found);
    if (change != SvgChange_Style && change != SvgChange_Attribute)
        return false;
    return holdsChildren(node) && noteWork(changes, SvgWork_Children, found);
}

// The depth of a node, which orders an update's work so that ancestors are built before their descendants.
uint32_t depthOf(const SvgTree* tree, SvgNodeIndex index) {
    uint32_t depth = 0;
    for (SvgNodeIndex at = tree->nodes.data[index].parent; at != kSvgNodeNone; at = tree->nodes.data[at].parent)
        ++depth;
    return depth;
}

// ---------------------------------------------------------------------------------------------------------------------
// Comparing

bool sameLength(SvgLengthPercentage a, SvgLengthPercentage b) {
    return prMemEquals(&a, &b, sizeof(SvgLengthPercentage));
}

bool sameFloat(float a, float b) {
    return prMemEquals(&a, &b, sizeof(float));
}

bool sameRect(PrRect a, PrRect b) {
    return prMemEquals(&a, &b, sizeof(PrRect));
}

bool sameAffine(const PrAffine2& a, const PrAffine2& b) {
    return prMemEquals(&a, &b, sizeof(PrAffine2));
}

bool sameStroke(const SvgStrokeStyle& a, const SvgStrokeStyle& b) {
    if (!sameLength(a.width, b.width) || !sameLength(a.dashOffset, b.dashOffset) || a.dashCount != b.dashCount ||
            !sameFloat(a.miterLimit, b.miterLimit) || a.join != b.join || a.cap != b.cap)
        return false;
    for (uint32_t i = 0; i < a.dashCount && i < kSvgMaxDashes; ++i) {
        if (!sameLength(a.dashes[i], b.dashes[i]))
            return false;
    }
    return true;
}

bool sameTransformStyle(const SvgTransformStyle& a, const SvgTransformStyle& b) {
    if (a.any != b.any || a.list.count != b.list.count || a.box != b.box || !sameLength(a.origin[0], b.origin[0]) ||
            !sameLength(a.origin[1], b.origin[1]))
        return false;
    for (uint32_t i = 0; i < a.list.count; ++i) {
        const PrLayoutTransformFunction& f = a.list.functions[i];
        const PrLayoutTransformFunction& g = b.list.functions[i];
        if (f.kind != g.kind || !prMemEquals(f.values, g.values, sizeof(f.values)))
            return false;
    }
    return true;
}

bool sameGradient(const SvgGradient& a, const SvgGradient& b) {
    return a.radial == b.radial && a.userSpace == b.userSpace && a.hasFx == b.hasFx && a.hasFy == b.hasFy &&
        a.extend == b.extend && sameAffine(a.transform, b.transform) && sameLength(a.x1, b.x1) &&
        sameLength(a.y1, b.y1) && sameLength(a.x2, b.x2) && sameLength(a.y2, b.y2) && sameLength(a.cx, b.cx) &&
        sameLength(a.cy, b.cy) && sameLength(a.r, b.r) && sameLength(a.fx, b.fx) && sameLength(a.fy, b.fy) &&
        sameLength(a.fr, b.fr) && a.space == b.space && a.stopCount == b.stopCount &&
        (a.stopCount == 0 || prMemEquals(a.stops, b.stops, a.stopCount * sizeof(PrDrawColorStop)));
}

bool sameMarker(const SvgMarker& a, const SvgMarker& b) {
    return sameLength(a.width, b.width) && sameLength(a.height, b.height) && a.hasViewBox == b.hasViewBox &&
        (!a.hasViewBox || sameRect(a.viewBox, b.viewBox)) && a.aspect.x == b.aspect.x &&
        a.aspect.y == b.aspect.y && a.aspect.slice == b.aspect.slice && a.clips == b.clips &&
        a.strokeUnits == b.strokeUnits && a.orient == b.orient && sameFloat(a.angle, b.angle) &&
        sameLength(a.refX, b.refX) && sameLength(a.refY, b.refY) &&
        (a.content == kSvgNodeNone) == (b.content == kSvgNodeNone);
}

bool samePattern(const SvgPattern& a, const SvgPattern& b) {
    return a.userSpace == b.userSpace && a.contentBoundingBox == b.contentBoundingBox && a.hasViewBox == b.hasViewBox &&
        (!a.hasViewBox || sameRect(a.viewBox, b.viewBox)) && a.aspect.x == b.aspect.x &&
        a.aspect.y == b.aspect.y && a.aspect.slice == b.aspect.slice && sameLength(a.x, b.x) &&
        sameLength(a.y, b.y) && sameLength(a.width, b.width) && sameLength(a.height, b.height) &&
        sameAffine(a.transform, b.transform) && (a.content == kSvgNodeNone) == (b.content == kSvgNodeNone);
}

bool sameMask(const SvgMask& a, const SvgMask& b) {
    return a.userSpace == b.userSpace && a.contentBoundingBox == b.contentBoundingBox && sameLength(a.x, b.x) &&
        sameLength(a.y, b.y) && sameLength(a.width, b.width) && sameLength(a.height, b.height) &&
        a.mode == b.mode && a.space == b.space && (a.content == kSvgNodeNone) == (b.content == kSvgNodeNone);
}

// Returns true if two references to resources, each of its own tree, are to the same element and resource. The content
// of a resource is compared apart from what references it (svgTreeSame).
bool sameResource(const SvgTree* tree, SvgResourceIndex a, const SvgTree* expectedTree, SvgResourceIndex b) {
    if ((a == kSvgResourceNone) != (b == kSvgResourceNone))
        return false;
    if (a == kSvgResourceNone || (tree == expectedTree && a == b))
        return true;
    const SvgResource& x = tree->resources.data[a];
    const SvgResource& y = expectedTree->resources.data[b];
    if (x.element != y.element || x.kind != y.kind)
        return false;
    switch (x.kind) {
        case SvgResourceKind_Marker: return sameMarker(x.marker, y.marker);
        case SvgResourceKind_Pattern: return samePattern(x.pattern, y.pattern);
        case SvgResourceKind_ClipPath:
            return x.clipPath.boundingBox == y.clipPath.boundingBox &&
                sameAffine(x.clipPath.transform, y.clipPath.transform) &&
                (x.clipPath.content == kSvgNodeNone) == (y.clipPath.content == kSvgNodeNone);
        case SvgResourceKind_Mask: return sameMask(x.mask, y.mask);
        case SvgResourceKind_Filter: return svgFilterSame(x.filter, y.filter);
        default: return sameGradient(x.gradient, y.gradient);
    }
}

// Returns true if two gradients of mask layers are the same.
bool sameGradientLayer(const PrLayoutGradient& a, const PrLayoutGradient& b) {
    return a.kind == b.kind && a.space == b.space && a.hueMethod == b.hueMethod && a.repeating == b.repeating &&
        a.corner == b.corner && sameFloat(a.angle, b.angle) && a.shape == b.shape && a.extent == b.extent &&
        a.radiusX.bits == b.radiusX.bits && a.radiusY.bits == b.radiusY.bits && a.centerX.bits == b.centerX.bits &&
        a.centerY.bits == b.centerY.bits && a.stopCount == b.stopCount &&
        (a.stopCount == 0 || prMemEquals(a.stops, b.stops, a.stopCount * sizeof(PrLayoutColorStop)));
}

// Returns true if two filter functions are the same.
bool sameFunction(const CssFilterFunction& x, const CssFilterFunction& y) {
    const PrLayoutFilter& a = x.filter;
    const PrLayoutFilter& b = y.filter;
    return a.kind == b.kind && sameFloat(a.amount, b.amount) && prMemEquals(&a.offset, &b.offset, sizeof(PrVec2)) &&
        sameFloat(a.sigma, b.sigma) && prMemEquals(&a.color, &b.color, sizeof(PrColor16));
}

// Returns true if two nodes' effects are the same, each with the resources of its own tree.
bool sameEffects(const SvgTree* tree, uint32_t a, const SvgTree* expectedTree, uint32_t b) {
    if ((a == kArrayMissing) != (b == kArrayMissing))
        return false;
    if (a == kArrayMissing)
        return true;
    const SvgEffects& x = tree->effects.data[a];
    const SvgEffects& y = expectedTree->effects.data[b];
    if (x.values != y.values || x.clip != y.clip || x.layers.count != y.layers.count ||
            x.filters.count != y.filters.count || x.blend != y.blend || x.composite != y.composite ||
            x.isolated != y.isolated)
        return false;
    for (uint32_t i = 0; i < x.filters.count; ++i) {
        const SvgFilterItem& p = x.filters.data[i];
        const SvgFilterItem& q = y.filters.data[i];
        if (!sameFunction(p.function, q.function) || !sameResource(tree, p.filter, expectedTree, q.filter))
            return false;
    }
    if (x.clip == SvgClipKind_Url && !sameResource(tree, x.clipPath, expectedTree, y.clipPath))
        return false;
    for (uint32_t i = 0; i < x.layers.count; ++i) {
        const SvgMaskLayer& p = x.layers.data[i];
        const SvgMaskLayer& q = y.layers.data[i];
        if (p.kind != q.kind || p.picture.raster != q.picture.raster || p.picture.svg != q.picture.svg)
            return false;
        if (p.kind == SvgMaskLayerKind_Gradient && !sameGradientLayer(p.gradient, q.gradient))
            return false;
        if (p.kind == SvgMaskLayerKind_Mask && !sameResource(tree, p.mask, expectedTree, q.mask))
            return false;
    }
    return true;
}

bool samePaint(const SvgTree* tree, const SvgPaint& a, const SvgTree* expectedTree, const SvgPaint& b) {
    return a.kind == b.kind && a.fallback == b.fallback && prMemEquals(&a.color, &b.color, sizeof(PrColor)) &&
        sameResource(tree, a.server, expectedTree, b.server);
}

// Returns true if two nodes' vertex markers are the same, each with the resources of its own tree.
bool sameMarkers(const SvgTree* tree, uint32_t a, const SvgTree* expectedTree, uint32_t b) {
    if ((a == kArrayMissing) != (b == kArrayMissing))
        return false;
    if (a == kArrayMissing)
        return true;
    const SvgMarkers& x = tree->markers.data[a];
    const SvgMarkers& y = expectedTree->markers.data[b];
    return sameResource(tree, x.start, expectedTree, y.start) && sameResource(tree, x.mid, expectedTree, y.mid) &&
        sameResource(tree, x.end, expectedTree, y.end) && sameLength(x.strokeWidth, y.strokeWidth) &&
        x.vertices.count == y.vertices.count &&
        prMemEquals(x.vertices.data, y.vertices.data, x.vertices.count * sizeof(SvgVertex));
}

bool sameLimits(const SvgSizeLimits& a, const SvgSizeLimits& b) {
    for (uint32_t i = 0; i < 2; ++i) {
        if (!sameLength(a.min[i], b.min[i]) || !sameLength(a.max[i], b.max[i]) || a.bounded[i] != b.bounded[i])
            return false;
    }
    return true;
}

bool sameViewport(const SvgViewportShape& a, const SvgViewportShape& b) {
    return sameLength(a.x, b.x) && sameLength(a.y, b.y) && sameLength(a.width, b.width) &&
        sameLength(a.height, b.height) && a.hasViewBox == b.hasViewBox &&
        (!a.hasViewBox || sameRect(a.viewBox, b.viewBox)) && a.aspect.x == b.aspect.x &&
        a.aspect.y == b.aspect.y && a.aspect.slice == b.aspect.slice && a.clips == b.clips &&
        a.fills == b.fills && prMemEquals(&a.userExtent, &b.userExtent, sizeof(PrVec2)) &&
        a.hasRefX == b.hasRefX && a.hasRefY == b.hasRefY && sameLength(a.refX, b.refX) && sameLength(a.refY, b.refY);
}

bool sameImage(const SvgImageShape& a, const SvgImageShape& b) {
    return sameLength(a.x, b.x) && sameLength(a.y, b.y) && sameLength(a.width, b.width) &&
        sameLength(a.height, b.height) && a.widthAuto == b.widthAuto && a.heightAuto == b.heightAuto &&
        sameLimits(a.limits, b.limits) && a.clips == b.clips && a.aspect.x == b.aspect.x && a.aspect.y == b.aspect.y &&
        a.aspect.slice == b.aspect.slice && a.fit == b.fit && sameLength(a.position[0], b.position[0]) &&
        sameLength(a.position[1], b.position[1]) && a.sampling == b.sampling &&
        a.picture.raster == b.picture.raster && a.picture.svg == b.picture.svg;
}

// Returns true if two paints paint the same, with the strokes given, and each with the servers of its own tree.
bool samePaints(const SvgTree* tree, const SvgPaints& paints, const SvgStrokeStyle* PR_NULLABLE stroke,
        const SvgTree* expectedTree, const SvgPaints& expected, const SvgStrokeStyle* PR_NULLABLE expectedStroke) {
    return samePaint(tree, paints.fill, expectedTree, expected.fill) &&
        samePaint(tree, paints.stroke, expectedTree, expected.stroke) &&
        sameFloat(paints.fillOpacity, expected.fillOpacity) &&
        sameFloat(paints.strokeOpacity, expected.strokeOpacity) && paints.crisp == expected.crisp &&
        paints.nonScaling == expected.nonScaling && sameFloat(paints.pathLength, expected.pathLength) &&
        prMemEquals(paints.order, expected.order, sizeof(paints.order)) &&
        (stroke == nullptr) == (expectedStroke == nullptr) &&
        (stroke == nullptr || sameStroke(*stroke, *expectedStroke));
}

// Returns true if two nodes draw the same, without their children.
bool sameValues(const NodeValues& values, const NodeValues& expectedValues, PrTextBuffer* difference) {
    const SvgTree* tree = values.tree;
    const SvgTree* expectedTree = expectedValues.tree;
    const SvgNode& node = *values.node;
    const SvgNode& expected = *expectedValues.node;
    if (node.kind != expected.kind) {
        prTextBufferAppend(difference, "kinds differ");
        return false;
    }
    if (node.visible != expected.visible || node.fillRule != expected.fillRule || node.clipRule != expected.clipRule ||
            node.lines != expected.lines || !sameFloat(node.opacity, expected.opacity) ||
            node.context != expected.context || node.linear != expected.linear ||
            node.pointerEvents != expected.pointerEvents || node.vectorEffects != expected.vectorEffects) {
        prTextBufferAppend(difference, "visibility, fill or clip rule, decorations, opacity, context, color-interpolation, "
            "pointer-events or vector effects differ");
        return false;
    }
    if (!sameEffects(tree, node.effects, expectedTree, expected.effects)) {
        prTextBufferAppend(difference, "clip paths, masks, filters or blending differ");
        return false;
    }
    const SvgTransformStyle* transform = values.transform;
    const SvgTransformStyle* expectedTransform = expectedValues.transform;
    bool sameTransform =
        sameAffine(node.transform, expected.transform) && (transform == nullptr) == (expectedTransform == nullptr) &&
        (transform == nullptr || sameTransformStyle(*transform, *expectedTransform));
    if (!sameTransform) {
        prTextBufferAppend(difference, "transforms differ");
        return false;
    }
    if (!samePaints(tree, node.paints, values.stroke, expectedTree, expected.paints, expectedValues.stroke)) {
        prTextBufferAppend(difference, "paints differ");
        return false;
    }
    if (!sameMarkers(tree, node.markers, expectedTree, expected.markers)) {
        prTextBufferAppend(difference, "markers differ");
        return false;
    }
    bool same = true;
    switch (node.kind) {
        case SvgNodeKind_Rect: {
            const SvgRectShape& a = node.rect;
            const SvgRectShape& b = expected.rect;
            same = sameLength(a.x, b.x) && sameLength(a.y, b.y) && sameLength(a.width, b.width) &&
                sameLength(a.height, b.height) && sameLength(a.rx, b.rx) && sameLength(a.ry, b.ry) &&
                a.rxAuto == b.rxAuto && a.ryAuto == b.ryAuto && sameLimits(a.limits, b.limits);
            break;
        }
        case SvgNodeKind_Ellipse: {
            const SvgEllipseShape& a = node.ellipse;
            const SvgEllipseShape& b = expected.ellipse;
            same = sameLength(a.cx, b.cx) && sameLength(a.cy, b.cy) && sameLength(a.rx, b.rx) && sameLength(a.ry,
                b.ry) && a.rxAuto == b.rxAuto && a.ryAuto == b.ryAuto && a.circle == b.circle;
            break;
        }
        case SvgNodeKind_Line: {
            const SvgLineShape& a = node.line;
            const SvgLineShape& b = expected.line;
            same = sameLength(a.x1, b.x1) && sameLength(a.y1, b.y1) && sameLength(a.x2, b.x2) && sameLength(a.y2, b.y2);
            break;
        }
        case SvgNodeKind_Path:
            same = sameRect(node.path.bounds, expected.path.bounds) && svgPathSame(node.path.path, expected.path.path);
            break;
        case SvgNodeKind_Viewport:
            same = sameViewport(node.viewport, expected.viewport);
            break;
        case SvgNodeKind_Image:
            same = sameImage(node.image, expected.image);
            break;
        case SvgNodeKind_ForeignObject: {
            const SvgForeignShape& a = node.foreign;
            const SvgForeignShape& b = expected.foreign;
            same = sameLength(a.x, b.x) && sameLength(a.y, b.y) && sameLength(a.width, b.width) &&
                sameLength(a.height, b.height) && sameLimits(a.limits, b.limits) && a.clips == b.clips &&
                a.root == b.root && prMemEquals(&a.extent, &b.extent, sizeof(PrVec2));
            break;
        }
        case SvgNodeKind_Text:
            return svgTextSame(tree, node.text, expectedTree, expected.text, difference);
        default:
            break;
    }
    if (!same)
        prTextBufferAppend(difference, "geometry differs");
    return same;
}

// Appends a description of an element: its name, and its ID if it has one.
void describe(PrTextBuffer* out, const PrDomElement* element) {
    prTextBufferAppend(out, "<");
    prTextBufferAppend(out, domElementName(element));
    PrStringView id = domAttributeValue(element, "id");
    if (id.data != nullptr) {
        prTextBufferAppend(out, " id=");
        prTextBufferAppend(out, id);
    }
    prTextBufferAppend(out, ">");
}

// Returns true if a node of the tree blends with what lies beneath it, or applies a filter that reads it. Freed effects
// are empty, and read nothing.
bool readsBackdrop(const SvgTree* tree) {
    for (uint32_t i = 0; i < tree->effects.count; ++i) {
        const SvgEffects& effects = tree->effects.data[i];
        if (effects.blend != PrDrawBlendMode_Normal || effects.composite != PrDrawComposite_SourceOver)
            return true;
        for (uint32_t k = 0; k < effects.filters.count; ++k) {
            SvgResourceIndex filter = effects.filters.data[k].filter;
            if (filter != kSvgResourceNone && svgFilterReadsBackdrop(tree->resources.data[filter].filter))
                return true;
        }
    }
    return false;
}

// Returns true if the node `root` of `tree` and its descendants draw as `expectedRoot` of `expected` and its
// descendants do. Otherwise writes the first difference to `difference`.
bool sameSubtree(const SvgTree* tree, SvgNodeIndex root, const SvgTree* expected, SvgNodeIndex expectedRoot,
        PrTextBuffer* difference) {
    RawArray<SvgNodeIndex> stack;
    stack.push(root);
    stack.push(expectedRoot);
    bool same = true;
    while (same && stack.count != 0) {
        SvgNodeIndex e = stack.last();
        stack.pop();
        SvgNodeIndex a = stack.last();
        stack.pop();
        const SvgNode& node = tree->nodes.data[a];
        const SvgNode& other = expected->nodes.data[e];
        PrTextBuffer what{};
        // The nodes of a use element's shadow tree are of copies, made again by each build.
        same = node.own == other.own &&
            (node.own ? node.element == other.element : node.element->tag == other.element->tag);
        if (!same)
            prTextBufferAppend(&what, "the nodes are of different elements");
        same = same && sameValues(valuesOf(tree, node), valuesOf(expected, other), &what);
        SvgNodeIndex x = node.first;
        SvgNodeIndex y = other.first;
        for (; same && x != kSvgNodeNone && y != kSvgNodeNone;
                x = tree->nodes.data[x].next, y = expected->nodes.data[y].next) {
            stack.push(x);
            stack.push(y);
        }
        if (same && (x != kSvgNodeNone || y != kSvgNodeNone)) {
            prTextBufferAppend(&what, "the nodes have different numbers of children");
            same = false;
        }
        if (!same) {
            describe(difference, node.element);
            prTextBufferAppend(difference, ": ");
            prTextBufferAppend(difference, prTextBufferView(&what));
        }
        prTextBufferReset(&what);
    }
    stack.reset();
    return same;
}

} // namespace

uint32_t svgTreeBuild(SvgTree* tree, const PrDomElement* element, SvgRoot root, const SvgBuild& build) {
    svgTreeReset(tree);
    ++tree->update;
    SvgBuilder b{
        .tree = tree,
        .build = &build,
        .document = element->document,
        .viewport = build.viewport,
        .owner = kSvgNodeNone,
        .ownerResource = kSvgResourceNone,
    };
    const CssComputedStyle* style = svgBuildStyle(b, element);
    if (style != nullptr && !svgStyleIs(style, PrCssProperty_Display, PrCssKeyword_None)) {
        if (root == SvgRoot_InPlace) {
            // The element is drawn as a child of a group that stands for its parent's user space.
            SvgNodeIndex group = addNode(b, SvgNodeKind_Group, element, style, kSvgNodeNone, StyleParts_None, false);
            const PrDomElement* parent = domElementFromNode(element->parent);
            const CssComputedStyle* parentStyle = parent != nullptr ? svgBuildStyle(b, parent) : nullptr;
            buildElement(b, element, parentStyle, group, false);
        } else {
            buildRoot(b, element, style, root);
        }
    }
    finish(b);
    tree->sweptResources = tree->resources.count - tree->freeResources.count;
    tree->readsBackdrop = readsBackdrop(tree);
    return b.built;
}

void svgTreeReset(SvgTree* tree) {
    for (uint32_t i = 0; i < tree->nodes.count; ++i) {
        SvgNode& node = tree->nodes.data[i];
        if (node.kind == SvgNodeKind_Path)
            prPathDestroy(node.path.path);
        else if (node.kind == SvgNodeKind_Text)
            svgTextDestroy(tree, node.text);
        else if (node.kind == SvgNodeKind_Image)
            releasePicture(node.image.picture);
    }
    tree->nodes.reset();
    tree->freeNodes.reset();
    tree->elements.reset();
    tree->strokes.reset();
    tree->freeStrokes.reset();
    tree->transforms.reset();
    tree->freeTransforms.reset();
    for (uint32_t i = 0; i < tree->markers.count; ++i)
        tree->markers.data[i].vertices.reset();
    tree->markers.reset();
    tree->freeMarkers.reset();
    for (uint32_t i = 0; i < tree->effects.count; ++i)
        resetEffects(tree->effects.data[i]);
    tree->effects.reset();
    tree->freeEffects.reset();
    for (uint32_t i = 0; i < tree->resources.count; ++i) {
        SvgResource& resource = tree->resources.data[i];
        if (resource.element != nullptr)
            freeResourceData(resource);
        domNodeRelease(const_cast<PrDomElement*>(resource.element));
    }
    tree->resources.reset();
    tree->freeResources.reset();
    for (uint32_t i = 0; i < tree->references.count; ++i) {
        SvgReference& reference = tree->references.data[i];
        prRawStringReset(&reference.id);
        prRawStringReset(&reference.document);
        domNodeRelease(const_cast<PrDomElement*>(reference.target));
        domNodeRelease(const_cast<PrDomNode*>(reference.scope));
    }
    tree->references.reset();
    tree->freeReferences.reset();
    for (uint32_t i = 0; i < tree->cascades.count; ++i) {
        svgCascadeReset(tree->cascades.data[i]);
        prMemoryFree(tree->cascades.data[i]);
    }
    tree->cascades.reset();
    tree->watched = 0;
    tree->sweptResources = 0;
    tree->fontFamilies.reset();
    tree->readsBackdrop = false;
}

void svgTreeDetach(SvgTree* tree) {
    for (uint32_t i = 0; i < tree->nodes.count; ++i) {
        SvgNode& node = tree->nodes.data[i];
        node.firstReference = kArrayMissing;
    }
    for (uint32_t i = 0; i < tree->resources.count; ++i) {
        SvgResource& resource = tree->resources.data[i];
        domNodeRelease(const_cast<PrDomElement*>(resource.element));
        resource.element = nullptr;
        resource.firstReference = kArrayMissing;
    }
    for (uint32_t i = 0; i < tree->references.count; ++i) {
        SvgReference& reference = tree->references.data[i];
        prRawStringReset(&reference.id);
        prRawStringReset(&reference.document);
        domNodeRelease(const_cast<PrDomElement*>(reference.target));
        domNodeRelease(const_cast<PrDomNode*>(reference.scope));
    }
    tree->references.reset();
    tree->freeReferences.reset();
    tree->elements.reset();
    tree->watched = 0;
}

SvgNodeIndex svgTreeNodeFromElement(const SvgTree* tree, const PrDomElement* element) {
    uint32_t at =
        tree->elements.find(hashOf(element), [&](uint32_t index) { return tree->nodes.data[index].element == element; });
    return at != kHashMissing ? at : kSvgNodeNone;
}

bool svgReadShape(const PrDomElement* element, const CssComputedStyle* style, SvgNode* node,
        RawArray<PrPathDataCommand>* commands) {
    const PrDomElement* e = element;
    switch (e->space == PrDomNamespace_Svg ? e->tag : PrDomTag_Count) {
        case PrDomTag_Rect: {
            node->kind = SvgNodeKind_Rect;
            SvgRectShape& r = node->rect;
            r.x = svgStyleLength(style, PrCssProperty_X);
            r.y = svgStyleLength(style, PrCssProperty_Y);
            r.width = svgStyleLength(style, PrCssProperty_Width);
            r.height = svgStyleLength(style, PrCssProperty_Height);
            r.rx = svgStyleLength(style, PrCssProperty_Rx);
            r.ry = svgStyleLength(style, PrCssProperty_Ry);
            r.rxAuto = svgStyleIs(style, PrCssProperty_Rx, PrCssKeyword_Auto);
            r.ryAuto = svgStyleIs(style, PrCssProperty_Ry, PrCssKeyword_Auto);
            r.limits = svgStyleSizeLimits(style);
            return true;
        }
        case PrDomTag_Circle:
        case PrDomTag_Ellipse: {
            node->kind = SvgNodeKind_Ellipse;
            SvgEllipseShape& s = node->ellipse;
            s.cx = svgStyleLength(style, PrCssProperty_Cx);
            s.cy = svgStyleLength(style, PrCssProperty_Cy);
            s.circle = e->tag == PrDomTag_Circle;
            if (s.circle) {
                s.rx = s.ry = svgStyleLength(style, PrCssProperty_R);
            } else {
                s.rx = svgStyleLength(style, PrCssProperty_Rx);
                s.ry = svgStyleLength(style, PrCssProperty_Ry);
                s.rxAuto = svgStyleIs(style, PrCssProperty_Rx, PrCssKeyword_Auto);
                s.ryAuto = svgStyleIs(style, PrCssProperty_Ry, PrCssKeyword_Auto);
            }
            return true;
        }
        case PrDomTag_Line: {
            node->kind = SvgNodeKind_Line;
            SvgLineShape& l = node->line;
            l.x1 = svgAttributeLength(e, "x1", SvgLengthPercentage{}, style);
            l.y1 = svgAttributeLength(e, "y1", SvgLengthPercentage{}, style);
            l.x2 = svgAttributeLength(e, "x2", SvgLengthPercentage{}, style);
            l.y2 = svgAttributeLength(e, "y2", SvgLengthPercentage{}, style);
            return true;
        }
        case PrDomTag_Path:
            node->kind = SvgNodeKind_Path;
            svgStylePathCommands(style, commands);
            return commands->count != 0;
        case PrDomTag_Polyline:
        case PrDomTag_Polygon: {
            node->kind = SvgNodeKind_Path;
            // The points up to the first number that does not parse, without a lone coordinate at the end
            // (SVG2/shapes.html#PolylineElement).
            RawArray<float> numbers;
            svgParseNumbers(svgAttribute(e, "points"), &numbers);
            for (uint32_t i = 0; i + 1 < numbers.count; i += 2)
                commands->push(PrPathDataCommand{
                    .letter = i == 0 ? 'M' : 'L',
                    .arguments = {numbers.data[i], numbers.data[i + 1]},
                });
            if (e->tag == PrDomTag_Polygon && numbers.count >= 2)
                commands->push(PrPathDataCommand{.letter = 'Z'});
            numbers.reset();
            return commands->count != 0;
        }
        default:
            return false;
    }
}

bool svgTreeNoteChange(const SvgTree* tree, const PrDomElement* element, SvgChange change, PrStringView name,
        bool under, SvgChanges* changes) {
    if (changes->all)
        return true;
    bool noted = false;
    for (const PrDomElement* at = element; at != nullptr && at->space == PrDomNamespace_Svg;
            at = domElementFromNode(domFlatParent(at))) {
        // The dependents that read a subtree that the change is in.
        if ((tree->watched & watchBit(at)) == 0)
            continue;
        for (uint32_t i = 0; i < tree->references.count; ++i) {
            const SvgReference& reference = tree->references.data[i];
            if (reference.target == at && reference.content) {
                noteDependent(changes, reference);
                noted = true;
            }
        }
    }
    return (under && noteLocal(tree, element, change, name, changes)) || noted;
}

bool svgTreeNoteFontFamily(const SvgTree* tree, uint32_t familyHash, SvgChanges* changes) {
    if (tree->fontFamilies.find(familyHash) == kArrayMissing)
        return false;
    changes->all = true;
    return true;
}

bool svgTreeNoteIds(const SvgTree* tree, SvgChanges* changes) {
    if (tree->references.count == tree->freeReferences.count)
        return false;
    changes->ids = true;
    return true;
}

SvgUpdate svgTreeUpdate(SvgTree* tree, const PrDomElement* element, SvgChanges* changes, const SvgBuild& build) {
    SvgUpdate result{};
    bool whole = changes->all || !isLive(tree, 0);
    // An ID that names another element than it did builds again what reads it.
    if (!whole && changes->ids) {
        for (uint32_t i = 0; i < tree->references.count; ++i) {
            const SvgReference& reference = tree->references.data[i];
            if ((reference.node != kSvgNodeNone || reference.resource != kSvgResourceNone) &&
                    retargeted(reference, element->document))
                noteDependent(changes, reference);
        }
    }
    // Removals first, then the servers, then the nodes from the root down, so that work on a node that a rebuilt
    // ancestor built again is skipped.
    struct Ordered {
        uint32_t rank;
        SvgDirt dirt;
    };
    RawArray<Ordered> order;
    for (uint32_t i = 0; !whole && i < changes->dirt.count; ++i) {
        SvgDirt dirt = changes->dirt.data[i];
        bool node = dirt.work != SvgWork_Resource;
        if (node && !isLive(tree, dirt.index))
            continue;
        if (node && dirt.index == 0 && (dirt.work == SvgWork_Rebuild || dirt.work == SvgWork_Remove))
            whole = true;
        uint32_t rank =
            dirt.work == SvgWork_Remove   ? 0 : dirt.work == SvgWork_Resource ? 1 : 2 + depthOf(tree, dirt.index);
        order.push(Ordered{rank, dirt});
    }
    sortItems(order.span(), [](const Ordered& a, const Ordered& b) { return a.rank < b.rank; });
    ++tree->update;
    SvgBuilder b{
        .tree = tree,
        .build = &build,
        .document = element->document,
        .viewport = build.viewport,
        .owner = kSvgNodeNone,
        .ownerResource = kSvgResourceNone,
    };
    for (uint32_t i = 0; !whole && i < order.count; ++i) {
        SvgDirt dirt = order.data[i].dirt;
        if (dirt.work == SvgWork_Resource) {
            if (tree->resources.data[dirt.index].element != nullptr) {
                rebuildResource(b, dirt.index);
                result.changed = true;
            }
            continue;
        }
        // A node that this update built is up to date, as is a slot that it freed.
        if (!isLive(tree, dirt.index) || tree->nodes.data[dirt.index].built == tree->update)
            continue;
        switch (dirt.work) {
            case SvgWork_Remove:
                freeSubtree(tree, dirt.index);
                result.changed = true;
                break;
            case SvgWork_Refresh: {
                Refreshed refreshed = refresh(b, dirt.index);
                whole = refreshed == Refreshed_Whole;
                result.changed = result.changed || refreshed == Refreshed_Changed;
                break;
            }
            case SvgWork_Children:
                reconcile(b, dirt.index);
                result.changed = true;
                break;
            case SvgWork_Rebuild:
                whole = !rebuild(b, dirt.index);
                result.changed = true;
                break;
            default:
                break;
        }
    }
    order.reset();
    finish(b);
    svgChangesReset(changes);
    if (whole) {
        result.changed = true;
        result.built = svgTreeBuild(tree, element, SvgRoot_Box, build);
        return result;
    }
    sweepResources(tree);
    tree->readsBackdrop = readsBackdrop(tree);
    result.built = b.built;
    return result;
}

void svgChangesReset(SvgChanges* changes) {
    changes->all = false;
    changes->ids = false;
    changes->dirt.reset();
}

SvgResourceIndex svgTreeResourceFor(SvgTree* tree, const PrDomElement* from, PrStringView url, SvgResourceKind kind,
        const SvgBuild& build) {
    SvgBuilder b{
        .tree = tree,
        .build = &build,
        .document = from->document,
        .viewport = build.viewport,
        .owner = kSvgNodeNone,
        .ownerResource = kSvgResourceNone,
    };
    const PrDomElement* target = svgBuildReference(b, from, url, false);
    SvgResourceIndex index = target != nullptr ? resourceOf(b, target, kind) : kSvgResourceNone;
    finish(b);
    return index;
}

bool svgTreeUpdateResources(SvgTree* tree, const PrDomNode* document, SvgChanges* changes, const SvgBuild& build) {
    bool changed = false;
    SvgBuilder b{
        .tree = tree,
        .build = &build,
        .document = document,
        .viewport = build.viewport,
        .owner = kSvgNodeNone,
        .ownerResource = kSvgResourceNone,
    };
    ++tree->update;
    if (changes->all) {
        for (uint32_t i = 0; i < tree->resources.count; ++i) {
            if (tree->resources.data[i].element != nullptr)
                changes->dirt.push(SvgDirt{SvgWork_Resource, i});
        }
    } else if (changes->ids) {
        for (uint32_t i = 0; i < tree->references.count; ++i) {
            const SvgReference& reference = tree->references.data[i];
            if ((reference.node != kSvgNodeNone || reference.resource != kSvgResourceNone) && retargeted(reference, document))
                noteDependent(changes, reference);
        }
    }
    // A resource is built again once, however many changes reach it.
    for (uint32_t i = 0; i < changes->dirt.count; ++i) {
        SvgDirt dirt = changes->dirt.data[i];
        if (dirt.work != SvgWork_Resource || tree->resources.data[dirt.index].element == nullptr)
            continue;
        bool seen = false;
        for (uint32_t k = 0; k < i && !seen; ++k)
            seen = changes->dirt.data[k].work == SvgWork_Resource && changes->dirt.data[k].index == dirt.index;
        if (seen)
            continue;
        rebuildResource(b, dirt.index);
        changed = true;
    }
    finish(b);
    svgChangesReset(changes);
    return changed;
}

void svgTreeFreeResource(SvgTree* tree, SvgResourceIndex index) {
    freeResource(tree, index);
}

bool svgTreeSame(const SvgTree* tree, const SvgTree* expected, PrTextBuffer* difference) {
    if (isLive(tree, 0) != isLive(expected, 0)) {
        prTextBufferAppend(difference, "one tree has a root and the other has none");
        return false;
    }
    if (!isLive(tree, 0))
        return true;
    if (!sameSubtree(tree, 0, expected, 0, difference))
        return false;
    // The content of each marker and pattern that the expected tree drew, which the tree has a resource for if it draws
    // the same.
    for (uint32_t i = 0; i < expected->resources.count; ++i) {
        const SvgResource& wanted = expected->resources.data[i];
        SvgNodeIndex wantedContent = contentOf(wanted);
        if (wanted.element == nullptr || wantedContent == kSvgNodeNone)
            continue;
        SvgNodeIndex content = kSvgNodeNone;
        for (uint32_t k = 0; k < tree->resources.count && content == kSvgNodeNone; ++k) {
            if (tree->resources.data[k].element == wanted.element)
                content = contentOf(tree->resources.data[k]);
        }
        if (content == kSvgNodeNone) {
            describe(difference, wanted.element);
            prTextBufferAppend(difference, ": the resource has no content");
            return false;
        }
        if (!sameSubtree(tree, content, expected, wantedContent, difference))
            return false;
    }
    return true;
}

SvgNodeIndex svgBuildNode(SvgBuilder& b, const SvgNode& node, SvgNodeIndex parent, bool own) {
    SvgTree* tree = b.tree;
    SvgNode added = node;
    added.parent = parent;
    added.first = added.last = added.next = added.prev = kSvgNodeNone;
    added.own = own;
    added.built = tree->update;
    added.firstReference = kArrayMissing;
    SvgNodeIndex index = take(tree->nodes, tree->freeNodes, added);
    if (parent != kSvgNodeNone) {
        SvgNode& p = tree->nodes.data[parent];
        tree->nodes.data[index].prev = p.last;
        if (p.last == kSvgNodeNone)
            p.first = index;
        else
            tree->nodes.data[p.last].next = index;
        p.last = index;
    }
    if (own) {
        tree->elements.insert(hashOf(node.element), index);
        b.owner = index;
    }
    ++b.built;
    return index;
}

SvgPaints svgBuildPaints(SvgBuilder& b, const CssComputedStyle* style, const PrDomElement* element) {
    const CssColorEnvironment& environment = b.build->environment;
    SvgPaints paints{};
    paints.fill = resolvePaint(b, svgStylePaint(style, PrCssProperty_Fill, environment), element);
    paints.stroke = resolvePaint(b, svgStylePaint(style, PrCssProperty_Stroke, environment), element);
    paints.fillOpacity = prF32Clamp(svgStyleNumber(style, PrCssProperty_FillOpacity, 1), 0, 1);
    paints.strokeOpacity = prF32Clamp(svgStyleNumber(style, PrCssProperty_StrokeOpacity, 1), 0, 1);
    paints.strokeStyle = kArrayMissing;
    if (paints.stroke.kind != SvgPaintKind_None || svgPointerReadsStroke(svgStylePointerEvents(style)))
        paints.strokeStyle = take(b.tree->strokes, b.tree->freeStrokes, svgStyleStroke(style));
    PrCssKeyword rendering = svgStyleKeyword(style, PrCssProperty_ShapeRendering);
    paints.crisp = rendering == PrCssKeyword_Crispedges || rendering == PrCssKeyword_Optimizespeed;
    paints.nonScaling = svgStyleNonScalingStroke(style);
    if (svgStyleIs(style, PrCssProperty_PathLength, PrCssKeyword_None))
        paints.pathLength = -1;
    else
        paints.pathLength = prF32Max(svgStyleLength(style, PrCssProperty_PathLength).px, 0);
    svgStylePaintOrder(style, paints.order);
    return paints;
}

void svgBuildEffects(SvgBuilder& b, SvgNodeIndex index, const CssComputedStyle* style) {
    readEffects(b, index, style, StyleParts_Effects);
}

SvgNodeIndex svgBuildContentGroup(SvgBuilder& b, const PrDomElement* e) {
    return addContentGroup(b, e);
}

SvgNodeIndex svgBuildElement(SvgBuilder& b, const PrDomElement* e, SvgNodeIndex parent) {
    return builds(e) ? buildElement(b, e, nullptr, parent, false) : kSvgNodeNone;
}

void svgTreeFreeStroke(SvgTree* tree, const SvgPaints& paints) {
    if (paints.strokeStyle != kArrayMissing)
        tree->freeStrokes.push(paints.strokeStyle);
}

bool svgPaintsSame(const SvgTree* tree, const SvgPaints& paints, const SvgTree* expectedTree, const SvgPaints& expected,
        PrTextBuffer* difference) {
    bool same =
        samePaints(tree, paints, strokeOf(tree, paints), expectedTree, expected, strokeOf(expectedTree, expected));
    if (!same)
        prTextBufferAppend(difference, "paints differ");
    return same;
}

bool svgPathSame(const PrPath* path, const PrPath* expected) {
    uint32_t verbs = prPathVerbCount(path);
    uint32_t points = prPathPointCount(path);
    return verbs == prPathVerbCount(expected) && points == prPathPointCount(expected) &&
        prMemEquals(prPathVerbs(path), prPathVerbs(expected), verbs * sizeof(PrPathVerb)) &&
        prMemEquals(prPathPoints(path), prPathPoints(expected), points * sizeof(PrVec2));
}

void svgMarkerVertices(const PrPathDataCommand* PR_NULLABLE commands, uint32_t count, RawArray<SvgVertex>* out) {
    if (count == 0)
        return;
    RawArray<PrPathDataVertex> path;
    path.resize(count);
    const PrPathDataVertex* vertices = path.data;
    prPathDataVertices(commands, count, path.data);
    auto angleOf = [](PrVec2 direction) { return prF32Atan2(direction.y, direction.x); };
    // Half way between two directions, the shorter way round.
    auto between = [&](PrVec2 into, PrVec2 outOf) {
        float from = angleOf(into);
        float turn = angleOf(outOf) - from;
        if (turn > kPrF32Pi)
            turn -= 2 * kPrF32Pi;
        else if (turn <= -kPrF32Pi)
            turn += 2 * kPrF32Pi;
        return from + turn / 2;
    };
    auto isMove = [&](uint32_t i) { return i < count && (commands[i].letter | 0x20) == 'm'; };
    // Each subpath runs from its moveto at `start` up to the next moveto, at `end`.
    for (uint32_t start = 0; start < count;) {
        uint32_t end = start + 1;
        while (end < count && !isMove(end))
            ++end;
        bool closed = end > start + 1 && (commands[end - 1].letter | 0x20) == 'z';
        for (uint32_t i = start; i < end; ++i) {
            float angle = 0;
            if (i == start && closed)
                angle = between(vertices[end - 1].endDirection, vertices[start + 1].startDirection);
            else if (i == start)
                angle = angleOf(end > start + 1 ? vertices[start + 1].startDirection : vertices[start].endDirection);
            else if (i + 1 == end && closed)
                angle = between(vertices[i].endDirection, vertices[start + 1].startDirection);
            else if (i + 1 == end)
                angle = angleOf(vertices[i].endDirection);
            else
                angle = between(vertices[i].endDirection, vertices[i + 1].startDirection);
            out->push(SvgVertex{.point = vertices[i].point, .angle = angle});
        }
        start = end;
    }
    path.reset();
}

const PrDomElement* PR_NULLABLE svgBuildReference(SvgBuilder& b, const PrDomElement* from, PrStringView url, bool content) {
    if (url.size == 0)
        return nullptr;
    if (url.data[0] != '#') {
        DomReference reference{};
        if (!domReferenceSplit(from->document, url, &reference))
            return nullptr;
        // A URL of the document itself names an element by its fragment.
        if (!reference.external) {
            PrStringView fragment = reference.fragment;
            if (fragment.data != nullptr)
                return svgBuildReference(b, from, PrStringView{fragment.data - 1, fragment.size + 1}, content);
            return nullptr;
        }
        const PrDomElement* target = domReferenceTarget(from->document, reference);
        record(b, reference.fragment, target, nullptr, content, prRawStringView(&reference.url));
        prRawStringReset(&reference.url);
        return target;
    }
    if (url.size < 2)
        return nullptr;
    PrStringView id{url.data + 1, url.size - 1};
    const PrDomNode* root = from;
    while (root->parent != nullptr)
        root = root->parent;
    const PrDomNode* scope = domNodeShadowRoot(root) != nullptr ? root : nullptr;
    const PrDomElement* target = findInScope(scope, from->document, id);
    record(b, id, target, scope, content);
    return target;
}

const CssComputedStyle* PR_NULLABLE svgBuildStyle(SvgBuilder& b, const PrDomElement* e) {
    const SvgStyleSource& source = b.build->source;
    if (e->document == b.document)
        return source.style(source.context, e);
    return svgCascadeStyle(cascadeOf(b, e->document), e);
}

const CssComputedStyle* svgBuildStyleUnder(SvgBuilder& b, const PrDomElement* e, const CssComputedStyle* PR_NULLABLE parent) {
    const SvgStyleSource& source = b.build->source;
    if (e->document == b.document)
        return source.styleUnder(source.context, e, parent);
    return svgCascadeStyleUnder(cascadeOf(b, e->document), e, parent);
}

const CssComputedStyle* PR_NULLABLE svgBuildPseudoStyleUnder(SvgBuilder& b, const PrDomElement* e, CssPseudo pseudo,
        const CssComputedStyle* PR_NULLABLE parent) {
    const SvgStyleSource& source = b.build->source;
    if (e->document == b.document)
        return source.pseudoUnder(source.context, e, pseudo, parent);
    return svgCascadePseudoStyleUnder(cascadeOf(b, e->document), e, pseudo, parent);
}

const PrDomElement* PR_NULLABLE svgBuildHref(SvgBuilder& b, const PrDomElement* from, bool content) {
    return svgBuildReference(b, from, svgHref(from), content);
}

bool svgParseLength(PrStringView text, const CssComputedStyle* PR_NULLABLE style, SvgLengthPercentage* out) {
    size_t pos = 0;
    float number = 0;
    if (!numberListRead(text, &pos, &number))
        return false;
    PrStringView unit = prStringTrimEnd(prStringSlice(text, pos, text.size - pos));
    double em = style != nullptr ? cssStyleFontSize(style) : kCssMediumFontSize;
    if (unit.size == 0) {
        *out = SvgLengthPercentage{number, 0};
    } else if (unit == "%") {
        *out = SvgLengthPercentage{0, number};
    } else if (unit == "em") {
        *out = SvgLengthPercentage{float(double(number) * em), 0};
    } else if (unit == "ex") {
        *out = SvgLengthPercentage{float(double(number) * em / 2), 0};
    } else {
        PrCssUnit css = cssUnitFromText(unit);
        double scale = css != PrCssUnit_Unknown && cssUnitCategory(css) == CssUnitCategory_Length ? cssUnitScale(css) : 0;
        if (scale == 0)
            return false;
        *out = SvgLengthPercentage{float(double(number) * scale), 0};
    }
    return true;
}

void svgBuildInstanceChildren(SvgBuilder& b, const PrDomElement* element, const CssComputedStyle* style, SvgNodeIndex parent) {
    buildChildren(b, element, style, parent, true);
}

SvgLengthPercentage svgAttributeLength(const PrDomElement* element, PrStringView name, SvgLengthPercentage fallback,
        const CssComputedStyle* PR_NULLABLE style) {
    PrStringView text = svgAttribute(element, name);
    SvgLengthPercentage length{};
    return text.data != nullptr && svgParseLength(text, style, &length) ? length : fallback;
}

PrStringView svgAttribute(const PrDomElement* element, PrStringView name) {
    PrStringView value = domAttributeValue(element, name);
    if (value.data == nullptr)
        return value;
    return prStringTrim(value);
}

SvgSizeLimits svgStyleSizeLimits(const CssComputedStyle* style) {
    SvgSizeLimits limits{};
    limits.min[0] = svgStyleLength(style, PrCssProperty_MinWidth);
    limits.min[1] = svgStyleLength(style, PrCssProperty_MinHeight);
    limits.bounded[0] = !svgStyleIs(style, PrCssProperty_MaxWidth, PrCssKeyword_None);
    limits.bounded[1] = !svgStyleIs(style, PrCssProperty_MaxHeight, PrCssKeyword_None);
    if (limits.bounded[0])
        limits.max[0] = svgStyleLength(style, PrCssProperty_MaxWidth);
    if (limits.bounded[1])
        limits.max[1] = svgStyleLength(style, PrCssProperty_MaxHeight);
    return limits;
}

PrVec2 svgLimitExtent(const SvgSizeLimits& limits, PrVec2 extent, PrVec2 viewport) {
    float size[2] = {extent.x, extent.y};
    float base[2] = {viewport.x, viewport.y};
    for (uint32_t i = 0; i < 2; ++i) {
        if (limits.bounded[i])
            size[i] = prF32Min(size[i], svgLengthResolve(limits.max[i], base[i]));
        size[i] = prF32Max(size[i], svgLengthResolve(limits.min[i], base[i]));
    }
    return PrVec2{size[0], size[1]};
}

bool svgIs(const PrDomElement* element, PrDomTag tag) {
    return element->space == PrDomNamespace_Svg && element->tag == tag;
}

PrStringView svgHref(const PrDomElement* element) {
    PrStringView href = svgAttribute(element, "href");
    if (href.data == nullptr) {
        const PrDomAttr* xlink = domAttribute(element, PrDomNamespace_XLink, "href");
        if (xlink != nullptr)
            href = prRawStringView(&xlink->value);
    }
    return href;
}

PrStringView svgImageSource(const PrDomElement* element) {
    PrStringView href = svgHref(element);
    if (svgIs(element, PrDomTag_FeImage) && href.size != 0 && href.data[0] == '#')
        return PrStringView{};
    return href;
}

const PrDomElement* PR_NULLABLE svgFindById(const PrDomNode* document, PrStringView id) {
    return domElementById(const_cast<PrDomNode*>(document), id);
}

} // namespace pr_impl
