// The constants that the fragment renderer's frame holds and its shaders name: the enumerations of the frame, which
// this file defines, and the values of the public enumerations that the frame carries. The renderer
// (draw_fragment_frame.h) and the shaders (draw_fragment.slang) both compile this file, so the two agree. C++ checks
// each public value against the header that defines it. The device tests, which compare masks by luminance with the cpu
// backend's, check the luminance weights against color.h's.

#pragma once

#if defined(__SLANG__)

// A constant of the shaders.
#define PR_FRAGMENT_CONSTANT static const

// A public enumeration's value, which the shaders read as a constant.
#define PR_FRAGMENT_PUBLIC(name, value) static const uint32_t name = value;

#else

#include "pr/base.h"
#include "pr/draw.h"
#include "pr/draw_filter.h"
#include "pr/image_process.h"

// A constant of the renderer.
#define PR_FRAGMENT_CONSTANT constexpr

// Checks that a public enumeration's header gives it the value that the shaders read.
#define PR_FRAGMENT_PUBLIC(name, value) static_assert(uint32_t(name) == (value), #name " is not the value of its header");

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

#endif

// How an instance covers a pixel. The kinds from Rect to Shadow read a record from the frame's values, starting at the
// instance's `data`.
enum : uint32_t {
    FragmentCoverage_Solid,           // the whole pixel

    // The area of the pixel inside the record's rectangle: left, top, right and bottom, in the target's pixels.
    FragmentCoverage_Rect,

    FragmentCoverage_Shape,           // a FragmentCornerRect record
    FragmentCoverage_Ring,            // a FragmentRing record
    FragmentCoverage_SidedRing,       // a FragmentSidedRing record, which also gives the color
    FragmentCoverage_Shadow,          // a FragmentShadow record
    FragmentCoverage_Triangle,        // the instance's `coverages`, interpolated between its corners

    // The red channel of the texture that `data` names, an index into the frame's textures plus one, at the pixel's
    // point in local space, which is in the texture's texels.
    FragmentCoverage_Atlas,

    // The byte for the pixel's position in the frame's `coverage`. The quad's bytes start at `data`, in rows of the
    // quad's width.
    FragmentCoverage_Bytes,

    // The tile record at `data` in the frame's `tileData`, under the nonzero or the even-odd rule. A record holds its
    // segment count, the winding at the left of each of the quad's rows, then the start and end x, y of each segment in
    // the quad's pixels. A segment runs in the direction of its edge.
    FragmentCoverage_TilesNonZero,
    FragmentCoverage_TilesEvenOdd,
};
typedef uint8_t FragmentCoverage;

// An instance's coverage, and the flags beside it.
enum : uint32_t {
    FragmentInstanceFlag_Coverage = 0xff,             // its FragmentCoverage
    FragmentInstanceFlag_ReadsDestination = 0x100,    // its paint record composites by reading the destination
    FragmentInstanceFlag_Basic = 0x200,               // simple enough for the basic pipeline (fragmentShadeBasic)
};
typedef uint32_t FragmentInstanceFlags;

// What a paint record paints. A gradient reads its parameters from the record's `gradient`, in brush space. It is
// transparent where it has no parameter, and past its ends if it extends by None.
enum : uint32_t {
    FragmentPaintKind_Solid,      // opaque white, which leaves the instance's color unchanged
    FragmentPaintKind_Linear,     // start x, y, end x, y
    FragmentPaintKind_Radial,     // center x, y, radius, focal x, y, focal radius
    FragmentPaintKind_Conic,      // center x, y, and the angle of parameter 0 in radians from +x
    FragmentPaintKind_Diamond,    // center x, y, radii x, y

    FragmentPaintKind_Image,      // the record's texture, tiled

    FragmentPaintKind_Layer,      // the record's texture, a layer, at the pixel
};
typedef uint8_t FragmentPaintKind;

// How a group mask's pixel becomes its value.
enum : uint32_t {
    FragmentMask_None,                // no mask
    FragmentMask_Alpha,               // alpha
    FragmentMask_Luminance,           // the luminance of the premultiplied channels, with color.h's weights
    FragmentMask_LinearLuminance,     // the luminance of the straight channels in linear light, times alpha
};
typedef uint8_t FragmentMask;

// A group mask's kind, and the flags beside it.
enum : uint32_t {
    FragmentMaskFlag_Kind = 0x3,          // its FragmentMask
    FragmentMaskFlag_Invert = 0x4,        // one minus the value
};
typedef uint8_t FragmentMaskFlags;

// How an Image paint reads its texture. Either way, a tap is placed by the tile modes, and is transparent if it lands
// on no texel: beyond a tile that extends by None, or in the spacing between repeated tiles. Pad clamps a tap to the
// texels at the edge of `texels`.
enum : uint32_t {
    // The hardware's sampler filters the texture. The shader places the pixel's point by the tile modes within
    // `texels`, and the filter's taps beyond that point read by the sampler's address modes. Along an axis that
    // `texels` spans whole, a tap past the texture's edge reads as the axis's tile mode places it: clamped for Pad,
    // wrapped for Repeat, mirrored for Reflect, and transparent for None or for tiles with spacing. Along an axis that
    // `texels` spans in part, a tap past `texels` reads whatever texels lie around them (PrDrawSamplingFlag_Gutter),
    // and a tap past the texture's edge is transparent. Pad keeps the point within the centers of the edge texels.
    //
    // Nearest and Linear read the first level. Mipmap is trilinear and anisotropic, up to 16 taps, along the pixel's
    // footprint: the brush's change in texels from the pixel to its right and to its lower neighbor. Cubic is
    // Catmull-Rom over the sixteen nearest texels, as nine bilinear samples of the first level.
    FragmentImageRead_Sampler,

