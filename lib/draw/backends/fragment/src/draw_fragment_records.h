// The records of the fragment renderer's frame that its shaders read: the builder writes them (draw_fragment_frame.h)
// and the shaders read them (draw_fragment.slang). Both compile this file. C++ sees each field's type on the left of
// PR_FRAGMENT_TYPE, and Slang the type on its right, which has the same size.
//
// Every field lies at a multiple of its shader type's alignment: 16 bytes for a 4-vector, 8 for a 2-vector, and 4 for a
// scalar or an array of scalars. No field is a 3-vector, and every record's size is a multiple of 16 bytes. So C++,
// Metal, Vulkan and Direct3D 12 lay out each record alike, a shader reads one as a typed structure (prGpuLoad), and a
// record that the frame's values hold keeps the values' 16-byte alignment. The assertions at the end check the C++
// side.

#pragma once

#if defined(__SLANG__)

// A field of type `shaderType` in the shaders, which is `cType` in C++.
#define PR_FRAGMENT_TYPE(cType, shaderType) shaderType

// Slang lays the records out as C++ does without them.
#define PR_NO_PADDING_BEGIN
#define PR_NO_PADDING_END

// A projective matrix, by rows (PrMatrix3).
struct FragmentMatrix3 {
    float m[3][3];
};

#else

#include "pr/base.h"
#include "pr/color.h"
#include "pr/geom.h"
#include "pr/vec.h"

#include <stddef.h>

// A field of type `cType` in C++, which is `shaderType` in the shaders.
#define PR_FRAGMENT_TYPE(cType, shaderType) cType

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

#endif

// A quad, parallelogram or triangle of the target's pixels to draw, regardless of winding. A pixel's coverage is the
// instance's coverage of it times its clip coverage. The records of Shape, Ring and Shadow coverage are in local space,
// and are evaluated at the pixel's center.
//
// An instance without ReadsDestination draws its paint times the coverage over the destination. The blend factors are
// one and one minus source alpha, for color and alpha alike. An instance with ReadsDestination composites its paint
// onto the destination as its paint record says, and mixes the result with the destination by the coverage.
//
// For Atlas, Bytes and Tiles coverage, `transform` is a translation. For Bytes and Tiles, `bounds` has its top left at
// the origin, and a pixel's position is its offset from that corner.
PR_NO_PADDING_BEGIN
struct FragmentInstance {
#if defined(__SLANG__)
    float4 bounds;
    float2 transform[3];
#else
    union {
        PrRect bounds;              // the quad in local space
        float coverages[3];         // Triangle: the coverage at each corner
    };
    union {
        PrAffine2 transform;       // local space to the target's pixels, as the images of x, y and the origin
        PrVec2 corners[3];         // Triangle: in the target's pixels
    };
#endif
    PR_FRAGMENT_TYPE(PrColor16, uint2) color; // premultiplied, half floats
    uint32_t flags;                           // FragmentInstanceFlags
    uint32_t data;                            // the start of the coverage's data, as the coverage kind defines
    uint32_t paint;                           // its paint record's index plus one, or zero to paint `color` alone
    uint32_t clip;                            // its clip record's index plus one, or zero for none
};
PR_NO_PADDING_END

// A rectangle whose corners are superellipses. The arrays are indexed by PrRectCorner. A corner is square unless both
// its radii are positive. Otherwise its shape K (PrCorner) gives it the exponent 2^|K|. A corner with K of zero or more
// is convex, centered `radii` in from the rectangle's corner. A corner with a negative K is concave: the outside of the
// superellipse centered on the rectangle's corner.
PR_NO_PADDING_BEGIN
struct FragmentCornerRect {
    PR_FRAGMENT_TYPE(PrRect, float4) rect;
    PR_FRAGMENT_TYPE(PrVec2, float2) radii[4];
    float shapes[4];                // each corner's K
};
PR_NO_PADDING_END

