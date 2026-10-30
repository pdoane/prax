// Feature drawings for filters and backdrops.

#include "tools/viewer/features_filters.h"

#include "tools/viewer/drawing.h"

namespace viewer {
namespace {

// Returns a filter of the nodes. A node takes the previous node as its input if its input is -1 and its kind reads an
// input. The list keeps its own reference, so the caller releases the filter once it is pushed.
const PrDrawFilter* filterOf(std::initializer_list<PrDrawFilterNode> nodes, PrRect region = {}) {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    prDrawFilterBuilderSetRegion(builder, region);
    for (const PrDrawFilterNode& node : nodes)
        prDrawFilterBuilderAdd(builder, &node);
    const PrDrawFilter* filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    return filter;
}

PrDrawFilterNode nodeOf(PrDrawFilterKind kind, int32_t input = PrDrawFilterInput_Source,
        int32_t second = PrDrawFilterInput_Source) {
    PrDrawFilterNode node{};
    node.kind = kind;
    node.space = PrColorSpace_Srgb;
    node.inputs[0] = input;
    node.inputs[1] = second;
    return node;
}

// Pushes a group filtered by `filter`, bounded to the cell, and releases the caller's reference.
void pushFiltered(PrDrawList* list, PrRect cell, const PrDrawFilter* filter) {
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = cell;
    group.filter = filter;
    prDrawListPushGroup(list, &group);
    prDrawFilterRelease(filter);
}

// Draws a small scene for filters to act on: shapes in several colors, on transparency.
void subject(PrDrawList* list, PrRect c) {
    PrVec2 center = prRectCenter(c);
    float r = extentOf(c) * 0.28f;
    prDrawListEllipse(list, PrVec2{center.x - r * 0.5f, center.y - r * 0.3f}, PrVec2{r, r}, 0, {.color = kRed});
    prDrawListRect(list, PrRect{
        center.x, center.y - r * 0.2f, center.x + r * 1.1f,
        center.y + r * 0.9f,
    }, {.color = kBlue});
    PrPath* path = star(PrVec2{center.x - r * 0.3f, center.y + r * 0.6f}, r * 0.6f, 5, 2);
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = kGold});
    prPathDestroy(path);
}

void blursAndShadows(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 4, 2};
    PrDrawFilterNode blur = nodeOf(PrDrawFilterKind_Blur);
    blur.blur.sigma = PrVec2{6, 6};
    PrRect c = grid.at(0);
    pushFiltered(list, c, filterOf({blur}));
    subject(list, c);
    prDrawListPopGroup(list);

    blur.blur.sigma = PrVec2{16, 1};
    c = grid.at(1);
    pushFiltered(list, c, filterOf({blur}));
    subject(list, c);
    prDrawListPopGroup(list);

    PrDrawFilterNode drop = nodeOf(PrDrawFilterKind_DropShadow);
    drop.shadow.offset = PrVec2{8, 10};
    drop.shadow.sigma = PrVec2{5, 5};
    drop.shadow.color = withAlpha(kInk, 0.6f);
    c = grid.at(2);
    pushFiltered(list, c, filterOf({drop}));
    subject(list, c);
    prDrawListPopGroup(list);

    PrDrawFilterNode innerShadow = nodeOf(PrDrawFilterKind_InnerShadow);
    innerShadow.shadow.offset = PrVec2{5, 6};
    innerShadow.shadow.sigma = PrVec2{4, 4};
    innerShadow.shadow.color = withAlpha(kInk, 0.8f);
    c = grid.at(3);
    pushFiltered(list, c, filterOf({innerShadow}));
    subject(list, c);
    prDrawListPopGroup(list);

    PrDrawFilterNode outer = nodeOf(PrDrawFilterKind_OuterGlow);
    outer.glow.sigma = 8;
    outer.glow.spread = 3;
    outer.glow.color = kGold;
    c = grid.at(4);
    prDrawListRect(list, c, {.color = kInk});
    pushFiltered(list, c, filterOf({outer}));
    subject(list, c);
    prDrawListPopGroup(list);

    PrDrawFilterNode innerGlow = nodeOf(PrDrawFilterKind_InnerGlow);
    innerGlow.glow.sigma = 6;
    innerGlow.glow.spread = 1;
    innerGlow.glow.color = kWhite;
    c = grid.at(5);
    pushFiltered(list, c, filterOf({innerGlow}));
    subject(list, c);
    prDrawListPopGroup(list);

    PrDrawFilterNode feather = nodeOf(PrDrawFilterKind_Feather);
    feather.feather = 24;
    c = grid.at(6);
    pushFiltered(list, c, filterOf({feather}));
    drawImage(list, testCard(), PrRect{0, 0, 64, 64}, inset(c, 20), PrDrawSampling_Linear, 1);
    prDrawListPopGroup(list);

    // A blur with a different sigma on each axis, in a rotated space. It blurs along the space's axes.
    c = grid.at(7);
    PrVec2 center = prRectCenter(c);
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.5f), prAffine2Translate(center)));
    blur.blur.sigma = PrVec2{12, 0};
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{-120, -120, 120, 120};
    group.filter = filterOf({blur});
    prDrawListPushGroup(list, &group);
    prDrawFilterRelease(group.filter);
    prDrawListRect(list, PrRect{-50, -50, 50, 50}, {.color = kPurple});
    prDrawListPopGroup(list);
    prDrawListPopTransform(list);
    const char* captions[] = {
        "blur 6",         "blur 16 by 1",  "drop shadow",
        "inner shadow",   "outer glow",    "inner glow",
        "feather",        "blur along x, rotated half a radian",
    };
    for (uint32_t i = 0; i < 8; ++i)
        grid.captionAt(i, captions[i]);
}