    // The shader reads each tap's texel within `texels` and filters the taps itself, from the first level or, for
    // Mipmap, anisotropically: from the two levels that the minor axis of the pixel's footprint falls between, with up
    // to 16 taps along its major axis.
    FragmentImageRead_Texels,
};
typedef uint8_t FragmentImageRead;

// The steps of a gradient's ramp table from 0 to 1, and the colors per unit of the parameter in a ramp's pieces that
// mix nonlinearly (prDrawGradientRamp).
PR_FRAGMENT_CONSTANT uint32_t kFragmentRampResolution = 256;

// A step of a filter, which a pass carries out over its instances' pixels into its layer. A step's inputs are up to
// three layers, which it reads at pixels of the target. A layer is transparent outside itself. Colors are premultiplied
// unless a step says straight. Each step says what it reads from `values` and `data`.
enum : uint32_t {
    FragmentFilterOp_Offset,          // the first input moved by (values[0], values[1]) pixels, interpolated
    FragmentFilterOp_Flood,           // the premultiplied color in values[0..3]

    // The first input's straight color times a matrix. `data` holds 20 values: a row for each of R, G, B and A, each
    // the weights of straight R, G, B, A and 1.
    FragmentFilterOp_ColorMatrix,

    // A transfer function on each of the first input's straight channels. `data` holds, for R, G, B and A in turn: the
    // PrDrawTransferKind, slope, intercept, amplitude, exponent, offset, a count, then `count` values.
    FragmentFilterOp_Transfer,

    // The first input composited onto the second, by the PrDrawComposite in values[0] and the PrDrawBlendMode in
    // values[1].
    FragmentFilterOp_Composite,

    // k1 a b + k2 a + k3 b + k4, where a is the first input, b the second, and k1 to k4 are values[0..3].
    FragmentFilterOp_Arithmetic,

    // The first input blurred along x or y by a Gaussian of sigma values[0] pixels. A read beyond the rectangle in
    // values[2..5] (left, top, right, bottom) follows the PrImageEdgeMode in values[1].
    FragmentFilterOp_BlurX,
    FragmentFilterOp_BlurY,

    // Each channel's smallest or largest value in the first input within values[0] pixels along x or y, as the
    // PrDrawMorphologyOp in values[1] says.
    FragmentFilterOp_MorphologyX,
    FragmentFilterOp_MorphologyY,

    // The first input convolved with the kernel in `data`, by rows. `values` holds the columns, rows, divisor, bias,
    // target x and y, the PrImageEdgeMode, whether to preserve alpha, then the rectangle as for a blur. A divisor of
    // zero means the kernel's sum, or one if the sum is zero.
    FragmentFilterOp_Convolution,

    // Perlin turbulence. `data` holds the transform from the target's pixels to the group's space (6 values), the base
    // frequency x and y, the octaves, the PrDrawTurbulenceKind, whether stitched, the stitch tile in the group's space
    // (left, top, right, bottom), then the seed's PrImageNoise: its lattice, then its gradients.
    FragmentFilterOp_Turbulence,

    FragmentFilterOp_Tile,            // the first input's rectangle in values[0..3] repeated
    FragmentFilterOp_Alpha,           // black with the first input's alpha
    FragmentFilterOp_InverseAlpha,    // black with one minus the first input's alpha
    FragmentFilterOp_Colorize,        // the straight color in values[0..3] times the first input's alpha
    FragmentFilterOp_Over,            // the first input source-over the second
    FragmentFilterOp_Atop,            // the first input source-atop the second
    FragmentFilterOp_Mask,            // the first input times the second's alpha

    // The first input read at the nearest pixel to a displaced point. The point moves along x by values[0] (c - 0.5),
    // where c is the second input's straight channel values[2], from 0 for R to 3 for A. It moves along y likewise, by
    // values[1] and values[3].
    FragmentFilterOp_Displacement,

    // Light on the surface that the first input's alpha gives. `data` holds the PrDrawLightingKind, the surface scale,
    // the constant, the exponent, the color (3), the PrDrawLightKind, the azimuth, the elevation, the position (3) and
    // the point that it aims at (3) in the target's pixels, the spot exponent, the cosine of the cone's angle (-1 for
    // no cone), and the scale from the group's space to the target's pixels.
    FragmentFilterOp_Lighting,

    // The first input mixed toward the second by t = clamp((a - values[0]) / (values[1] - values[0]), 0, 1), where a is
    // the third input's alpha.
    FragmentFilterOp_Mix,
};
typedef uint8_t FragmentFilterOp;

