// Draw filters: immutable graphs of image operations, applied to a group's contents or backdrop.
//
// Lengths and positions are in the group's space.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw_list.h"
#include "pr/geom.h"
#include "pr/image_process.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Builds a filter graph node by node. Each node reads earlier nodes or the group's inputs, and the last node is the
// result.
typedef struct PrDrawFilterBuilder PrDrawFilterBuilder;

// The operation that a node performs.
enum {
    PrDrawFilterKind_Offset,
    PrDrawFilterKind_Flood,
    PrDrawFilterKind_ColorMatrix,
    PrDrawFilterKind_ComponentTransfer,
    PrDrawFilterKind_Composite,         // inputs[0] composited over inputs[1] by an operator
    PrDrawFilterKind_Arithmetic,        // k1 i1 i2 + k2 i1 + k3 i2 + k4, premultiplied
    PrDrawFilterKind_Blend,             // inputs[0] blended onto inputs[1]
    PrDrawFilterKind_Merge,             // the merge inputs composited source-over in order, first at the bottom
    PrDrawFilterKind_Blur,
    PrDrawFilterKind_ProgressiveBlur,
    PrDrawFilterKind_Morphology,
    PrDrawFilterKind_Convolution,
    PrDrawFilterKind_Turbulence,
    PrDrawFilterKind_Image,
    PrDrawFilterKind_Tile,              // a part of inputs[0], repeated across the region
    PrDrawFilterKind_DropShadow,        // the input over its shadow
    PrDrawFilterKind_InnerShadow,       // the input with a shadow inside its alpha
    PrDrawFilterKind_OuterGlow,         // the input over a glow outside it
    PrDrawFilterKind_InnerGlow,         // the input with a glow inside its edges
    PrDrawFilterKind_Feather,           // the input, with its alpha softened inward from its edges
    PrDrawFilterKind_DisplacementMap,   // inputs[0], unconverted, displaced by the channels of inputs[1]
    PrDrawFilterKind_Lighting,          // light on a surface whose height is the alpha of inputs[0]
    PrDrawFilterKind_Count,
};
typedef uint8_t PrDrawFilterKind;

// An input of a node: an earlier node's index, or one of the group's inputs below.
enum {
    PrDrawFilterInput_Source = -1,          // the group's contents
    PrDrawFilterInput_SourceAlpha = -2,     // the alpha of the contents, as black
    PrDrawFilterInput_Backdrop = -3,        // what lies beneath
};
typedef int32_t PrDrawFilterInput;

// The function that a component transfer applies to a channel's value c.
enum {
    PrDrawTransferKind_Identity,
    PrDrawTransferKind_Table,       // piecewise linear through the values at even steps
    PrDrawTransferKind_Discrete,    // a step function of the values
    PrDrawTransferKind_Linear,      // slope c + intercept
    PrDrawTransferKind_Gamma,       // amplitude c^exponent + offset
};
typedef uint8_t PrDrawTransferKind;

// A component transfer function. Its kind decides which fields it uses.
typedef struct PrDrawTransfer {
    PrDrawTransferKind kind;
    const float* PR_NULLABLE values;    // Table, Discrete
    uint32_t valueCount;
    float slope;                        // Linear
    float intercept;                    // Linear
    float amplitude;                    // Gamma
    float exponent;                     // Gamma
    float offset;                       // Gamma
} PrDrawTransfer;

// The channels of a color.
enum {
    PrDrawChannel_R,
    PrDrawChannel_G,
    PrDrawChannel_B,
    PrDrawChannel_A,
    PrDrawChannel_Count,
};
typedef uint8_t PrDrawChannel;

// The value that a Morphology node selects from the pixels within its radius.
enum {
    PrDrawMorphologyOp_Erode,       // each channel's smallest value
    PrDrawMorphologyOp_Dilate,      // each channel's largest value
};
typedef uint8_t PrDrawMorphologyOp;

// The noise that a Turbulence node computes, as SVG's feTurbulence type names it.
enum {
    PrDrawTurbulenceKind_Turbulence,
    PrDrawTurbulenceKind_FractalNoise,
};
typedef uint8_t PrDrawTurbulenceKind;

// The reflection that a Lighting node computes, as defined by SVG's feDiffuseLighting and feSpecularLighting.
enum {
    PrDrawLightingKind_Diffuse,
    PrDrawLightingKind_Specular,
};
typedef uint8_t PrDrawLightingKind;