// The area inside `outer` and outside `inner`, limited to three half planes. Each plane (a, b, c) holds the points
// where a x + b y + c < 0. A plane whose a and b are zero holds everything.
PR_NO_PADDING_BEGIN
struct FragmentRing {
    FragmentCornerRect outer;
    FragmentCornerRect inner;
    float planes[3][3];
    uint32_t outside;               // whether it is limited to what lies outside the planes' intersection instead
    uint32_t _pad[2];
};
PR_NO_PADDING_END

// A border's ring, which paints each side in its own color. Its coverage is the area inside `outer` and outside
// `inner`, as a whole, so the sides meet without a seam. A pixel takes its color in place of the instance's: the color
// of the side that the pixel's center lies on, blended across the line that divides two sides over about a pixel. The
// lines at `split` divide the ring into its corners' quarters, and in each quarter the corner's line divides the side
// before it from the side after it, clockwise.
PR_NO_PADDING_BEGIN
struct FragmentSidedRing {
    FragmentCornerRect outer;
    FragmentCornerRect inner;
#if defined(__SLANG__)
    float4 colors[4][2];
#else
    float colors[4][2][4];          // the colors of the sides before and after each corner, premultiplied
#endif

    // Each corner's line (a, b, c), where a x + b y + c is proportional to the distance from the line, positive toward
    // the side after the corner.
    float divides[4][3];

    PR_FRAGMENT_TYPE(PrVec2, float2) split;
    uint32_t _pad[2];
};
PR_NO_PADDING_END

// A shadow. An outer shadow is `shape` blurred by a Gaussian of `sigma`, less what `caster` covers. An inner shadow is
// the outside of the blurred shape, within `caster`. A `sigma` of zero does not blur.
PR_NO_PADDING_BEGIN
struct FragmentShadow {
    FragmentCornerRect shape;
    FragmentCornerRect caster;
    float sigma;
    uint32_t inner;                 // whether it is an inner shadow
    uint32_t _pad[2];
};
PR_NO_PADDING_END

// The centerline of a stroke whose brush is mapped along or across it, in the target's pixels.
//
// Each pixel uses the point of the centerline nearest to its center within `reach`, as chosen by prPathMeasureNearest.
// A pixel with no such point paints nothing. The point gives two fractions: how far along its contour it lies, and how
// far across the stroke's width the pixel lies, from the stroke's left side. Along places them in the unit square as
// (along, across), and Across as (across, along).
//
// The width on each side is the profile's width, interpolated linearly between its points and held past its ends, times
// `width`. Without a profile, each side is half of `width`.
//
// The segments are binned in a grid of square cells. Each cell lists every segment within `reach` of it.
PR_NO_PADDING_BEGIN
struct FragmentMap {
    PR_FRAGMENT_TYPE(PrVec2, float2) origin;    // the grid's top left
    float cellSize;
    uint32_t columns;
    uint32_t rows;

    // Where the list of cell starts begins in the frame's `mapIndices`. The list runs by rows, with one more start
    // after the last cell. A cell's segments are the numbers in `mapIndices` from its start up to the next start.
    uint32_t cells;

    // Where the segments begin in the frame's `mapData`. Each segment is six values: a.x, a.y, b.x, b.y, the distance
    // along its contour to a, and the contour's length.
    uint32_t segments;

    // Where the profile's points begin in the frame's `mapData`, and how many there are, zero for none. Each point is
    // three values: its fraction along the contour, then the widths on the left and on the right as fractions of
    // `width`.
    uint32_t profile;
    uint32_t profileCount;

    float width;
    float reach;
    uint32_t mapping;               // PrDrawStrokeMapping
};
PR_NO_PADDING_END

// A piece of an image drawn in pieces, such as a nine-slice. A point of brush space reads the first piece whose
// destination holds it, with left and top edges in and right and bottom edges out. A point outside every destination
// reads the nearest piece, by the sum of its distances along each axis. A piece maps its source's texels onto its
// destination, and clamps reads to them.
PR_NO_PADDING_BEGIN
struct FragmentPiece {
    PR_FRAGMENT_TYPE(PrRect, float4) destination;   // in brush space
    PR_FRAGMENT_TYPE(PrRect, float4) source;        // in the first level's texels
};
PR_NO_PADDING_END