// How a filter step converts colors. The third input is never converted.
enum : uint32_t {
    FragmentFilterFlag_LinearInputs = 1 << 0,     // inputs are converted from sRGB's encoding to linear light
    FragmentFilterFlag_LinearResult = 1 << 1,     // the result is converted back to sRGB's encoding
    FragmentFilterFlag_RawFirst = 1 << 2,         // the first input is read without conversion
};
typedef uint8_t FragmentFilterFlags;

// The weights of the red, green and blue channels in the luminance of a color (kPrColorLuminanceRed and the others).
PR_FRAGMENT_CONSTANT float kFragmentLuminanceRed = 0.2125f;
PR_FRAGMENT_CONSTANT float kFragmentLuminanceGreen = 0.7154f;
PR_FRAGMENT_CONSTANT float kFragmentLuminanceBlue = 0.0721f;

// The values of the public enumerations that the frame carries.
PR_FRAGMENT_PUBLIC(PrDrawExtend_Pad, 0)
PR_FRAGMENT_PUBLIC(PrDrawExtend_Repeat, 1)
PR_FRAGMENT_PUBLIC(PrDrawExtend_Reflect, 2)
PR_FRAGMENT_PUBLIC(PrDrawExtend_None, 3)
PR_FRAGMENT_PUBLIC(PrDrawTileFlag_Clamped, 1)
PR_FRAGMENT_PUBLIC(PrDrawAlpha_Straight, 1)
PR_FRAGMENT_PUBLIC(PrDrawAlpha_Opaque, 2)
PR_FRAGMENT_PUBLIC(PrDrawStrokeMapping_Plain, 0)
PR_FRAGMENT_PUBLIC(PrDrawStrokeMapping_Along, 1)
PR_FRAGMENT_PUBLIC(PrDrawStrokeMapping_Across, 2)
PR_FRAGMENT_PUBLIC(PrDrawSampling_Nearest, 0)
PR_FRAGMENT_PUBLIC(PrDrawSampling_Linear, 1)
PR_FRAGMENT_PUBLIC(PrDrawSampling_Cubic, 2)
PR_FRAGMENT_PUBLIC(PrDrawSampling_Mipmap, 3)
PR_FRAGMENT_PUBLIC(PrImageEdgeMode_None, 0)
PR_FRAGMENT_PUBLIC(PrImageEdgeMode_Duplicate, 1)
PR_FRAGMENT_PUBLIC(PrImageEdgeMode_Wrap, 2)
PR_FRAGMENT_PUBLIC(PrImageEdgeMode_Mirror, 3)
PR_FRAGMENT_PUBLIC(PrDrawTransferKind_Identity, 0)
PR_FRAGMENT_PUBLIC(PrDrawTransferKind_Table, 1)
PR_FRAGMENT_PUBLIC(PrDrawTransferKind_Discrete, 2)
PR_FRAGMENT_PUBLIC(PrDrawTransferKind_Linear, 3)
PR_FRAGMENT_PUBLIC(PrDrawTransferKind_Gamma, 4)
PR_FRAGMENT_PUBLIC(PrDrawLightKind_Distant, 0)
PR_FRAGMENT_PUBLIC(PrDrawLightKind_Point, 1)
PR_FRAGMENT_PUBLIC(PrDrawLightKind_Spot, 2)
PR_FRAGMENT_PUBLIC(PrDrawComposite_SourceOver, 0)
PR_FRAGMENT_PUBLIC(PrDrawComposite_Clear, 1)
PR_FRAGMENT_PUBLIC(PrDrawComposite_Copy, 2)
PR_FRAGMENT_PUBLIC(PrDrawComposite_SourceIn, 3)
PR_FRAGMENT_PUBLIC(PrDrawComposite_SourceOut, 4)
PR_FRAGMENT_PUBLIC(PrDrawComposite_SourceAtop, 5)
PR_FRAGMENT_PUBLIC(PrDrawComposite_DestinationOver, 6)
PR_FRAGMENT_PUBLIC(PrDrawComposite_DestinationIn, 7)
PR_FRAGMENT_PUBLIC(PrDrawComposite_DestinationOut, 8)
PR_FRAGMENT_PUBLIC(PrDrawComposite_DestinationAtop, 9)
PR_FRAGMENT_PUBLIC(PrDrawComposite_Xor, 10)
PR_FRAGMENT_PUBLIC(PrDrawComposite_PlusLighter, 11)
PR_FRAGMENT_PUBLIC(PrDrawComposite_PlusDarker, 12)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_Normal, 0)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_Multiply, 1)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_Screen, 2)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_Overlay, 3)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_Darken, 4)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_Lighten, 5)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_ColorDodge, 6)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_ColorBurn, 7)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_HardLight, 8)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_SoftLight, 9)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_Difference, 10)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_Exclusion, 11)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_HardMix, 12)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_Hue, 13)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_Saturation, 14)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_Color, 15)
PR_FRAGMENT_PUBLIC(PrDrawBlendMode_Luminosity, 16)

#if !defined(__SLANG__)

} // namespace pr_impl

PR_ASSUME_NONNULL_END

#endif