void colorFilters(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 4, 2};
    PrRect whole{0, 0, 64, 64};
    // Grayscale, sepia and a channel swap by color matrices.
    const float gray[20] = {
        0.2126f, 0.7152f, 0.0722f, 0, 0, 0.2126f, 0.7152f, 0.0722f, 0, 0,
        0.2126f, 0.7152f, 0.0722f, 0, 0, 0,       0,       0,       1, 0,
    };
    const float sepia[20] = {
        0.393f, 0.769f, 0.189f, 0, 0, 0.349f, 0.686f, 0.168f, 0, 0,
        0.272f, 0.534f, 0.131f, 0, 0, 0,      0,      0,      1, 0,
    };
    const float swap[20] = {0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0};
    const float* matrices[] = {gray, sepia, swap};
    for (uint32_t i = 0; i < 3; ++i) {
        PrDrawFilterNode node = nodeOf(PrDrawFilterKind_ColorMatrix);
        for (uint32_t k = 0; k < 20; ++k)
            node.matrix.m[k / 5][k % 5] = matrices[i][k];
        PrRect c = grid.at(i, 20);
        pushFiltered(list, c, filterOf({node}));
        drawImage(list, testCard(), whole, c, PrDrawSampling_Linear, 1);
        prDrawListPopGroup(list);
    }
    // Posterized by discrete steps, inverted by a table, and brightened by a gamma.
    const float steps[] = {0, 0.33f, 0.66f, 1};
    const float invert[] = {1, 0};
    PrDrawFilterNode node = nodeOf(PrDrawFilterKind_ComponentTransfer);
    for (uint32_t k = 0; k < 3; ++k)
        node.transfer[k] = PrDrawTransfer{PrDrawTransferKind_Discrete, steps, 4, 0, 0, 0, 0, 0};
    PrRect c = grid.at(3, 20);
    pushFiltered(list, c, filterOf({node}));
    drawImage(list, testCard(), whole, c, PrDrawSampling_Linear, 1);
    prDrawListPopGroup(list);
    for (uint32_t k = 0; k < 3; ++k)
        node.transfer[k] = PrDrawTransfer{PrDrawTransferKind_Table, invert, 2, 0, 0, 0, 0, 0};
    c = grid.at(4, 20);
    pushFiltered(list, c, filterOf({node}));
    drawImage(list, testCard(), whole, c, PrDrawSampling_Linear, 1);
    prDrawListPopGroup(list);
    for (uint32_t k = 0; k < 3; ++k)
        node.transfer[k] = PrDrawTransfer{PrDrawTransferKind_Gamma, nullptr, 0, 0, 0, 1, 0.5f, 0};
    c = grid.at(5, 20);
    pushFiltered(list, c, filterOf({node}));
    drawImage(list, testCard(), whole, c, PrDrawSampling_Linear, 1);
    prDrawListPopGroup(list);
    // A flood composited in by the source's alpha, and the source blended with a flood.
    c = grid.at(6, 20);
    PrDrawFilterNode flood = nodeOf(PrDrawFilterKind_Flood);
    flood.flood = kTeal;
    PrDrawFilterNode in = nodeOf(PrDrawFilterKind_Composite, 0, PrDrawFilterInput_Source);
    in.composite = PrDrawComposite_SourceIn;
    pushFiltered(list, c, filterOf({flood, in}));
    subject(list, c);
    prDrawListPopGroup(list);
    c = grid.at(7, 20);
    PrDrawFilterNode stripes = nodeOf(PrDrawFilterKind_Flood);
    stripes.flood = kGold;
    PrDrawFilterNode blend = nodeOf(PrDrawFilterKind_Blend, PrDrawFilterInput_Source, 0);
    blend.blend = PrDrawBlendMode_Multiply;
    pushFiltered(list, c, filterOf({stripes, blend}));
    drawImage(list, testCard(), whole, c, PrDrawSampling_Linear, 1);
    prDrawListPopGroup(list);
    const char* captions[] = {
        "grayscale matrix",
        "sepia matrix",
        "channels swapped",
        "posterized by discrete steps",
        "inverted by a table",
        "gamma 0.5",
        "a flood kept to the source's alpha",
        "a gold flood multiplied under it",
    };
    for (uint32_t i = 0; i < 8; ++i)
        grid.captionAt(i, captions[i], 20);
}

