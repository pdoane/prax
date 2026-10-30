// Draw filters: immutable graphs of image operations, applied to a group's contents or backdrop.

#include "pr/draw_filter.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/vec.h"

#include "draw/src/draw_list.h"
#include "draw/src/identity.h"

namespace pr_impl {

namespace {

// The luminance coefficients of feColorMatrix's matrices.
constexpr float kRed = 0.2126f;
constexpr float kGreen = 0.7152f;
constexpr float kBlue = 0.0722f;

// Returns a matrix of straight color whose rows are `rows`, without an offset, and which keeps alpha.
PrAffine4 colorMatrix(const float rows[3][3]) {
    PrAffine4 m{};
    for (uint32_t r = 0; r < 3; ++r) {
        for (uint32_t c = 0; c < 3; ++c)
            m.m[r][c] = rows[r][c];
    }
    m.m[3][3] = 1;
    return m;
}

// The nodes and the data that they point to. A builder owns them while it builds, and the filter owns them afterward.
struct Graph {
    Array<PrDrawFilterNode> nodes;
    Array<float*> floats;             // tables and kernels, one allocation each
    Array<PrDrawFilterInput*> inputs; // merge inputs
    Array<PrDrawColorStop*> stops;    // progressive blur brushes' stops
    Array<const PrDrawImage*> images; // holds one reference to each image
    Array<const PrDrawList*> lists;
    PrRect region;
};

struct DrawFilterBuilder {
    Graph graph;
};

struct DrawFilter {
    mutable Atomic<uint32_t> references;    // changes behind const, as holders retain and release the immutable filter
    uint64_t id;
    Graph graph;
    PrRect outset;
};

void releaseGraph(Graph& g) {
    for (uint32_t i = 0; i < g.floats.count; ++i)
        prMemoryFree(g.floats.data[i]);
    for (uint32_t i = 0; i < g.inputs.count; ++i)
        prMemoryFree(g.inputs.data[i]);
    for (uint32_t i = 0; i < g.stops.count; ++i)
        prMemoryFree(g.stops.data[i]);
    for (uint32_t i = 0; i < g.images.count; ++i)
        prDrawImageRelease(g.images.data[i]);
    for (uint32_t i = 0; i < g.lists.count; ++i)
        drawListRelease(g.lists.data[i]);
    g.nodes.reset();
    g.floats.reset();
    g.inputs.reset();
    g.stops.reset();
    g.images.reset();
    g.lists.reset();
}

template <typename T>
T* copyOf(const T* items, uint32_t count) {
    T* copy = static_cast<T*>(prMemoryAlloc(sizeof(T) * (count != 0 ? count : 1)));
    memCopy(copy, items, sizeof(T) * count);
    return copy;
}

// The number of entries of `inputs` that the kind reads.
uint32_t inputCount(PrDrawFilterKind kind) {
    switch (kind) {
        case PrDrawFilterKind_Flood:
        case PrDrawFilterKind_Turbulence:
        case PrDrawFilterKind_Image:
        case PrDrawFilterKind_Merge:
            return 0;
        case PrDrawFilterKind_Composite:
        case PrDrawFilterKind_Arithmetic:
        case PrDrawFilterKind_Blend:
        case PrDrawFilterKind_DisplacementMap:
            return 2;
        default:
            return 1;
    }
}

// Takes a reference to the source's image or list, which the graph holds.
void holdSource(Graph& g, const PrDrawSource& source) {
    if (source.kind == PrDrawSourceKind_Image && source.image != nullptr) {
        prDrawImageRetain(source.image);
        g.images.push(source.image);
    } else if (source.kind == PrDrawSourceKind_List && source.list != nullptr) {
        drawListRetain(source.list);
        g.lists.push(source.list);
    }
}

bool validInput(PrDrawFilterInput input, uint32_t index) {
    return (input >= 0 && uint32_t(input) < index) || input == PrDrawFilterInput_Source ||
        input == PrDrawFilterInput_SourceAlpha || input == PrDrawFilterInput_Backdrop;
}

bool finite(float value) {
    return isFiniteF32(value);
}

bool finite(PrVec2 value) {
    return isFiniteF32(value.x) && isFiniteF32(value.y);
}

// Clamps a negative size to zero, and returns true if the size is finite.
bool clampSize(float& value) {
    value = maxF32(value, 0);
    return isFiniteF32(value);
}

bool clampSize(PrVec2& value) {
    bool x = clampSize(value.x);
    bool y = clampSize(value.y);
    return x && y;
}

// Returns how far a node may move the content that it reads, in the group's space.
PrRect nodeOutset(const PrDrawFilterNode& n) {
    auto blurReach = [](PrVec2 sigma) {
        return PrVec2{4 * absF32(sigma.x), 4 * absF32(sigma.y)};
    };
    PrVec2 reach{0, 0};
    PrVec2 shift{0, 0};
    switch (n.kind) {
        case PrDrawFilterKind_Offset:
            shift = n.offset;
            break;
        case PrDrawFilterKind_Blur:
            reach = blurReach(n.blur.sigma);
            break;
        case PrDrawFilterKind_ProgressiveBlur:
            reach = blurReach(PrVec2{n.progressive.sigma, n.progressive.sigma});
            break;
        case PrDrawFilterKind_Morphology:
            reach = n.morphology.radius;
            break;
        case PrDrawFilterKind_Convolution:
            reach = PrVec2{float(n.convolution.extent.x), float(n.convolution.extent.y)};
            break;
        case PrDrawFilterKind_DropShadow:
        case PrDrawFilterKind_InnerShadow:
            reach = blurReach(n.shadow.sigma);
            shift = n.shadow.offset;
            break;
        case PrDrawFilterKind_OuterGlow:
        case PrDrawFilterKind_InnerGlow:
            reach = blurReach(PrVec2{n.glow.sigma, n.glow.sigma});
            reach.x += maxF32(n.glow.spread, 0);
            reach.y += maxF32(n.glow.spread, 0);
            break;
        case PrDrawFilterKind_DisplacementMap:
            reach = PrVec2{absF32(n.displacement.scale) / 2, absF32(n.displacement.scale) / 2};
            break;
        default:
            break;
    }
    return PrRect{
        -reach.x - maxF32(shift.x, 0), -reach.y - maxF32(shift.y, 0),
        reach.x - minF32(shift.x, 0), reach.y - minF32(shift.y, 0),
    };
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrDrawFilterBuilder : DrawFilterBuilder {};
struct PrDrawFilter : DrawFilter {};

PrDrawFilterBuilder* prDrawFilterBuilderCreate(void) {
    return allocZeroed<PrDrawFilterBuilder>();
}

void prDrawFilterBuilderDestroy(PrDrawFilterBuilder* PR_NULLABLE builder) {
    if (builder == nullptr)
        return;
    releaseGraph(builder->graph);
    prMemoryFree(builder);
}

void prDrawFilterBuilderSetRegion(PrDrawFilterBuilder* builder, PrRect region) {
    builder->graph.region = region;
}

PrDrawFilterInput prDrawFilterBuilderAdd(PrDrawFilterBuilder* builder, const PrDrawFilterNode* node) {
    Graph& g = builder->graph;
    uint32_t index = g.nodes.count;
    PR_ASSERT(node->kind < PrDrawFilterKind_Count);
    PR_ASSERT(node->space == PrColorSpace_Srgb || node->space == PrColorSpace_SrgbLinear);
    for (uint32_t i = 0; i < inputCount(node->kind); ++i)
        PR_ASSERT(validInput(node->inputs[i], index));
    PrDrawFilterNode copy = *node;
    bool produces = true;
    switch (copy.kind) {
        case PrDrawFilterKind_ComponentTransfer:
            for (PrDrawTransfer& t : copy.transfer) {
                PR_ASSERT(t.kind <= PrDrawTransferKind_Gamma);
                bool table = t.kind == PrDrawTransferKind_Table || t.kind == PrDrawTransferKind_Discrete;
                if (table && (t.values == nullptr || t.valueCount == 0))
                    t.kind = PrDrawTransferKind_Identity;
            }
            for (PrDrawTransfer& t : copy.transfer) {
                if (t.kind == PrDrawTransferKind_Table || t.kind == PrDrawTransferKind_Discrete) {
                    float* values = copyOf(t.values, t.valueCount);
                    g.floats.push(values);
                    t.values = values;
                } else {
                    t.values = nullptr;
                    t.valueCount = 0;
                }
            }
            break;
        case PrDrawFilterKind_Composite:
            PR_ASSERT(copy.composite < PrDrawComposite_Count);
            break;
        case PrDrawFilterKind_Blend:
            PR_ASSERT(copy.blend < PrDrawBlendMode_Count);
            break;
        case PrDrawFilterKind_Merge: {
            PR_ASSERT(copy.merge.inputCount == 0 || copy.merge.inputs != nullptr);
            for (uint32_t i = 0; i < copy.merge.inputCount; ++i)
                PR_ASSERT(validInput(copy.merge.inputs[i], index));
            PrDrawFilterInput* inputs = copyOf(copy.merge.inputs, copy.merge.inputCount);
            g.inputs.push(inputs);
            copy.merge.inputs = inputs;
            break;
        }
        case PrDrawFilterKind_Offset:
            produces = finite(copy.offset);
            break;
        case PrDrawFilterKind_Blur:
            produces = clampSize(copy.blur.sigma);
            break;
        case PrDrawFilterKind_ProgressiveBlur: {
            PrDrawBrush& brush = copy.progressive.brush;
            produces = clampSize(copy.progressive.sigma);
            for (uint32_t i = 0; brush.stops != nullptr && i < brush.stopCount; ++i)
                PR_ASSERT(brush.stops[i].hint >= 0 && brush.stops[i].hint < 1);
            if (!produces)
                break;
            if (brush.stops != nullptr && brush.stopCount != 0) {
                PrDrawColorStop* stops = copyOf(brush.stops, brush.stopCount);
                g.stops.push(stops);
                brush.stops = stops;
            } else {
                brush.stops = nullptr;
                brush.stopCount = 0;
            }
            if (brush.kind == PrDrawBrushKind_Image)
                holdSource(g, brush.image.source);
            break;
        }
        case PrDrawFilterKind_Morphology:
            produces = clampSize(copy.morphology.radius);
            break;
        case PrDrawFilterKind_Convolution: {
            PrUVec2 extent = copy.convolution.extent;
            PR_ASSERT(copy.convolution.kernel != nullptr && extent.x != 0 && extent.y != 0);
            PR_ASSERT(copy.convolution.target.x < extent.x && copy.convolution.target.y < extent.y);
            produces = extent.x <= kPrDrawConvolutionMaxExtent && extent.y <= kPrDrawConvolutionMaxExtent;
            if (!produces)
                break;
            float* kernel = copyOf(copy.convolution.kernel, extent.x * extent.y);
            g.floats.push(kernel);
            copy.convolution.kernel = kernel;
            // The default divisor is the kernel's sum, and 1 for a sum of zero. Kernels written to sum to zero, as an
            // edge detector's do, sum to a rounding error in floats, which counts as zero.
            if (copy.convolution.divisor == 0) {
                double sum = 0;
                double magnitude = 0;
                for (uint32_t i = 0; i < extent.x * extent.y; ++i) {
                    sum += double(kernel[i]);
                    magnitude += double(absF32(kernel[i]));
                }
                copy.convolution.divisor = absF64(sum) <= magnitude * 1e-6 ? 1.0f : float(sum);
            }
            break;
        }
        case PrDrawFilterKind_Turbulence:
            produces = clampSize(copy.turbulence.baseFrequency) && finite(copy.turbulence.seed);
            if (copy.turbulence.octaves > kPrDrawTurbulenceMaxOctaves)
                copy.turbulence.octaves = kPrDrawTurbulenceMaxOctaves;
            break;
        case PrDrawFilterKind_Tile:
            produces = !prRectIsEmpty(copy.tile);
            break;
        case PrDrawFilterKind_Image:
            produces = prDrawSourceIsValid(copy.image.source);
            if (produces)
                holdSource(g, copy.image.source);
            break;
        case PrDrawFilterKind_DropShadow:
        case PrDrawFilterKind_InnerShadow:
            produces = clampSize(copy.shadow.sigma) && finite(copy.shadow.offset);
            break;
        case PrDrawFilterKind_OuterGlow:
        case PrDrawFilterKind_InnerGlow:
            produces = clampSize(copy.glow.sigma) && finite(copy.glow.spread);
            break;
        case PrDrawFilterKind_Feather:
            produces = clampSize(copy.feather);
            break;
        case PrDrawFilterKind_DisplacementMap:
            PR_ASSERT(copy.displacement.x <= PrDrawChannel_A && copy.displacement.y <= PrDrawChannel_A);
            produces = finite(copy.displacement.scale);
            break;
        case PrDrawFilterKind_Lighting:
            PR_ASSERT(copy.lighting.light.kind <= PrDrawLightKind_Spot);
            break;
        default:
            break;
    }
    if (!produces) {
        PrRect region = copy.region;
        copy = PrDrawFilterNode{};
        copy.kind = PrDrawFilterKind_Flood;
        copy.space = node->space;
        copy.region = region;
    }
    g.nodes.push(copy);
    return PrDrawFilterInput(index);
}

const PrDrawFilter* prDrawFilterCreate(PrDrawFilterBuilder* builder) {
    DrawFilterBuilder* b = builder;
    PR_ASSERT(b->graph.nodes.count != 0);
    PrDrawFilter* filter = allocZeroed<PrDrawFilter>();
    filter->references.storeRelaxed(1);
    filter->id = drawNextId();
    filter->graph = b->graph;
    memZero(&b->graph, sizeof(Graph));
    // How far the result may read from its inputs. This is the sum of each node's reach along the longest chain, which
    // the sum of every node's reach bounds from above.
    PrRect outset{0, 0, 0, 0};
    for (uint32_t i = 0; i < filter->graph.nodes.count; ++i) {
        PrRect o = nodeOutset(filter->graph.nodes.data[i]);
        outset = PrRect{outset.left + o.left, outset.top + o.top, outset.right + o.right, outset.bottom + o.bottom};
    }
    filter->outset = outset;
    return filter;
}

void prDrawFilterRetain(const PrDrawFilter* filter) {
    filter->references.fetchAdd(1);
}

void prDrawFilterRelease(const PrDrawFilter* PR_NULLABLE filter) {
    if (filter == nullptr || filter->references.fetchSub(1) != 1)
        return;
    PrDrawFilter* last = const_cast<PrDrawFilter*>(filter);
    releaseGraph(last->graph);
    prMemoryFree(last);
}

uint32_t prDrawFilterNodeCount(const PrDrawFilter* filter) {
    return filter->graph.nodes.count;
}

const PrDrawFilterNode* prDrawFilterNodes(const PrDrawFilter* filter) {
    return filter->graph.nodes.data;
}

PrRect prDrawFilterRegion(const PrDrawFilter* filter) {
    return filter->graph.region;
}

PrRect prDrawFilterOutset(const PrDrawFilter* filter) {
    return filter->outset;
}

uint64_t prDrawFilterId(const PrDrawFilter* filter) {
    return filter->id;
}

PrAffine4 prDrawColorMatrixSaturate(float amount) {
    float s = amount;
    const float rows[3][3] = {
        {kRed + (1 - kRed) * s, kGreen - kGreen * s, kBlue - kBlue * s},
        {kRed - kRed * s, kGreen + (1 - kGreen) * s, kBlue - kBlue * s},
        {kRed - kRed * s, kGreen - kGreen * s, kBlue + (1 - kBlue) * s},
    };
    return colorMatrix(rows);
}

PrAffine4 prDrawColorMatrixHueRotate(float radians) {
    float c = cosF32(radians);
    float s = sinF32(radians);
    const float rows[3][3] = {
        {kRed + c * (1 - kRed) - s * kRed, kGreen - c * kGreen - s * kGreen, kBlue - c * kBlue + s * (1 - kBlue)},
        {kRed - c * kRed + s * 0.143f, kGreen + c * (1 - kGreen) + s * 0.140f, kBlue - c * kBlue - s * 0.283f},
        {kRed - c * kRed - s * (1 - kRed), kGreen - c * kGreen + s * kGreen, kBlue + c * (1 - kBlue) + s * kBlue},
    };
    return colorMatrix(rows);
}

PrAffine4 prDrawColorMatrixLuminanceToAlpha(void) {
    PrAffine4 m{};
    m.m[3][0] = kRed;
    m.m[3][1] = kGreen;
    m.m[3][2] = kBlue;
    return m;
}