// A paint and how it composites. A pixel's paint is the record's paint times the instance's color, then times the group
// mask's value if it has one.
//
// `brush` maps a pixel's center to brush space. For a mapped stroke, it maps the pixel's point of the unit square
// instead. An image is sampled by `sampling`, as `read` says. Its texture is sampled in its own texels, whatever its
// size.
PR_NO_PADDING_BEGIN
struct FragmentPaint {
    PR_FRAGMENT_TYPE(PrRect, float4) tile;      // Image: one tile in brush space, which shows `texels`
    PR_FRAGMENT_TYPE(PrRect, float4) texels;    // Image: the source's texels in the first level
    PR_FRAGMENT_TYPE(PrVec2, float2) spacing;   // Image: the gap between tiles, in brush space
    PR_FRAGMENT_TYPE(PrMatrix3, FragmentMatrix3) brush;
    float gradient[6];              // gradients: as the kind says
    uint32_t kind;                  // FragmentPaintKind
    uint32_t extend;                // gradients: PrDrawExtend
    uint32_t ramp;                  // gradients: the ramp's index in the frame's ramp tables and cells
    uint32_t tileX;                 // Image: PrDrawExtend along each axis
    uint32_t tileY;
    uint32_t sampling;              // Image: PrDrawSampling
    uint32_t read;                  // Image: FragmentImageRead
    uint32_t texture;               // Image, Layer: an index into the frame's textures plus one
    uint32_t pieces;                // Image: the index of its first piece in the frame's pieces
    uint32_t pieceCount;            // Image: zero for an image in one piece
    uint32_t map;                   // the index of a mapped stroke's map plus one, or zero
    uint32_t composite;             // PrDrawComposite
    uint32_t blendMode;             // PrDrawBlendMode
    uint32_t knockout;              // whether it composites over transparent instead of the destination
    uint32_t groupMask;             // FragmentMaskFlags of its group's mask
    uint32_t groupMaskTexture;      // the group mask's layer, an index into the frame's textures plus one

    // Image: PrDrawAlpha of the texels. Texels read straight are premultiplied per tap, and texels that the sampler
    // filters straight are premultiplied after filtering, so a transparent texel's color bleeds into its neighbors'.
    uint32_t alpha;

    float limit;                    // Image: the brightest that a texel paints (prColorLimit), or zero for no limit
    uint32_t tileFlags;             // Image: PrDrawTileFlags
};
PR_NO_PADDING_END

// A rectangle with shaped corners that clips. `transform` maps the target's pixels to the shape's space.
PR_NO_PADDING_BEGIN
struct FragmentClipShape {
    FragmentCornerRect shape;
    PR_FRAGMENT_TYPE(PrMatrix3, FragmentMatrix3) transform;
    uint32_t invert;                // whether it covers what lies outside the shape instead
    uint32_t _pad[2];
};
PR_NO_PADDING_END

// The clips in force for an instance. A pixel's clip coverage is the area of the pixel inside `rect`, times each
// shape's coverage, times the mask's alpha at the pixel if there is a mask.
PR_NO_PADDING_BEGIN
struct FragmentClip {
    PR_FRAGMENT_TYPE(PrRect, float4) rect; // in the target's pixels
    uint32_t shapes;                       // where its entries begin in the frame's `clipShapes`
    uint32_t shapeCount;
    uint32_t mask;                  // the mask layer, an index into the frame's textures plus one, or zero
    uint32_t _pad;
};
PR_NO_PADDING_END