void generatedFilters(PrDrawList* list, float width, float height, double) {
    Grid grid{width, height, 4, 2};
    // Turbulence and fractal noise.
    for (uint32_t i = 0; i < 2; ++i) {
        PrRect c = grid.at(i, 20);
        PrDrawFilterNode noise = nodeOf(PrDrawFilterKind_Turbulence);
        noise.turbulence.baseFrequency = PrVec2{0.02f, 0.03f};
        noise.turbulence.octaves = 4;
        noise.turbulence.seed = 7;
        noise.turbulence.kind = i == 1 ? PrDrawTurbulenceKind_FractalNoise : PrDrawTurbulenceKind_Turbulence;
        pushFiltered(list, c, filterOf({noise}));
        prDrawListRect(list, c, {.color = kWhite});
        prDrawListPopGroup(list);
    }
    // The test card displaced by noise.
    PrRect c = grid.at(2, 20);
    PrDrawFilterNode noise = nodeOf(PrDrawFilterKind_Turbulence);
    noise.turbulence.baseFrequency = PrVec2{0.03f, 0.03f};
    noise.turbulence.octaves = 2;
    noise.turbulence.seed = 3;
    PrDrawFilterNode displace = nodeOf(PrDrawFilterKind_DisplacementMap, PrDrawFilterInput_Source, 0);
    displace.displacement.scale = 30;
    displace.displacement.x = PrDrawChannel_R;
    displace.displacement.y = PrDrawChannel_G;
    pushFiltered(list, c, filterOf({noise, displace}));
    drawImage(list, testCard(), PrRect{0, 0, 64, 64}, inset(c, 10), PrDrawSampling_Linear, 1);
    prDrawListPopGroup(list);
    // Erode and dilate.
    c = grid.at(3, 20);
    PrDrawFilterNode erode = nodeOf(PrDrawFilterKind_Morphology);
    erode.morphology.radius = PrVec2{4, 4};
    PrDrawFilterNode dilate = nodeOf(PrDrawFilterKind_Morphology);
    dilate.morphology.radius = PrVec2{3, 3};
    dilate.morphology.op = PrDrawMorphologyOp_Dilate;
    PrDrawFilterNode merge = nodeOf(PrDrawFilterKind_Merge);
    merge.merge.inputCount = 2;
    PrDrawFilterNode tinted = nodeOf(PrDrawFilterKind_ColorMatrix, 1);
    const float half[20] = {0.5f, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0, 1, 0};
    for (uint32_t k = 0; k < 20; ++k)
        tinted.matrix.m[k / 5][k % 5] = half[k];
    const int32_t order[] = {2, 0};
    merge.merge.inputs = order;
    pushFiltered(list, c, filterOf({erode, dilate, tinted, merge}));
    subject(list, c);
    prDrawListPopGroup(list);
    // Edge detection and emboss by convolution.
    const float edges[9] = {-1, -1, -1, -1, 8, -1, -1, -1, -1};
    const float emboss[9] = {-2, -1, 0, -1, 1, 1, 0, 1, 2};
    const float* kernels[] = {edges, emboss};
    for (uint32_t i = 0; i < 2; ++i) {
        c = grid.at(4 + i, 20);
        PrDrawFilterNode convolve = nodeOf(PrDrawFilterKind_Convolution);
        convolve.convolution.kernel = kernels[i];
        convolve.convolution.extent.x = 3;
        convolve.convolution.extent.y = 3;
        convolve.convolution.divisor = 1;
        convolve.convolution.target.x = 1;
        convolve.convolution.target.y = 1;
        convolve.convolution.edge = PrImageEdgeMode_Duplicate;
        convolve.convolution.preserveAlpha = true;
        pushFiltered(list, c, filterOf({convolve}));
        drawImage(list, testCard(), PrRect{0, 0, 64, 64}, c, PrDrawSampling_Linear, 1);
        prDrawListPopGroup(list);
    }
    // Diffuse and specular light on the subject's alpha.
    for (uint32_t i = 0; i < 2; ++i) {
        c = grid.at(6 + i, 20);
        PrDrawFilterNode soften = nodeOf(PrDrawFilterKind_Blur, PrDrawFilterInput_SourceAlpha);
        soften.blur.sigma = PrVec2{3, 3};
        PrDrawFilterNode lit = nodeOf(PrDrawFilterKind_Lighting, 0);
        lit.lighting.kind = i == 1 ? PrDrawLightingKind_Specular : PrDrawLightingKind_Diffuse;
        lit.lighting.surfaceScale = 5;
        lit.lighting.constant = 1;
        lit.lighting.specularExponent = 20;
        lit.lighting.color = kWhite;
        lit.lighting.light.kind = i == 0 ? PrDrawLightKind_Distant : PrDrawLightKind_Point;
        lit.lighting.light.azimuth = kPi * 1.25f;
        lit.lighting.light.elevation = kPi / 5;
        lit.lighting.light.position = PrVec3{c.left + 60, c.top + 60, 120};
        // Diffuse light multiplies the source. Specular light adds to it, kept to its shape.
        PrDrawFilterNode product = nodeOf(PrDrawFilterKind_Arithmetic, PrDrawFilterInput_Source, 1);
        product.k[0] = 1;
        PrDrawFilterNode sum = nodeOf(PrDrawFilterKind_Arithmetic, PrDrawFilterInput_Source, 1);
        sum.k[1] = 1;
        sum.k[2] = 1;
        PrDrawFilterNode cut = nodeOf(PrDrawFilterKind_Composite, 2, PrDrawFilterInput_Source);
        cut.composite = PrDrawComposite_SourceIn;
        if (i == 0)
            pushFiltered(list, c, filterOf({soften, lit, product}));
        else
            pushFiltered(list, c, filterOf({soften, lit, sum, cut}));
        subject(list, c);
        prDrawListPopGroup(list);
    }
    const char* captions[] = {
        "turbulence",
        "fractal noise",
        "displaced by noise",
        "eroded over dilated",
        "edges by convolution",
        "emboss by convolution",
        "diffuse light, distant",
        "specular light, point",
    };
    for (uint32_t i = 0; i < 8; ++i)
        grid.captionAt(i, captions[i], 20);
}

