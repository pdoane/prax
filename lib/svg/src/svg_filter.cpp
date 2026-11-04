// SVG filters: filter elements as resources of the SVG tree, and an element's filter list as a lib/draw filter graph.
//
// filter-effects-1 - https://drafts.fxtf.org/filter-effects-1/

#include "svg/src/svg_filter.h"

#include "pr/float.h"
#include "pr/mem.h"
#include "pr/memory.h"

#include "dom/src/dom_node.h"
#include "svg/src/svg_image.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Building

// A primitive's result and the name that its result attribute gives it.
struct Named {
    PrStringView name;
    uint32_t index;
};

// What a filter's build collects before the filter owns it.
struct FilterParts {
    RawArray<SvgPrimitive> primitives;
    RawArray<float> numbers;
    RawArray<SvgFilterInput> merges;
    RawArray<Named> names;
    RawArray<float> scratch;

    // True for each primitive whose result is tainted (filter-effects-1/#tainted-filter-primitives), by primitive.
    RawArray<bool> tainted;
};

// Returns true if an input is tainted: every source is, and so is the result of a tainted primitive.
bool taints(const FilterParts& parts, SvgFilterInput input) {
    return input < 0 || parts.tainted.data[input];
}

// Returns true if the result of a primitive is tainted: it reads a color that currentColor decides, an element, or a
// tainted input (filter-effects-1/#tainted-filter-primitives). A color that mixes currentColor, or is relative to it,
// reads it too.
bool isTainted(const FilterParts& parts, const PrDomElement* e, const CssComputedStyle* PR_NULLABLE style,
        const SvgPrimitive& p) {
    auto current = [&](PrCssProperty property) {
        if (style == nullptr)
            return false;
        CssValueRun run = cssComputedValue(style, property);
        for (uint32_t i = 0; i < run.count; ++i) {
            if (cssValueIsKeyword(&run.values[i], PrCssKeyword_Currentcolor))
                return true;
        }
        return false;
    };
    switch (e->tag) {
        case PrDomTag_FeFlood:
        case PrDomTag_FeDropShadow:
            if (current(PrCssProperty_FloodColor))
                return true;
            break;
        case PrDomTag_FeDiffuseLighting:
        case PrDomTag_FeSpecularLighting:
            if (current(PrCssProperty_LightingColor))
                return true;
            break;
        case PrDomTag_FeImage:
            if (p.kind == SvgPrimitiveKind_Image && p.image.node != kSvgNodeNone)
                return true;
            break;
        default: break;
    }
    if (p.kind == SvgPrimitiveKind_Merge) {
        for (uint32_t i = 0; i < p.merge.count; ++i) {
            if (taints(parts, parts.merges.data[p.merge.first + i]))
                return true;
        }
        return false;
    }
    switch (p.kind) {
        case SvgPrimitiveKind_Flood:
        case SvgPrimitiveKind_Image:
        case SvgPrimitiveKind_Turbulence:
        case SvgPrimitiveKind_Transparent: return false;
        case SvgPrimitiveKind_Blend:
        case SvgPrimitiveKind_Composite:
        case SvgPrimitiveKind_Arithmetic: return taints(parts, p.inputs[0]) || taints(parts, p.inputs[1]);
        default: return taints(parts, p.inputs[0]);
    }
}

// The most filters that a chain of templates follows.
constexpr uint32_t kMaxTemplates = 16;

// Returns `value` rounded toward zero.
float truncated(float value) {
    return value < 0 ? prF32Ceil(value) : prF32Floor(value);
}