// An entry of the frame's textures. Instances and records name an entry by its index plus one, and zero names none. The
// entry's texture is a texture that the requests created, a layer's texture, or a host texture.
//
// A layer covers its rectangle of the target's pixels, and is transparent outside it whatever its texture holds there.
// Reading the entry at a pixel gives its texture's pixel composited source-over the pixel of the entry that `under`
// names, or source-over `color` if `under` is zero.
PR_NO_PADDING_BEGIN
struct FragmentTexture {
    PR_FRAGMENT_TYPE(PrIVec2, int2) origin;     // a layer's position and extent in the target's pixels
    PR_FRAGMENT_TYPE(PrUVec2, uint2) extent;
    PR_FRAGMENT_TYPE(PrColor16, uint2) color; // premultiplied half floats: what it is read over if `under` is zero
    uint32_t texture;                         // the texture's number
    uint32_t under;                           // the entry that it is read over, as its index plus one, or zero

    // The host's texture read instead of `texture`, as its index in `hostTextures` plus one, or zero.
    uint32_t hostTexture;

    uint32_t _pad[3];
};
PR_NO_PADDING_END

// A step of a filter, and what it reads (FragmentFilterOp).
PR_NO_PADDING_BEGIN
struct FragmentFilter {
    uint32_t op;                    // FragmentFilterOp
    uint32_t flags;                 // FragmentFilterFlags
    float values[16];               // as the op says
    uint32_t data;                  // where its data begins in the frame's `filterData`
    uint32_t dataCount;
    uint32_t inputs[3];             // the layers it reads, each an index into the frame's textures plus one, or zero
    uint32_t _pad;
};
PR_NO_PADDING_END

#if !defined(__SLANG__)

// The layout that the shaders read, as the file's comment says.
#define PR_FRAGMENT_AT(Record, field, alignment) \
    static_assert(offsetof(Record, field) % (alignment) == 0, #Record "." #field " is not aligned for the shaders")
#define PR_FRAGMENT_SIZE(Record) static_assert(sizeof(Record) % 16 == 0, #Record " is not a multiple of 16 bytes")

// Each record's fields at the shaders' alignments, and its size a multiple of 16 bytes.
PR_FRAGMENT_AT(FragmentInstance, bounds, 16);
PR_FRAGMENT_AT(FragmentInstance, transform, 8);
PR_FRAGMENT_AT(FragmentInstance, color, 8);
PR_FRAGMENT_SIZE(FragmentInstance);
PR_FRAGMENT_AT(FragmentCornerRect, rect, 16);
PR_FRAGMENT_AT(FragmentCornerRect, radii, 8);
PR_FRAGMENT_SIZE(FragmentCornerRect);
static_assert(PrRectCorner_Count == 4);
PR_FRAGMENT_AT(FragmentRing, inner, 16);
PR_FRAGMENT_SIZE(FragmentRing);
PR_FRAGMENT_AT(FragmentSidedRing, inner, 16);
PR_FRAGMENT_AT(FragmentSidedRing, split, 8);
PR_FRAGMENT_SIZE(FragmentSidedRing);
PR_FRAGMENT_AT(FragmentShadow, caster, 16);
PR_FRAGMENT_SIZE(FragmentShadow);
PR_FRAGMENT_SIZE(FragmentMap);
PR_FRAGMENT_AT(FragmentPiece, source, 16);
PR_FRAGMENT_SIZE(FragmentPiece);
PR_FRAGMENT_AT(FragmentPaint, texels, 16);
PR_FRAGMENT_AT(FragmentPaint, spacing, 8);
PR_FRAGMENT_SIZE(FragmentPaint);
PR_FRAGMENT_SIZE(FragmentClipShape);
PR_FRAGMENT_SIZE(FragmentClip);
PR_FRAGMENT_AT(FragmentTexture, extent, 8);
PR_FRAGMENT_AT(FragmentTexture, color, 8);
PR_FRAGMENT_SIZE(FragmentTexture);
PR_FRAGMENT_SIZE(FragmentFilter);
#undef PR_FRAGMENT_AT
#undef PR_FRAGMENT_SIZE

} // namespace pr_impl

PR_ASSUME_NONNULL_END

#endif