void backdrops(PrDrawList* list, float width, float height, double) {
    // A busy background, and panels over it that blur what lies beneath.
    for (uint32_t i = 0; i < 40; ++i) {
        float x = float((i * 97) % 1000) + 30;
        float y = float((i * 53) % 700) + 40;
        PrColor colors[] = {kRed, kBlue, kGreen, kGold, kPurple, kTeal};
        prDrawListEllipse(list, PrVec2{x, y}, PrVec2{30 + float(i % 5) * 8, 30 + float(i % 3) * 10}, 0,
            {.color = colors[i % 6]});
    }
    PrDrawFilterNode blur = nodeOf(PrDrawFilterKind_Blur);
    blur.blur.sigma = PrVec2{10, 10};
    blur.blur.edge = PrImageEdgeMode_Duplicate;
    PrRect panel{80, 80, width / 2 - 40, height / 2 - 20};
    const PrDrawFilter* frosted = filterOf({blur});
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = panel;
    group.backdrop = frosted;
    prDrawListPushClipCornerRect(list, prCornerRectRound(panel, 24), PrDrawEdges_Antialiased);
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, panel, {.color = withAlpha(kWhite, 0.35f)});
    prDrawListPopGroup(list);
    prDrawListPopClip(list);
    prDrawFilterRelease(frosted);
    // A backdrop darkened and desaturated by a color matrix.
    PrRect second{width / 2 + 20, 80, width - 80, height / 2 - 20};
    PrDrawFilterNode dim = nodeOf(PrDrawFilterKind_ColorMatrix);
    const float matrix[20] = {
        0.15f, 0.3f, 0.05f, 0, 0, 0.15f, 0.3f, 0.05f, 0, 0,
        0.15f, 0.3f, 0.05f, 0, 0, 0,     0,    0,     1, 0,
    };
    for (uint32_t k = 0; k < 20; ++k)
        dim.matrix.m[k / 5][k % 5] = matrix[k];
    group.bounds = second;
    group.backdrop = filterOf({dim});
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, inset(second, 40), {.color = kGold});
    prDrawListPopGroup(list);
    prDrawFilterRelease(group.backdrop);
    // A progressive blur: sharp at the top of the region, blurred at its bottom.
    PrRect third{80, height / 2 + 20, width - 80, height - 60};
    PrDrawFilterNode progressive = nodeOf(PrDrawFilterKind_ProgressiveBlur);
    progressive.progressive.sigma = 14;
    const PrDrawColorStop ramp[] = {{.offset = 0, .color = withAlpha(kWhite, 0)}, {.offset = 1, .color = kWhite}};
    progressive.progressive.brush = prDrawBrushLinear(PrVec2{0, third.top}, PrVec2{0, third.bottom}, ramp, 2);
    group.bounds = third;
    group.backdrop = filterOf({progressive});
    prDrawListPushGroup(list, &group);
    prDrawListPopGroup(list);
    prDrawFilterRelease(group.backdrop);
    caption(PrRect{panel.left, panel.bottom - 22, panel.right, panel.bottom - 4}, "frosted: blurred under a translucent panel");
    caption(PrRect{second.left, second.bottom - 22, second.right, second.bottom - 4}, "dimmed to gray under a gold square");
    caption(PrRect{third.left, third.bottom + 4, third.right, third.bottom + 22}, "a progressive blur, growing toward the bottom");
}

} // namespace

const std::vector<Drawing>& filterFeatures() {
    static const std::vector<Drawing> features = {
        {"Filters: blurs and shadows", "Blurs, shadows, glows and feathering on a group.", blursAndShadows},
        {"Filters: color", "Color matrices, component transfers, floods and blends.", colorFilters},
        {
            "Filters: generated and distorted",
            "Noise, displacement, morphology, convolution and lighting.", generatedFilters,
        },
        {"Backdrops", "Filters on what lies beneath a group.", backdrops},
    };
    return features;
}

} // namespace viewer