// Returns the larger of two counts.
uint32_t larger(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

// Returns true if an element is a filter primitive (filter-effects-1/#elementdef-filter-primitive).
bool isPrimitive(const PrDomElement* e) {
    if (e->space != PrDomNamespace_Svg)
        return false;
    switch (e->tag) {
        case PrDomTag_FeBlend:
        case PrDomTag_FeColorMatrix:
        case PrDomTag_FeComponentTransfer:
        case PrDomTag_FeComposite:
        case PrDomTag_FeConvolveMatrix:
        case PrDomTag_FeDiffuseLighting:
        case PrDomTag_FeDisplacementMap:
        case PrDomTag_FeDropShadow:
        case PrDomTag_FeFlood:
        case PrDomTag_FeGaussianBlur:
        case PrDomTag_FeImage:
        case PrDomTag_FeMerge:
        case PrDomTag_FeMorphology:
        case PrDomTag_FeOffset:
        case PrDomTag_FeSpecularLighting:
        case PrDomTag_FeTile:
        case PrDomTag_FeTurbulence: return true;
        default: return false;
    }
}

// Parses the attribute `name` as a list of numbers into `numbers`. Returns false if the attribute is missing or a
// number does not parse.
bool numbersOf(const PrDomElement* e, PrStringView name, RawArray<float>* numbers) {
    PrStringView text = svgAttribute(e, name);
    numbers->clear();
    return text.data != nullptr && svgParseNumbers(text, numbers);
}

// Returns the number that the attribute `name` gives, or `fallback` if it is missing or is not one number.
float numberOf(FilterParts& parts, const PrDomElement* e, PrStringView name, float fallback) {
    return numbersOf(e, name, &parts.scratch) && parts.scratch.count == 1 ? parts.scratch.data[0] : fallback;
}

// The outcome of reading a <number-optional-number>.
enum {
    Pair_Missing,
    Pair_Given,
    Pair_Error,                 // the attribute holds no number, or more than two
};
typedef uint8_t Pair;

// Reads a <number-optional-number> into `out`, whose second number is the first if it has only one.
Pair pairOf(FilterParts& parts, const PrDomElement* e, PrStringView name, PrVec2* out) {
    if (svgAttribute(e, name).data == nullptr)
        return Pair_Missing;
    if (!numbersOf(e, name, &parts.scratch) || parts.scratch.count == 0 || parts.scratch.count > 2)
        return Pair_Error;
    out->x = parts.scratch.data[0];
    out->y = parts.scratch.data[parts.scratch.count - 1];
    return Pair_Given;
}

// Returns the input that the value of an `in` or `in2` attribute names: a source, or the result of the closest
// preceding primitive of that name (filter-effects-1/#element-attrdef-filter-primitive-in). A missing value, or a name
// of no preceding result, gives `implicit`.
SvgFilterInput inputOf(const FilterParts& parts, PrStringView value, SvgFilterInput implicit) {
    if (value.data == nullptr)
        return implicit;
    if (value == "SourceGraphic")
        return SvgFilterInput_Graphic;
    if (value == "SourceAlpha")
        return SvgFilterInput_Alpha;
    if (value == "BackgroundImage")
        return SvgFilterInput_Background;
    if (value == "BackgroundAlpha")
        return SvgFilterInput_BackgroundAlpha;
    if (value == "FillPaint")
        return SvgFilterInput_Fill;
    if (value == "StrokePaint")
        return SvgFilterInput_Stroke;
    for (uint32_t i = parts.names.count; i-- > 0;) {
        if (parts.names.data[i].name == value)
            return SvgFilterInput(parts.names.data[i].index);
    }
    return implicit;
}

// Returns the space that color-interpolation-filters gives a primitive. auto is sRGB, which the user agent may choose
// (filter-effects-1/#ColorInterpolationFiltersProperty).
PrColorSpace spaceOf(const CssComputedStyle* PR_NULLABLE style) {
    if (style == nullptr)
        return PrColorSpace_SrgbLinear;
    PrCssKeyword keyword = svgStyleKeyword(style, PrCssProperty_ColorInterpolationFilters);
    return keyword == PrCssKeyword_Srgb || keyword == PrCssKeyword_Auto ? PrColorSpace_Srgb : PrColorSpace_SrgbLinear;
}

// Returns the flood color of a primitive's style: flood-color, with its alpha times flood-opacity
// (filter-effects-1/#FloodOpacityProperty).
PrColor floodOf(SvgBuilder& b, const CssComputedStyle* PR_NULLABLE style) {
    if (style == nullptr)
        return PrColor{0, 0, 0, 1};
    PrColor color = svgStyleColor(style, PrCssProperty_FloodColor, b.build->environment);
    color.a *= prF32Clamp(svgStyleNumber(style, PrCssProperty_FloodOpacity, 1), 0, 1);
    return color;
}

// Returns the edge mode that an edgeMode attribute names, or `fallback` for a missing or unknown value.
PrImageEdgeMode edgeOf(const PrDomElement* e, PrImageEdgeMode fallback) {
    PrStringView value = svgAttribute(e, "edgeMode");
    if (value == "none")
        return PrImageEdgeMode_None;
    if (value == "duplicate")
        return PrImageEdgeMode_Duplicate;
    if (value == "wrap")
        return PrImageEdgeMode_Wrap;
    if (value == "mirror")
        return PrImageEdgeMode_Mirror;
    return fallback;
}

// Returns the channel that a channel selector names, A for a missing or unknown value.
PrDrawChannel channelOf(PrStringView value) {
    if (value == "R")
        return PrDrawChannel_R;
    if (value == "G")
        return PrDrawChannel_G;
    if (value == "B")
        return PrDrawChannel_B;
    return PrDrawChannel_A;
}

// Writes the blend mode that feBlend's mode names to `out` (compositing-1/#ltblendmodegt). Returns false for an unknown
// mode.
bool blendOf(PrStringView mode, PrDrawBlendMode* out) {
    struct Mode {
        const char* name;
        PrDrawBlendMode mode;
    };
    static const Mode kModes[] = {
        {"normal", PrDrawBlendMode_Normal},          {"multiply", PrDrawBlendMode_Multiply},
        {"screen", PrDrawBlendMode_Screen},          {"overlay", PrDrawBlendMode_Overlay},
        {"darken", PrDrawBlendMode_Darken},          {"lighten", PrDrawBlendMode_Lighten},
        {"color-dodge", PrDrawBlendMode_ColorDodge}, {"color-burn", PrDrawBlendMode_ColorBurn},
        {"hard-light", PrDrawBlendMode_HardLight},   {"soft-light", PrDrawBlendMode_SoftLight},
        {"difference", PrDrawBlendMode_Difference},  {"exclusion", PrDrawBlendMode_Exclusion},
        {"hue", PrDrawBlendMode_Hue},                {"saturation", PrDrawBlendMode_Saturation},
        {"color", PrDrawBlendMode_Color},            {"luminosity", PrDrawBlendMode_Luminosity},
    };
    for (const Mode& m : kModes) {
        if (mode == m.name) {
            *out = m.mode;
            return true;
        }
    }
    return false;
}

// Reads a feColorMatrix (filter-effects-1/#feColorMatrixElement). A list of values of the wrong length for the type
// makes the primitive pass its input through.
void readColorMatrix(FilterParts& parts, const PrDomElement* e, SvgPrimitive* p) {
    PrStringView type = svgAttribute(e, "type");
    bool given = svgAttribute(e, "values").data != nullptr;
    bool parsed = given && numbersOf(e, "values", &parts.scratch);
    uint32_t count = parsed ? parts.scratch.count : 0;
    const float* values = parts.scratch.data;
    p->kind = SvgPrimitiveKind_ColorMatrix;
    if (type == "saturate") {
        if (given && count != 1)
            p->kind = SvgPrimitiveKind_PassThrough;
        else
            p->matrix = prDrawColorMatrixSaturate(given ? values[0] : 1);
    } else if (type == "hueRotate") {
        if (given && count != 1)
            p->kind = SvgPrimitiveKind_PassThrough;
        else
            p->matrix = prDrawColorMatrixHueRotate(given ? values[0] * kPrF32Pi / 180 : 0);
    } else if (type == "luminanceToAlpha") {
        p->matrix = prDrawColorMatrixLuminanceToAlpha();
    } else if (!given) {
        p->kind = SvgPrimitiveKind_PassThrough;
    } else if (count != 20) {
        p->kind = SvgPrimitiveKind_PassThrough;
    } else {
        for (uint32_t k = 0; k < 20; ++k)
            p->matrix.m[k / 5][k % 5] = values[k];
    }
}

// Reads a feComponentTransfer's transfer functions (filter-effects-1/#feComponentTransferElement). The last function
// element of each channel applies, and a channel without one, or with an unknown type, is the identity.
void readTransfer(FilterParts& parts, const PrDomElement* e, SvgPrimitive* p) {
    p->kind = SvgPrimitiveKind_ComponentTransfer;
    for (SvgTransfer& t : p->transfer)
        t = SvgTransfer{.kind = PrDrawTransferKind_Identity};
    for (const PrDomNode* child = e->first; child != nullptr; child = child->next) {
        const PrDomElement* f = domElementFromNode(child);
        if (f == nullptr || f->space != PrDomNamespace_Svg)
            continue;
        PrDrawChannel channel = PrDrawChannel_Count;
        switch (f->tag) {
            case PrDomTag_FeFuncR: channel = PrDrawChannel_R; break;
            case PrDomTag_FeFuncG: channel = PrDrawChannel_G; break;
            case PrDomTag_FeFuncB: channel = PrDrawChannel_B; break;
            case PrDomTag_FeFuncA: channel = PrDrawChannel_A; break;
            default: continue;
        }
        SvgTransfer t{.kind = PrDrawTransferKind_Identity};
        PrStringView type = svgAttribute(f, "type");
        if (type == "table")
            t.kind = PrDrawTransferKind_Table;
        else if (type == "discrete")
            t.kind = PrDrawTransferKind_Discrete;
        else if (type == "linear")
            t.kind = PrDrawTransferKind_Linear;
        else if (type == "gamma")
            t.kind = PrDrawTransferKind_Gamma;
        t.slope = numberOf(parts, f, "slope", 1);
        t.intercept = numberOf(parts, f, "intercept", 0);
        t.amplitude = numberOf(parts, f, "amplitude", 1);
        t.exponent = numberOf(parts, f, "exponent", 1);
        t.offset = numberOf(parts, f, "offset", 0);
        // An empty or missing table is the identity, which lib/draw makes of a table without values.
        if ((t.kind == PrDrawTransferKind_Table || t.kind == PrDrawTransferKind_Discrete) &&
                numbersOf(f, "tableValues", &parts.scratch)) {
            t.first = parts.numbers.count;
            t.count = parts.scratch.count;
            parts.numbers.append(parts.scratch.span());
        }
        p->transfer[channel] = t;
    }
}

// Reads a feComposite (filter-effects-1/#feCompositeElement). An unknown operator is over.
void readComposite(FilterParts& parts, const PrDomElement* e, SvgPrimitive* p) {
    PrStringView op = svgAttribute(e, "operator");
    if (op == "arithmetic") {
        p->kind = SvgPrimitiveKind_Arithmetic;
        p->k[0] = numberOf(parts, e, "k1", 0);
        p->k[1] = numberOf(parts, e, "k2", 0);
        p->k[2] = numberOf(parts, e, "k3", 0);
        p->k[3] = numberOf(parts, e, "k4", 0);
        return;
    }
    p->kind = SvgPrimitiveKind_Composite;
    if (op == "in")
        p->composite = PrDrawComposite_SourceIn;
    else if (op == "out")
        p->composite = PrDrawComposite_SourceOut;
    else if (op == "atop")
        p->composite = PrDrawComposite_SourceAtop;
    else if (op == "xor")
        p->composite = PrDrawComposite_Xor;
    else if (op == "lighter")
        p->composite = PrDrawComposite_PlusLighter;
    else
        p->composite = PrDrawComposite_SourceOver;
}

// Reads a feConvolveMatrix (filter-effects-1/#feConvolveMatrixElement). An order that is not positive, a kernel of the
// wrong length and a target outside the kernel are errors, which make the primitive transparent black.
void readConvolve(FilterParts& parts, const PrDomElement* e, SvgPrimitive* p) {
    p->kind = SvgPrimitiveKind_Transparent;
    PrVec2 order{3, 3};
    if (pairOf(parts, e, "order", &order) == Pair_Error)
        return;
    // A number that is not an integer is truncated.
    order = PrVec2{truncated(order.x), truncated(order.y)};
    float most = float(kPrDrawConvolutionMaxExtent);
    if (!(order.x >= 1) || !(order.y >= 1) || order.x > most || order.y > most)
        return;
    PrUVec2 extent{uint32_t(order.x), uint32_t(order.y)};
    if (!numbersOf(e, "kernelMatrix", &parts.scratch) || parts.scratch.count != extent.x * extent.y)
        return;
    uint32_t first = parts.numbers.count;
    parts.numbers.append(parts.scratch.span());
    float targetX = numberOf(parts, e, "targetX", float(extent.x / 2));
    float targetY = numberOf(parts, e, "targetY", float(extent.y / 2));
    if (!(targetX >= 0) || !(targetY >= 0) || targetX >= order.x || targetY >= order.y) {
        parts.numbers.count = first;
        return;
    }
    p->kind = SvgPrimitiveKind_ConvolveMatrix;
    p->convolve.order = extent;
    p->convolve.target = PrUVec2{uint32_t(targetX), uint32_t(targetY)};
    p->convolve.first = first;
    p->convolve.divisor = numberOf(parts, e, "divisor", 0);
    p->convolve.bias = numberOf(parts, e, "bias", 0);
    p->convolve.edge = edgeOf(e, PrImageEdgeMode_Duplicate);
    p->convolve.preserveAlpha = svgAttribute(e, "preserveAlpha") == "true";
}

// Reads a feDiffuseLighting or feSpecularLighting and its light source, its first child that is one
// (filter-effects-1/#LightSourceDefinitions). A primitive without a light is transparent black.
void readLighting(SvgBuilder& b, FilterParts& parts, const PrDomElement* e, const CssComputedStyle* PR_NULLABLE style,
        SvgPrimitive* p) {
    p->kind = SvgPrimitiveKind_Transparent;
    const PrDomElement* light = nullptr;
    for (const PrDomNode* child = e->first; child != nullptr && light == nullptr; child = child->next) {
        const PrDomElement* c = domElementFromNode(child);
        if (c != nullptr && (svgIs(c, PrDomTag_FeDistantLight) || svgIs(c, PrDomTag_FePointLight) ||
                svgIs(c, PrDomTag_FeSpotLight)))
            light = c;
    }
    if (light == nullptr)
        return;
    bool specular = svgIs(e, PrDomTag_FeSpecularLighting);
    p->kind = SvgPrimitiveKind_Lighting;
    p->lighting.kind = specular ? PrDrawLightingKind_Specular : PrDrawLightingKind_Diffuse;
    p->lighting.surfaceScale = numberOf(parts, e, "surfaceScale", 1);
    p->lighting.constant = numberOf(parts, e, specular ? "specularConstant" : "diffuseConstant", 1);
    p->lighting.specularExponent = numberOf(parts, e, "specularExponent", 1);
    // feSpecularLighting's exponent is in [1, 128] (SVG11/filters.html#feSpecularLightingSpecularExponentAttribute),
    // and one outside it is an error.
    if (specular && !(p->lighting.specularExponent >= 1 && p->lighting.specularExponent <= 128)) {
        p->kind = SvgPrimitiveKind_Transparent;
        return;
    }
    if (style != nullptr)
        p->lighting.color = svgStyleColor(style, PrCssProperty_LightingColor, b.build->environment);
    else
        p->lighting.color = PrColor{1, 1, 1, 1};
    PrDrawLight& l = p->lighting.light;
    float degrees = kPrF32Pi / 180;
    if (svgIs(light, PrDomTag_FeDistantLight)) {
        l.kind = PrDrawLightKind_Distant;
        l.azimuth = numberOf(parts, light, "azimuth", 0) * degrees;
        l.elevation = numberOf(parts, light, "elevation", 0) * degrees;
        return;
    }
    l.kind = svgIs(light, PrDomTag_FePointLight) ? PrDrawLightKind_Point : PrDrawLightKind_Spot;
    l.position = PrVec3{numberOf(parts, light, "x", 0), numberOf(parts, light, "y", 0), numberOf(parts, light, "z", 0)};
    if (l.kind == PrDrawLightKind_Point)
        return;
    l.pointsAt = PrVec3{
        numberOf(parts, light, "pointsAtX", 0), numberOf(parts, light, "pointsAtY", 0),
        numberOf(parts, light, "pointsAtZ", 0),
    };
    // A negative exponent of the spot is taken as its initial value.
    l.spotExponent = numberOf(parts, light, "specularExponent", 1);
    if (l.spotExponent < 0)
        l.spotExponent = 1;
    // A missing limitingConeAngle is no cone. A cone of no angle lights nothing, which a cone narrower than any pixel
    // draws, since lib/draw takes an angle of zero for no cone.
    bool cone = numbersOf(light, "limitingConeAngle", &parts.scratch) && parts.scratch.count == 1;
    l.coneAngle = cone ? prF32Max(prF32Abs(parts.scratch.data[0]) * degrees, 1e-6f) : 0;
}

// Reads a feTurbulence (filter-effects-1/#feTurbulenceElement). A negative base frequency is an error, which makes the
// primitive transparent black.
void readTurbulence(FilterParts& parts, const PrDomElement* e, SvgPrimitive* p) {
    PrVec2 frequency{0, 0};
    Pair given = pairOf(parts, e, "baseFrequency", &frequency);
    if (given == Pair_Error || frequency.x < 0 || frequency.y < 0) {
        p->kind = SvgPrimitiveKind_Transparent;
        return;
    }
    float octaves = numberOf(parts, e, "numOctaves", 1);
    p->kind = SvgPrimitiveKind_Turbulence;
    bool fractal = svgAttribute(e, "type") == "fractalNoise";
    p->turbulence = PrDrawFilterTurbulence{
        .baseFrequency = frequency,
        .octaves = octaves > 0 ? uint32_t(prF32Min(octaves, float(kPrDrawTurbulenceMaxOctaves))) : 0,
        // The seed is truncated toward zero.
        .seed = truncated(numberOf(parts, e, "seed", 0)),
        .kind = PrDrawTurbulenceKind(fractal ? PrDrawTurbulenceKind_FractalNoise : PrDrawTurbulenceKind_Turbulence),
        .stitch = svgAttribute(e, "stitchTiles") == "stitch",
    };
}

// Reads a feImage: an image that its href names, or an element of the document, which is built in the filter's content
// group (filter-effects-1/#feImageElement). A reference that names nothing draws transparent black.
void readImage(SvgBuilder& b, SvgResourceIndex index, const PrDomElement* e, const CssComputedStyle* PR_NULLABLE style,
        SvgPrimitive* p) {
    p->kind = SvgPrimitiveKind_Image;
    p->image.node = kSvgNodeNone;
    p->image.picture = SvgPicture{};
    p->image.aspect = SvgAspect{};
    svgParseAspect(svgAttribute(e, "preserveAspectRatio"), &p->image.aspect);
    p->image.sampling = PrDrawSampling_Linear;
    if (style != nullptr) {
        PrCssKeyword rendering = svgStyleKeyword(style, PrCssProperty_ImageRendering);
        if (rendering == PrCssKeyword_Pixelated || rendering == PrCssKeyword_CrispEdges ||
                rendering == PrCssKeyword_Optimizespeed)
            p->image.sampling = PrDrawSampling_Nearest;
    }
    PrStringView href = svgHref(e);
    if (href.size == 0)
        return;
    if (href.data[0] == '#') {
        const PrDomElement* target = svgBuildReference(b, e, href, true);
        if (target == nullptr)
            return;
        if (b.tree->resources.data[index].filter.content == kSvgNodeNone) {
            SvgNodeIndex content = svgBuildContentGroup(b, b.tree->resources.data[index].element);
            b.tree->resources.data[index].filter.content = content;
        }
        p->image.node = svgBuildElement(b, target, b.tree->resources.data[index].filter.content);
        return;
    }
    const SvgImageSource& images = b.build->images;
    if (images.load != nullptr)
        images.load(images.context, e, href, &p->image.picture);
}

// Reads one primitive into `p`, whose common attributes are read.
void readPrimitive(SvgBuilder& b, SvgResourceIndex index, FilterParts& parts, const PrDomElement* e,
        const CssComputedStyle* PR_NULLABLE style, SvgFilterInput implicit, SvgPrimitive* p) {
    p->inputs[0] = inputOf(parts, svgAttribute(e, "in"), implicit);
    p->inputs[1] = inputOf(parts, svgAttribute(e, "in2"), implicit);
    switch (e->tag) {
        case PrDomTag_FeBlend:
            p->kind = SvgPrimitiveKind_Blend;
            p->blend = PrDrawBlendMode_Normal;
            blendOf(svgAttribute(e, "mode"), &p->blend);
            break;
        case PrDomTag_FeColorMatrix: readColorMatrix(parts, e, p); break;
        case PrDomTag_FeComponentTransfer: readTransfer(parts, e, p); break;
        case PrDomTag_FeComposite: readComposite(parts, e, p); break;
        case PrDomTag_FeConvolveMatrix: readConvolve(parts, e, p); break;
        case PrDomTag_FeDiffuseLighting:
        case PrDomTag_FeSpecularLighting: readLighting(b, parts, e, style, p); break;
        case PrDomTag_FeDisplacementMap:
            p->kind = SvgPrimitiveKind_DisplacementMap;
            p->displacement.scale = numberOf(parts, e, "scale", 0);
            p->displacement.x = channelOf(svgAttribute(e, "xChannelSelector"));
            p->displacement.y = channelOf(svgAttribute(e, "yChannelSelector"));
            break;
        case PrDomTag_FeDropShadow: {
            // A negative deviation is zero on its axis.
            PrVec2 sigma{2, 2};
            if (pairOf(parts, e, "stdDeviation", &sigma) == Pair_Error)
                sigma = PrVec2{2, 2};
            p->kind = SvgPrimitiveKind_DropShadow;
            p->shadow.offset = PrVec2{numberOf(parts, e, "dx", 2), numberOf(parts, e, "dy", 2)};
            p->shadow.sigma = PrVec2{prF32Max(sigma.x, 0), prF32Max(sigma.y, 0)};
            p->shadow.color = floodOf(b, style);
            break;
        }
        case PrDomTag_FeFlood:
            p->kind = SvgPrimitiveKind_Flood;
            p->flood = floodOf(b, style);
            break;
        case PrDomTag_FeGaussianBlur: {
            // A negative deviation is zero on its axis, and a deviation of zero on both passes the input through.
            PrVec2 sigma{0, 0};
            if (pairOf(parts, e, "stdDeviation", &sigma) == Pair_Error)
                sigma = PrVec2{0, 0};
            p->kind = SvgPrimitiveKind_GaussianBlur;
            p->blur.sigma = PrVec2{prF32Max(sigma.x, 0), prF32Max(sigma.y, 0)};
            p->blur.edge = edgeOf(e, PrImageEdgeMode_None);
            break;
        }
        case PrDomTag_FeImage: readImage(b, index, e, style, p); break;
        case PrDomTag_FeMerge: {
            p->kind = SvgPrimitiveKind_Merge;
            p->merge.first = parts.merges.count;
            for (const PrDomNode* child = e->first; child != nullptr; child = child->next) {
                const PrDomElement* node = domElementFromNode(child);
                if (node != nullptr && svgIs(node, PrDomTag_FeMergeNode))
                    parts.merges.push(inputOf(parts, svgAttribute(node, "in"), implicit));
            }
            p->merge.count = parts.merges.count - p->merge.first;
            break;
        }
        case PrDomTag_FeMorphology: {
            // A negative radius passes the input through, and a radius of zero leaves its axis alone.
            PrVec2 radius{0, 0};
            Pair given = pairOf(parts, e, "radius", &radius);
            p->kind = SvgPrimitiveKind_Morphology;
            if (given == Pair_Error || radius.x < 0 || radius.y < 0 || (radius.x == 0 && radius.y == 0))
                p->kind = SvgPrimitiveKind_PassThrough;
            p->morphology.radius = radius;
            p->morphology.op =
                svgAttribute(e, "operator") == "dilate" ? PrDrawMorphologyOp_Dilate : PrDrawMorphologyOp_Erode;
            break;
        }
        case PrDomTag_FeOffset:
            p->kind = SvgPrimitiveKind_Offset;
            p->offset = PrVec2{numberOf(parts, e, "dx", 0), numberOf(parts, e, "dy", 0)};
            break;
        case PrDomTag_FeTile: p->kind = SvgPrimitiveKind_Tile; break;
        case PrDomTag_FeTurbulence: readTurbulence(parts, e, p); break;
        default: p->kind = SvgPrimitiveKind_Transparent; break;
    }
}

// Reads the attribute `name` of a primitive's subregion into `out`, and returns `flag` if it is given and parses.
SvgSubregionFlags subregionOf(const PrDomElement* e, PrStringView name, const CssComputedStyle* PR_NULLABLE style,
        SvgSubregionFlags flag, SvgLengthPercentage* out) {
    PrStringView text = svgAttribute(e, name);
    return text.data != nullptr && svgParseLength(text, style, out) ? flag : 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Drawing

// A filter element's graph as it is built: the filter, where it is drawn, and the nodes of its sources and results.
struct Graph {
    SvgDrawer* d;
    const SvgNode* node;            // what the filter applies to
    SvgResourceIndex index;
    const SvgFilter* filter;
    PrDrawFilterBuilder* builder;
    PrRect box;                     // the object bounding box of what the filter applies to
    PrRect region;                  // the filter region, in user space
    PrDrawFilterInput source;       // the input of the filter, SourceGraphic before the filter region clips it

    // The node of each source, made when a primitive first reads it, or -1 until then.
    PrDrawFilterInput sources[6];

    // The node of each primitive's result, and the subregion that it produces in, in user space.
    RawArray<PrDrawFilterInput> results;
    RawArray<PrRect> subregions;
};

// Returns a length along an axis in the filter's primitive units: user units, or a fraction of the bounding box.
float primitiveLength(const Graph& g, float value, SvgAxis axis) {
    if (!g.filter->primitiveBoundingBox)
        return value;
    float w = prRectWidth(g.box);
    float h = prRectHeight(g.box);
    return axis == SvgAxis_X ? value * w : axis == SvgAxis_Y ? value * h : value * prF32Sqrt((w * w + h * h) / 2);
}

// Returns a position along an axis in the filter's primitive units.
float primitivePosition(const Graph& g, float value, SvgAxis axis) {
    if (!g.filter->primitiveBoundingBox)
        return value;
    return axis == SvgAxis_X ? g.box.left + value * prRectWidth(g.box) : g.box.top + value * prRectHeight(g.box);
}

// Returns a length of a subregion or region along an axis: in user space with percentages of the viewport, or a
// fraction of the bounding box, which a percentage is a hundredth of. `origin` is added to a fraction of a position.
float regionLength(const SvgDrawer& d, bool userSpace, SvgLengthPercentage length, SvgAxis axis, float origin, float extent) {
    if (userSpace)
        return svgLengthResolve(length, svgPercentBase(d, axis));
    return origin + (length.px + length.percent / 100) * extent;
}

// Appends a node to the graph and returns it.
PrDrawFilterInput add(Graph& g, const PrDrawFilterNode& node) {
    return prDrawFilterBuilderAdd(g.builder, &node);
}

// Returns a node that copies `input` within `region`.
PrDrawFilterNode copyOf(PrDrawFilterInput input, PrRect region) {
    PrDrawFilterNode node{.kind = PrDrawFilterKind_Offset, .space = PrColorSpace_Srgb, .region = region};
    node.inputs[0] = input;
    return node;
}

// Returns a node of the alpha of `input` within `region`, as black.
PrDrawFilterNode alphaOf(PrDrawFilterInput input, PrRect region) {
    PrDrawFilterNode node{.kind = PrDrawFilterKind_ColorMatrix, .space = PrColorSpace_Srgb, .region = region};
    node.inputs[0] = input;
    node.matrix.m[3][3] = 1;
    return node;
}

// Returns a node of transparent black within `region`.
PrDrawFilterNode transparent(PrRect region) {
    return PrDrawFilterNode{.kind = PrDrawFilterKind_Flood, .space = PrColorSpace_Srgb, .region = region};
}

// Returns the node of a list that `record` draws in user space, shown over `area`.
template <typename Record>
PrDrawFilterInput listNode(Graph& g, PrRect area, Record record) {
    PrDrawList* list = prDrawListCreate();
    SvgDrawer inner = svgEffectDrawer(*g.d, g.index, list);
    record(inner);
    PrDrawFilterNode node{.kind = PrDrawFilterKind_Image, .space = PrColorSpace_Srgb, .region = area};
    node.image = PrDrawFilterImage{
        .source = prDrawSourceList(list, area),
        .rect = area,
        .sampling = PrDrawSampling_Linear,
    };
    PrDrawFilterInput made = add(g, node);
    prDrawListRelease(list);
    return made;
}

// Returns the node of a filter source, made the first time that a primitive reads it. The filter region clips each
// source (filter-effects-1/#FilterPrimitiveSubRegion). FillPaint and StrokePaint are the element's fill and stroke over
// the filter region (filter-effects-1/#attr-valuedef-in-fillpaint).
PrDrawFilterInput sourceOf(Graph& g, SvgFilterInput which) {
    PrDrawFilterInput& made = g.sources[-which - 1];
    if (made >= 0)
        return made;
    switch (which) {
        case SvgFilterInput_Graphic: made = add(g, copyOf(g.source, g.region)); break;
        case SvgFilterInput_Alpha: made = add(g, alphaOf(sourceOf(g, SvgFilterInput_Graphic), g.region)); break;
        case SvgFilterInput_Background: made = add(g, copyOf(PrDrawFilterInput_Backdrop, g.region)); break;
        case SvgFilterInput_BackgroundAlpha:
            made = add(g, alphaOf(sourceOf(g, SvgFilterInput_Background), g.region));
            break;
        default: {
            bool fill = which == SvgFilterInput_Fill;
            const SvgPaints& paints = g.node->paints;
            made = listNode(g, g.region, [&](SvgDrawer& inner) {
                PrDrawPaint paint{};
                PrDrawBrush brush{};
                const SvgPaint& from = fill ? paints.fill : paints.stroke;
                float opacity = fill ? paints.fillOpacity : paints.strokeOpacity;
                if (svgPaintFor(inner, from, opacity, g.box, &paint, &brush))
                    prDrawListRect(inner.list, g.region, paint);
            });
            break;
        }
    }
    return made;
}

// Returns the node that an input of a primitive reads.
PrDrawFilterInput inputNode(Graph& g, SvgFilterInput input) {
    return input >= 0 ? g.results.data[input] : sourceOf(g, input);
}

// Returns the subregion of an input: its primitive's, or the filter region for a source.
PrRect inputSubregion(const Graph& g, SvgFilterInput input) {
    return input >= 0 ? g.subregions.data[input] : g.region;
}

// Returns the number of inputs that a primitive reads, or the inputs of its merge.
uint32_t inputsRead(const SvgPrimitive& p) {
    switch (p.kind) {
        case SvgPrimitiveKind_Blend:
        case SvgPrimitiveKind_Composite:
        case SvgPrimitiveKind_Arithmetic:
        case SvgPrimitiveKind_DisplacementMap: return 2;
        case SvgPrimitiveKind_Flood:
        case SvgPrimitiveKind_Image:
        case SvgPrimitiveKind_Turbulence:
        case SvgPrimitiveKind_Merge:
        case SvgPrimitiveKind_Transparent: return 0;
        default: return 1;
    }
}

// Returns a primitive's subregion in user space (filter-effects-1/#FilterPrimitiveSubRegion). Each attribute that the
// primitive does not give comes from the union of its inputs' subregions, or from the filter region if it reads no
// result, reads a source, or is a feTile.
PrRect subregionOf(const Graph& g, const SvgPrimitive& p) {
    PrRect base{};
    bool region = p.kind == SvgPrimitiveKind_Tile;
    bool any = false;
    auto join = [&](SvgFilterInput input) {
        if (input < 0)
            region = true;
        else
            base = any ? prRectUnion(base, g.subregions.data[input]) : g.subregions.data[input];
        any = true;
    };
    if (p.kind == SvgPrimitiveKind_Merge) {
        for (uint32_t i = 0; i < p.merge.count; ++i)
            join(g.filter->merges[p.merge.first + i]);
    } else {
        for (uint32_t i = 0; i < inputsRead(p); ++i)
            join(p.inputs[i]);
    }
    if (region || !any)
        base = g.region;
    const SvgDrawer& d = *g.d;
    bool user = !g.filter->primitiveBoundingBox;
    float x = (p.given & SvgSubregionFlag_X) ? regionLength(d, user, p.x, SvgAxis_X, g.box.left, prRectWidth(g.box)) : base.left;
    float y = (p.given & SvgSubregionFlag_Y) ? regionLength(d, user, p.y, SvgAxis_Y, g.box.top, prRectHeight(g.box)) : base.top;
    float w;
    if ((p.given & SvgSubregionFlag_Width))
        w = regionLength(d, user, p.width, SvgAxis_X, 0, prRectWidth(g.box));
    else
        w = prRectWidth(base);
    float h;
    if ((p.given & SvgSubregionFlag_Height))
        h = regionLength(d, user, p.height, SvgAxis_Y, 0, prRectHeight(g.box));
    else
        h = prRectHeight(base);
    return PrRect{x, y, x + w, y + h};
}

// Appends the nodes of a primitive whose output is clipped to `clip`, its subregion within the filter region, and
// returns the node of its result.
PrDrawFilterInput primitiveNode(Graph& g, const SvgPrimitive& p, PrRect subregion, PrRect clip) {
    const SvgFilter& f = *g.filter;
    PrDrawFilterNode node{.space = p.space, .region = clip};
    if (p.kind != SvgPrimitiveKind_Merge && inputsRead(p) >= 1)
        node.inputs[0] = inputNode(g, p.inputs[0]);
    else
        node.inputs[0] = PrDrawFilterInput_Source;
    node.inputs[1] = inputsRead(p) >= 2 ? inputNode(g, p.inputs[1]) : PrDrawFilterInput_Source;
    auto length = [&](float value, SvgAxis axis) { return primitiveLength(g, value, axis); };
    switch (p.kind) {
        case SvgPrimitiveKind_Blend:
            node.kind = PrDrawFilterKind_Blend;
            node.blend = p.blend;
            break;
        case SvgPrimitiveKind_ColorMatrix:
            node.kind = PrDrawFilterKind_ColorMatrix;
            node.matrix = p.matrix;
            break;
        case SvgPrimitiveKind_ComponentTransfer:
            node.kind = PrDrawFilterKind_ComponentTransfer;
            for (uint32_t c = 0; c < PrDrawChannel_Count; ++c) {
                const SvgTransfer& t = p.transfer[c];
                const float* values = t.count != 0 ? f.numbers + t.first : nullptr;
                node.transfer[c] = PrDrawTransfer{
                    .kind = t.kind,
                    .values = values,
                    .valueCount = t.count,
                    .slope = t.slope,
                    .intercept = t.intercept,
                    .amplitude = t.amplitude,
                    .exponent = t.exponent,
                    .offset = t.offset,
                };
            }
            break;
        case SvgPrimitiveKind_Composite:
            node.kind = PrDrawFilterKind_Composite;
            node.composite = p.composite;
            break;
        case SvgPrimitiveKind_Arithmetic:
            node.kind = PrDrawFilterKind_Arithmetic;
            prMemCopy(node.k, p.k, sizeof(node.k));
            break;
        case SvgPrimitiveKind_ConvolveMatrix:
            node.kind = PrDrawFilterKind_Convolution;
            node.convolution = PrDrawFilterConvolution{
                .kernel = f.numbers + p.convolve.first,
                .extent = p.convolve.order,
                .divisor = p.convolve.divisor,
                .bias = p.convolve.bias,
                .target = p.convolve.target,
                .edge = p.convolve.edge,
                .preserveAlpha = p.convolve.preserveAlpha,
            };
            break;
        case SvgPrimitiveKind_Lighting: {
            node.kind = PrDrawFilterKind_Lighting;
            PrDrawLight light = p.lighting.light;
            light.position = PrVec3{
                primitivePosition(g, light.position.x, SvgAxis_X),
                primitivePosition(g, light.position.y, SvgAxis_Y),
                length(light.position.z, SvgAxis_Other),
            };
            light.pointsAt = PrVec3{
                primitivePosition(g, light.pointsAt.x, SvgAxis_X),
                primitivePosition(g, light.pointsAt.y, SvgAxis_Y),
                length(light.pointsAt.z, SvgAxis_Other),
            };
            node.lighting = PrDrawFilterLighting{
                .kind = p.lighting.kind,
                .surfaceScale = p.lighting.surfaceScale,
                .constant = p.lighting.constant,
                .specularExponent = p.lighting.specularExponent,
                .color = prColor16FromColor(p.lighting.color),
                .light = light,
            };
            break;
        }
        case SvgPrimitiveKind_DisplacementMap:
            node.kind = PrDrawFilterKind_DisplacementMap;
            node.displacement = PrDrawFilterDisplacement{
                .scale = length(p.displacement.scale, SvgAxis_X),
                .x = p.displacement.x,
                .y = p.displacement.y,
            };
            break;
        case SvgPrimitiveKind_DropShadow:
            node.kind = PrDrawFilterKind_DropShadow;
            node.shadow = PrDrawFilterShadow{
                .offset = PrVec2{length(p.shadow.offset.x, SvgAxis_X), length(p.shadow.offset.y, SvgAxis_Y)},
                .sigma = PrVec2{length(p.shadow.sigma.x, SvgAxis_X), length(p.shadow.sigma.y, SvgAxis_Y)},
                .color = prColor16FromColor(p.shadow.color),
            };
            break;
        case SvgPrimitiveKind_Flood:
            // A flood is the same in either space.
            node.kind = PrDrawFilterKind_Flood;
            node.space = PrColorSpace_Srgb;
            node.flood = prColor16FromColor(p.flood);
            break;
        case SvgPrimitiveKind_GaussianBlur:
            node.kind = PrDrawFilterKind_Blur;
            node.blur = PrDrawFilterBlur{
                .sigma = PrVec2{length(p.blur.sigma.x, SvgAxis_X), length(p.blur.sigma.y, SvgAxis_Y)},
                .edge = p.blur.edge,
            };
            break;
        case SvgPrimitiveKind_Image: {
            // An element draws as a use element would, moved by the subregion's origin. An image fits the subregion as
            // an image element fits its positioning rectangle (filter-effects-1/#feImageElement).
            if (p.image.node == kSvgNodeNone && p.image.picture.raster == nullptr && p.image.picture.svg == nullptr)
                return add(g, transparent(clip));
            return listNode(g, clip, [&](SvgDrawer& inner) {
                if (p.image.node != kSvgNodeNone) {
                    prDrawListPushTransformAffine2(inner.list,
                        prAffine2Translate(PrVec2{subregion.left, subregion.top}));
                    inner.space =
                        prAffine2Concat(prAffine2Translate(PrVec2{subregion.left, subregion.top}), inner.space);
                    svgDrawNode(inner, p.image.node);
                    prDrawListPopTransform(inner.list);
                    return;
                }
                SvgImageShape shape{};
                shape.x = SvgLengthPercentage{subregion.left, 0};
                shape.y = SvgLengthPercentage{subregion.top, 0};
                shape.width = SvgLengthPercentage{prRectWidth(subregion), 0};
                shape.height = SvgLengthPercentage{prRectHeight(subregion), 0};
                shape.clips = true;
                shape.aspect = p.image.aspect;
                shape.fit = PrLayoutObjectFit_Fill;
                shape.sampling = p.image.sampling;
                shape.picture = p.image.picture;
                svgDrawImage(inner, shape);
            });
        }
        case SvgPrimitiveKind_Merge: {
            node.kind = PrDrawFilterKind_Merge;
            RawArray<PrDrawFilterInput> inputs;
            for (uint32_t i = 0; i < p.merge.count; ++i)
                inputs.push(inputNode(g, f.merges[p.merge.first + i]));
            node.merge = PrDrawFilterMerge{.inputs = inputs.data, .inputCount = inputs.count};
            PrDrawFilterInput made = add(g, node);
            inputs.reset();
            return made;
        }
        case SvgPrimitiveKind_Morphology:
            node.kind = PrDrawFilterKind_Morphology;
            node.morphology = PrDrawFilterMorphology{
                .radius = PrVec2{length(p.morphology.radius.x, SvgAxis_X), length(p.morphology.radius.y, SvgAxis_Y)},
                .op = p.morphology.op,
            };
            break;
        case SvgPrimitiveKind_Offset:
            node.kind = PrDrawFilterKind_Offset;
            node.space = PrColorSpace_Srgb;
            node.offset = PrVec2{length(p.offset.x, SvgAxis_X), length(p.offset.y, SvgAxis_Y)};
            break;
        case SvgPrimitiveKind_Tile:
            // The tile is the input's subregion (filter-effects-1/#feTileElement).
            node.kind = PrDrawFilterKind_Tile;
            node.space = PrColorSpace_Srgb;
            node.tile = prRectIntersect(inputSubregion(g, p.inputs[0]), g.region);
            break;
        case SvgPrimitiveKind_Turbulence:
            node.kind = PrDrawFilterKind_Turbulence;
            node.turbulence = p.turbulence;
            break;
        case SvgPrimitiveKind_PassThrough:
            return add(g, copyOf(node.inputs[0], clip));
        default:
            return add(g, transparent(clip));
    }
    return add(g, node);
}

// Returns the region of a filter element in user space, for what has `box` as its object bounding box
// (filter-effects-1/#FilterEffectsRegion).
PrRect regionOf(const SvgDrawer& d, const SvgFilter& f, PrRect box) {
    float x = regionLength(d, f.userSpace, f.x, SvgAxis_X, box.left, prRectWidth(box));
    float y = regionLength(d, f.userSpace, f.y, SvgAxis_Y, box.top, prRectHeight(box));
    float w = regionLength(d, f.userSpace, f.width, SvgAxis_X, 0, prRectWidth(box));
    float h = regionLength(d, f.userSpace, f.height, SvgAxis_Y, 0, prRectHeight(box));
    return PrRect{x, y, x + w, y + h};
}

// Appends the nodes of the filter element `index`, applied to the node `owner` whose bounding box is `box`, reading
// `source` as its SourceGraphic. Writes the node of its result and its region. Returns false if the filter hides the
// element: it has no primitives, its region has no area, or its units are of a bounding box that has none.
bool appendFilter(SvgDrawer& d, const SvgNode& owner, SvgResourceIndex index, PrRect box, PrDrawFilterInput source,
        PrDrawFilterBuilder* builder, PrDrawFilterInput* result, PrRect* region) {
    const SvgFilter& f = d.tree->resources.data[index].filter;
    if (f.primitiveCount == 0)
        return false;
    if ((!f.userSpace || f.primitiveBoundingBox) && prRectIsEmpty(box))
        return false;
    Graph g{
        .d = &d,
        .node = &owner,
        .index = index,
        .filter = &f,
        .builder = builder,
        .box = box,
        .region = regionOf(d, f, box),
        .source = source,
    };
    if (prRectIsEmpty(g.region))
        return false;
    for (PrDrawFilterInput& s : g.sources)
        s = -1;
    for (uint32_t i = 0; i < f.primitiveCount; ++i) {
        const SvgPrimitive& p = f.primitives[i];
        PrRect subregion = subregionOf(g, p);
        PrRect clip = prRectIntersect(subregion, g.region);
        // A subregion with no area disables the primitive, whose result is transparent black.
        PrDrawFilterInput made;
        if (!prRectIsEmpty(subregion) && !prRectIsEmpty(clip))
            made = primitiveNode(g, p, subregion, clip);
        else
            made = add(g, transparent(g.region));
        g.results.push(made);
        g.subregions.push(!prRectIsEmpty(clip) ? clip : PrRect{});
    }
    *result = g.results.last();
    *region = g.region;
    g.results.reset();
    g.subregions.reset();
    return true;
}

} // namespace

void svgBuildFilter(SvgBuilder& b, SvgResourceIndex index, const PrDomElement* e) {
    // The chain of templates through href (SVG11/filters.html#FilterElementHrefAttribute): each attribute comes from
    // the first filter of the chain that has it, and the primitives from the first that has any.
    const PrDomElement* chain[kMaxTemplates];
    uint32_t count = 0;
    for (const PrDomElement* at = e; at != nullptr && svgIs(at, PrDomTag_Filter) && count < kMaxTemplates;
            at = svgBuildHref(b, at, true)) {
        bool seen = false;
        for (uint32_t i = 0; i < count && !seen; ++i)
            seen = chain[i] == at;
        if (seen)
            break;
        chain[count++] = at;
    }
    auto with = [&](PrStringView name) {
        for (uint32_t i = 0; i < count; ++i) {
            if (svgAttribute(chain[i], name).data != nullptr)
                return chain[i];
        }
        return e;
    };
    const CssComputedStyle* style = svgBuildStyle(b, e);
    SvgFilter f{.content = kSvgNodeNone};
    f.userSpace = svgAttribute(with("filterUnits"), "filterUnits") == "userSpaceOnUse";
    f.primitiveBoundingBox = svgAttribute(with("primitiveUnits"), "primitiveUnits") == "objectBoundingBox";
    f.x = svgAttributeLength(with("x"), "x", SvgLengthPercentage{0, -10}, style);
    f.y = svgAttributeLength(with("y"), "y", SvgLengthPercentage{0, -10}, style);
    f.width = svgAttributeLength(with("width"), "width", SvgLengthPercentage{0, 120}, style);
    f.height = svgAttributeLength(with("height"), "height", SvgLengthPercentage{0, 120}, style);
    b.tree->resources.data[index].filter = f;
    const PrDomElement* source = e;
    for (uint32_t i = 0; i < count; ++i) {
        bool primitives = false;
        for (const PrDomNode* child = chain[i]->first; child != nullptr && !primitives; child = child->next) {
            const PrDomElement* element = domElementFromNode(child);
            primitives = element != nullptr && isPrimitive(element);
        }
        if (primitives) {
            source = chain[i];
            break;
        }
    }
    FilterParts parts{};
    for (const PrDomNode* child = source->first; child != nullptr; child = child->next) {
        const PrDomElement* element = domElementFromNode(child);
        if (element == nullptr || !isPrimitive(element))
            continue;
        const CssComputedStyle* primitiveStyle = svgBuildStyle(b, element);
        uint32_t at = parts.primitives.count;
        SvgPrimitive p{};
        p.space = spaceOf(primitiveStyle);
        p.given = subregionOf(element, "x", primitiveStyle, SvgSubregionFlag_X, &p.x) |
            subregionOf(element, "y", primitiveStyle, SvgSubregionFlag_Y, &p.y) |
            subregionOf(element, "width", primitiveStyle, SvgSubregionFlag_Width, &p.width) |
            subregionOf(element, "height", primitiveStyle, SvgSubregionFlag_Height, &p.height);
        SvgFilterInput implicit = at == 0 ? SvgFilterInput_Graphic : SvgFilterInput(at - 1);
        readPrimitive(b, index, parts, element, primitiveStyle, implicit, &p);
        // A displacement map that reads a tainted map passes its input through
        // (filter-effects-1/#fedisplacemnentmap-restrictions).
        if (p.kind == SvgPrimitiveKind_DisplacementMap && taints(parts, p.inputs[1]))
            p.kind = SvgPrimitiveKind_PassThrough;
        parts.tainted.push(isTainted(parts, element, primitiveStyle, p));
        parts.primitives.push(p);
        PrStringView name = svgAttribute(element, "result");
        if (name.size != 0)
            parts.names.push(Named{name, at});
    }
    // The names point into the elements' attributes, which only the build reads.
    SvgFilter& built = b.tree->resources.data[index].filter;
    built.primitiveCount = parts.primitives.count;
    if (parts.primitives.count != 0) {
        built.primitives = static_cast<SvgPrimitive*>(prMemoryAlloc(parts.primitives.count * sizeof(SvgPrimitive)));
        prMemCopy(built.primitives, parts.primitives.data, parts.primitives.count * sizeof(SvgPrimitive));
    }
    if (parts.numbers.count != 0) {
        built.numbers = static_cast<float*>(prMemoryAlloc(parts.numbers.count * sizeof(float)));
        prMemCopy(built.numbers, parts.numbers.data, parts.numbers.count * sizeof(float));
    }
    if (parts.merges.count != 0) {
        built.merges = static_cast<SvgFilterInput*>(prMemoryAlloc(parts.merges.count * sizeof(SvgFilterInput)));
        prMemCopy(built.merges, parts.merges.data, parts.merges.count * sizeof(SvgFilterInput));
    }
    parts.primitives.reset();
    parts.numbers.reset();
    parts.merges.reset();
    parts.names.reset();
    parts.scratch.reset();
    parts.tainted.reset();
}

void svgFilterFree(SvgFilter& filter) {
    for (uint32_t i = 0; i < filter.primitiveCount; ++i) {
        const SvgPrimitive& p = filter.primitives[i];
        if (p.kind == SvgPrimitiveKind_Image) {
            prDrawImageRelease(p.image.picture.raster);
            svgImageRelease(p.image.picture.svg);
        }
    }
    prMemoryFree(filter.primitives);
    prMemoryFree(filter.numbers);
    prMemoryFree(filter.merges);
    filter.primitives = nullptr;
    filter.numbers = nullptr;
    filter.merges = nullptr;
    filter.primitiveCount = 0;
}

bool svgFilterSame(const SvgFilter& a, const SvgFilter& b) {
    auto sameLength = [](SvgLengthPercentage x, SvgLengthPercentage y) {
        return x.px == y.px && x.percent == y.percent;
    };
    if (a.userSpace != b.userSpace || a.primitiveBoundingBox != b.primitiveBoundingBox || !sameLength(a.x, b.x) ||
            !sameLength(a.y, b.y) || !sameLength(a.width, b.width) || !sameLength(a.height, b.height) ||
            a.primitiveCount != b.primitiveCount || (a.content == kSvgNodeNone) != (b.content == kSvgNodeNone))
        return false;
    uint32_t numbers = 0;
    uint32_t merges = 0;
    for (uint32_t i = 0; i < a.primitiveCount; ++i) {
        const SvgPrimitive& p = a.primitives[i];
        const SvgPrimitive& q = b.primitives[i];
        if (p.kind != q.kind || p.space != q.space || p.given != q.given ||
                !prMemEquals(p.inputs, q.inputs, sizeof(p.inputs)) || !sameLength(p.x, q.x) || !sameLength(p.y, q.y) ||
                !sameLength(p.width, q.width) || !sameLength(p.height, q.height))
            return false;
        bool same = true;
        switch (p.kind) {
            case SvgPrimitiveKind_Blend: same = p.blend == q.blend; break;
            case SvgPrimitiveKind_ColorMatrix: same = prMemEquals(&p.matrix, &q.matrix, sizeof(p.matrix)); break;
            case SvgPrimitiveKind_ComponentTransfer:
                for (uint32_t c = 0; c < PrDrawChannel_Count && same; ++c) {
                    const SvgTransfer& s = p.transfer[c];
                    const SvgTransfer& t = q.transfer[c];
                    same = s.kind == t.kind && s.slope == t.slope && s.intercept == t.intercept &&
                        s.amplitude == t.amplitude && s.exponent == t.exponent && s.offset == t.offset && s.count == t.count;
                    numbers = larger(numbers, s.first + s.count);
                }
                break;
            case SvgPrimitiveKind_Composite: same = p.composite == q.composite; break;
            case SvgPrimitiveKind_Arithmetic: same = prMemEquals(p.k, q.k, sizeof(p.k)); break;
            case SvgPrimitiveKind_ConvolveMatrix:
                same = p.convolve.order.x == q.convolve.order.x && p.convolve.order.y == q.convolve.order.y &&
                    p.convolve.target.x == q.convolve.target.x && p.convolve.target.y == q.convolve.target.y &&
                    p.convolve.divisor == q.convolve.divisor && p.convolve.bias == q.convolve.bias &&
                    p.convolve.edge == q.convolve.edge && p.convolve.preserveAlpha == q.convolve.preserveAlpha;
                numbers = larger(numbers, p.convolve.first + p.convolve.order.x * p.convolve.order.y);
                break;
            case SvgPrimitiveKind_Lighting:
                same = p.lighting.kind == q.lighting.kind && p.lighting.surfaceScale == q.lighting.surfaceScale &&
                    p.lighting.constant == q.lighting.constant &&
                    p.lighting.specularExponent == q.lighting.specularExponent &&
                    prMemEquals(&p.lighting.color, &q.lighting.color, sizeof(PrColor)) &&
                    prMemEquals(&p.lighting.light, &q.lighting.light, sizeof(PrDrawLight));
                break;
            case SvgPrimitiveKind_DisplacementMap:
                same = p.displacement.scale == q.displacement.scale && p.displacement.x == q.displacement.x &&
                    p.displacement.y == q.displacement.y;
                break;
            case SvgPrimitiveKind_DropShadow:
                same = prMemEquals(&p.shadow.offset, &q.shadow.offset, sizeof(PrVec2)) &&
                    prMemEquals(&p.shadow.sigma, &q.shadow.sigma, sizeof(PrVec2)) &&
                    prMemEquals(&p.shadow.color, &q.shadow.color, sizeof(PrColor));
                break;
            case SvgPrimitiveKind_Flood: same = prMemEquals(&p.flood, &q.flood, sizeof(PrColor)); break;
            case SvgPrimitiveKind_GaussianBlur:
                same = prMemEquals(&p.blur.sigma, &q.blur.sigma, sizeof(PrVec2)) && p.blur.edge == q.blur.edge;
                break;
            case SvgPrimitiveKind_Image:
                same = p.image.picture.raster == q.image.picture.raster && p.image.picture.svg == q.image.picture.svg &&
                    (p.image.node == kSvgNodeNone) == (q.image.node == kSvgNodeNone) &&
                    p.image.aspect.x == q.image.aspect.x && p.image.aspect.y == q.image.aspect.y &&
                    p.image.aspect.slice == q.image.aspect.slice && p.image.sampling == q.image.sampling;
                break;
            case SvgPrimitiveKind_Merge:
                same = p.merge.count == q.merge.count;
                merges = larger(merges, p.merge.first + p.merge.count);
                break;
            case SvgPrimitiveKind_Morphology:
                same = prMemEquals(&p.morphology.radius, &q.morphology.radius, sizeof(PrVec2)) &&
                    p.morphology.op == q.morphology.op;
                break;
            case SvgPrimitiveKind_Offset: same = prMemEquals(&p.offset, &q.offset, sizeof(PrVec2)); break;
            case SvgPrimitiveKind_Turbulence:
                same = prMemEquals(&p.turbulence.baseFrequency, &q.turbulence.baseFrequency, sizeof(PrVec2)) &&
                    p.turbulence.octaves == q.turbulence.octaves && p.turbulence.seed == q.turbulence.seed &&
                    p.turbulence.kind == q.turbulence.kind && p.turbulence.stitch == q.turbulence.stitch;
                break;
            default: break;
        }
        if (!same)
            return false;
    }
    return (numbers == 0 || prMemEquals(a.numbers, b.numbers, numbers * sizeof(float))) &&
        (merges == 0 || prMemEquals(a.merges, b.merges, merges * sizeof(SvgFilterInput)));
}

bool svgFilterReadsBackdrop(const SvgFilter& filter) {
    auto reads = [](SvgFilterInput input) {
        return input == SvgFilterInput_Background || input == SvgFilterInput_BackgroundAlpha;
    };
    for (uint32_t i = 0; i < filter.primitiveCount; ++i) {
        const SvgPrimitive& p = filter.primitives[i];
        if (p.kind == SvgPrimitiveKind_Merge) {
            for (uint32_t k = 0; k < p.merge.count; ++k) {
                if (reads(filter.merges[p.merge.first + k]))
                    return true;
            }
        } else if (reads(p.inputs[0]) || reads(p.inputs[1])) {
            return true;
        }
    }
    return false;
}

SvgFiltered svgFilterFromNode(SvgDrawer& d, SvgNodeIndex index, SvgFilterGraph* out) {
    const SvgNode& node = d.tree->nodes.data[index];
    if (node.effects == kArrayMissing)
        return SvgFiltered_None;
    const SvgEffects& effects = d.tree->effects.data[node.effects];
    if (effects.filters.count == 0)
        return SvgFiltered_None;
    // A filter element that applies inside its own feImage content, or nested too deep, is in error, and hides the
    // element.
    for (uint32_t i = 0; i < effects.filters.count; ++i) {
        SvgResourceIndex filter = effects.filters.data[i].filter;
        if (effects.filters.data[i].function.filter.kind == PrLayoutFilterKind_Source && svgEffectCycles(d, filter))
            return SvgFiltered_Hidden;
    }
    PrRect box = svgEffectBounds(d, index, 0);
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterInput current = PrDrawFilterInput_Source;
    PrRect region{};
    bool bounded = false;
    for (uint32_t i = 0; i < effects.filters.count; ++i) {
        const SvgFilterItem& item = effects.filters.data[i];
        if (item.function.filter.kind != PrLayoutFilterKind_Source) {
            current = prLayoutFilterAppend(&item.function.filter, current, PrImageEdgeMode_None, builder);
            bounded = false;
            continue;
        }
        if (!appendFilter(d, node, item.filter, box, current, builder, &current, &region)) {
            prDrawFilterBuilderDestroy(builder);
            return SvgFiltered_Hidden;
        }
        bounded = true;
    }
    // The graph draws no further than the region of a filter element that ends the list, and anywhere otherwise.
    out->region = bounded ? region : PrRect{};
    prDrawFilterBuilderSetRegion(builder, out->region);
    out->filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    return SvgFiltered_Filter;
}

bool svgAppendFilter(const SvgTree* tree, SvgResourceIndex index, PrRect box, PrVec2 viewport,
        PrDrawFilterInput input, PrDrawFilterBuilder* builder, PrDrawFilterInput* result, PrRect* region) {
    // What the filter applies to has no node, and so no fill or stroke for FillPaint and StrokePaint.
    SvgNode owner{};
    owner.paints.strokeStyle = kArrayMissing;
    PrDrawList* unused = prDrawListCreate();
    SvgTiles tiles{};
    SvgDrawer d{
        .tree = tree,
        .list = unused,
        .viewport = viewport,
        .scratch = prPathCreate(),
        .space = prAffine2Identity(),
        .tiles = &tiles,
    };
    bool drawn = appendFilter(d, owner, index, box, input, builder, result, region);
    prPathDestroy(d.scratch);
    for (uint32_t i = 0; i < tiles.tiles.count; ++i)
        prDrawListRelease(tiles.tiles.data[i].list);
    tiles.tiles.reset();
    prDrawListRelease(unused);
    return drawn;
}

} // namespace pr_impl