// The kinds of light that a Lighting node casts.
enum {
    PrDrawLightKind_Distant,
    PrDrawLightKind_Point,
    PrDrawLightKind_Spot,
};
typedef uint8_t PrDrawLightKind;

// A light that a Lighting node casts. Its kind decides which fields it uses.
typedef struct PrDrawLight {
    PrDrawLightKind kind;
    float azimuth;              // Distant: radians
    float elevation;            // Distant: radians
    PrVec3 position;            // Point, Spot
    PrVec3 pointsAt;            // Spot: the point that the light aims at
    float spotExponent;         // Spot
    float coneAngle;            // Spot: radians, or zero for no cone
} PrDrawLight;

// The maximum number of octaves of turbulence that a node computes. A node that asks for more computes this many.
enum { kPrDrawTurbulenceMaxOctaves = 16 };

// The maximum number of columns or rows in a Convolution node's kernel.
enum { kPrDrawConvolutionMaxExtent = 64 };

// A Merge node's inputs, composited in order with source-over.
typedef struct PrDrawFilterMerge {
    const PrDrawFilterInput* inputs;
    uint32_t inputCount;
} PrDrawFilterMerge;

// A Blur node's sigma, and how it reads past the region of its input.
typedef struct PrDrawFilterBlur {
    PrVec2 sigma;
    PrImageEdgeMode edge;
} PrDrawFilterBlur;

// A ProgressiveBlur node's blur, which varies over the node with a brush's alpha.
typedef struct PrDrawFilterProgressiveBlur {
    float sigma;            // the largest sigma, used where the brush is opaque
    PrDrawBrush brush;      // the brush's alpha scales the sigma
} PrDrawFilterProgressiveBlur;

// A Morphology node's radius on each axis, and whether it erodes or dilates.
typedef struct PrDrawFilterMorphology {
    PrVec2 radius;
    PrDrawMorphologyOp op;
} PrDrawFilterMorphology;

// A Convolution node's kernel, and how it applies the kernel at each pixel.
typedef struct PrDrawFilterConvolution {
    const float* kernel;    // `extent.y` rows of `extent.x` columns
    PrUVec2 extent;
    float divisor;          // zero for the kernel's sum, or 1 if the sum is zero within rounding
    float bias;             // added to each channel: premultiplied, or straight with preserveAlpha
    PrUVec2 target;         // the kernel element over the pixel that the kernel computes
    PrImageEdgeMode edge;
    bool preserveAlpha;
} PrDrawFilterConvolution;

// A Turbulence node's noise, a function of the point in the group's space. Stitched noise tiles the node's region, or
// the graph's if the node has none.
typedef struct PrDrawFilterTurbulence {
    PrVec2 baseFrequency;
    uint32_t octaves;       // clamped to kPrDrawTurbulenceMaxOctaves
    float seed;
    PrDrawTurbulenceKind kind;
    bool stitch;
} PrDrawFilterTurbulence;

// An Image node's source, and where the node draws it.
typedef struct PrDrawFilterImage {
    PrDrawSource source;
    PrRect rect;            // the area that the image is drawn to
    PrDrawSampling sampling;
} PrDrawFilterImage;

// A DropShadow or InnerShadow node's shadow.
typedef struct PrDrawFilterShadow {
    PrVec2 offset;
    PrVec2 sigma;
    PrColor16 color;            // straight alpha
} PrDrawFilterShadow;

// An OuterGlow or InnerGlow node's glow.
typedef struct PrDrawFilterGlow {
    float sigma;
    float spread;           // how far the alpha grows before blurring
    PrColor16 color;        // straight alpha
} PrDrawFilterGlow;

// A DisplacementMap node's scale, and the channels of its second input that displace each axis. The node reads the
// channels in its space, and moves the pixels of its first input without converting them.
typedef struct PrDrawFilterDisplacement {
    float scale;
    PrDrawChannel x;
    PrDrawChannel y;
} PrDrawFilterDisplacement;

// A Lighting node's surface and its light.
typedef struct PrDrawFilterLighting {
    PrDrawLightingKind kind;
    float surfaceScale;
    float constant;             // the diffuse or specular constant
    float specularExponent;     // Specular
    PrColor16 color;            // straight alpha
    PrDrawLight light;
} PrDrawFilterLighting;

// A node. Its kind decides which member of the union it uses. A kind with a sigma blurs by a Gaussian of that sigma on
// each axis, which a backend may approximate.
//
// The region of an input is the region of the node that made it, within the graph's, or the graph's region for the
// group's inputs. A node with an edge mode reads past its input's region by that mode.
typedef struct PrDrawFilterNode {
    PrDrawFilterKind kind;
    PrColorSpace space;         // the space that the node computes in: sRGB or sRGB-linear
    PrDrawFilterInput inputs[2];
    PrRect region;              // where the node produces output in the group's space, or empty for everywhere
    union {
        PrVec2 offset;                                // Offset
        PrColor16 flood;                              // Flood: straight alpha
        PrAffine4 matrix;                             // ColorMatrix: of straight R, G, B, A
        PrDrawTransfer transfer[PrDrawChannel_Count]; // ComponentTransfer: of straight channels
        PrDrawComposite composite;                    // Composite
        float k[4];                                   // Arithmetic
        PrDrawBlendMode blend;                        // Blend
        PrDrawFilterMerge merge;                      // Merge
        PrDrawFilterBlur blur;                        // Blur
        PrDrawFilterProgressiveBlur progressive;      // ProgressiveBlur
        PrDrawFilterMorphology morphology;            // Morphology
        PrDrawFilterConvolution convolution;          // Convolution
        PrDrawFilterTurbulence turbulence;            // Turbulence
        PrDrawFilterImage image;                      // Image
        PrRect tile;                                  // Tile: the part repeated, in the group's space
        PrDrawFilterShadow shadow;                    // DropShadow, InnerShadow
        PrDrawFilterGlow glow;                        // OuterGlow, InnerGlow
        float feather;                                // Feather: the radius
        PrDrawFilterDisplacement displacement;        // DisplacementMap
        PrDrawFilterLighting lighting;                // Lighting
    };
} PrDrawFilterNode;

// Creates an empty builder, or frees one.
PrDrawFilterBuilder* prDrawFilterBuilderCreate(void);
void prDrawFilterBuilderDestroy(PrDrawFilterBuilder* PR_NULLABLE builder);

// Sets the area where the whole graph produces output, in the group's space. An empty region means the group's bounds,
// and is the initial value.
void prDrawFilterBuilderSetRegion(PrDrawFilterBuilder* builder, PrRect region);

// Copies the node, and the data that it points to, into the graph, and returns the node's index. A list that the node
// draws from is frozen (draw_list.h). The node's inputs must name earlier nodes or the graph's own inputs, and its
// enumerations must hold valid members. A size below zero, such as a sigma or a radius, is treated as zero. A node that
// has a parameter that is not finite, an empty tile, an image source that is not valid (prDrawSourceIsValid) or a
// kernel wider or taller than kPrDrawConvolutionMaxExtent produces transparent black.
PrDrawFilterInput prDrawFilterBuilderAdd(PrDrawFilterBuilder* builder, const PrDrawFilterNode* node);

// Creates a filter with one reference from the nodes added so far, with the last node as the result. At least one node
// must have been added. Leaves the builder empty.
const PrDrawFilter* prDrawFilterCreate(PrDrawFilterBuilder* builder);

// Adds or releases a reference. The last release frees the filter.
void prDrawFilterRetain(const PrDrawFilter* filter);
void prDrawFilterRelease(const PrDrawFilter* PR_NULLABLE filter);

// Returns the graph's nodes, in the order added, and its region.
uint32_t prDrawFilterNodeCount(const PrDrawFilter* filter);
const PrDrawFilterNode* prDrawFilterNodes(const PrDrawFilter* filter);
PrRect prDrawFilterRegion(const PrDrawFilter* filter);

// Returns how far the graph may move the content that it reads, in the group's space. A group's layer extends this far
// beyond the region that the graph produces.
PrRect prDrawFilterOutset(const PrDrawFilter* filter);

// Returns a number that is unique to the filter for the life of the process. A backend uses it as a key for data that
// it derives from the filter.
uint64_t prDrawFilterId(const PrDrawFilter* filter);

// ---------------------------------------------------------------------------------------------------------------------
// Color matrices

// Returns the color matrix of SVG's feColorMatrix that saturates by `amount`, that rotates hues by `radians`, and that
// moves luminance to alpha. Each maps straight R, G, B and A, as a ColorMatrix node's matrix does.
PrAffine4 prDrawColorMatrixSaturate(float amount);
PrAffine4 prDrawColorMatrixHueRotate(float radians);
PrAffine4 prDrawColorMatrixLuminanceToAlpha(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
