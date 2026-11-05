// Drawing: the vocabulary that a draw list records and a backend renders.
//
// A color is extended sRGB: sRGB's primaries, white and transfer function, mirrored below zero and extended past one. A
// color outside sRGB's gamut, or brighter than its white, has channels outside [0, 1]. A color carries no alpha
// convention, so each field that holds one says whether its alpha is straight or premultiplied.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/font.h"
#include "pr/geom.h"
#include "pr/path_stroke.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A list of draw commands, recorded once and rendered by any renderer (draw_list.h).
typedef struct PrDrawList PrDrawList;

// Pixels that commands draw from (draw_image.h).
typedef struct PrDrawImage PrDrawImage;

// An immutable graph of filter steps (draw_filter.h).
typedef struct PrDrawFilter PrDrawFilter;

// How a draw's result, once blended (PrDrawBlendMode), combines with what lies beneath.
enum {
    PrDrawComposite_SourceOver,
    PrDrawComposite_Clear,
    PrDrawComposite_Copy,
    PrDrawComposite_SourceIn,
    PrDrawComposite_SourceOut,
    PrDrawComposite_SourceAtop,
    PrDrawComposite_DestinationOver,
    PrDrawComposite_DestinationIn,
    PrDrawComposite_DestinationOut,
    PrDrawComposite_DestinationAtop,
    PrDrawComposite_Xor,
    PrDrawComposite_PlusLighter,
    PrDrawComposite_PlusDarker,
    PrDrawComposite_Count,
};
typedef uint8_t PrDrawComposite;

// Returns true if `composite` changes the destination in areas where the source is transparent. A group with such an
// operator changes everything within its bounds, not only what it drew.
bool prDrawCompositeIsUnbounded(PrDrawComposite composite);

// How a draw's color mixes with the color it is drawn over, before compositing.
enum {
    PrDrawBlendMode_Normal,
    PrDrawBlendMode_Multiply,
    PrDrawBlendMode_Screen,
    PrDrawBlendMode_Overlay,
    PrDrawBlendMode_Darken,
    PrDrawBlendMode_Lighten,
    PrDrawBlendMode_ColorDodge,
    PrDrawBlendMode_ColorBurn,
    PrDrawBlendMode_HardLight,
    PrDrawBlendMode_SoftLight,
    PrDrawBlendMode_Difference,
    PrDrawBlendMode_Exclusion,
    PrDrawBlendMode_HardMix,        // per channel: full if the two channels sum to one or more, otherwise zero
    PrDrawBlendMode_Hue,
    PrDrawBlendMode_Saturation,
    PrDrawBlendMode_Color,
    PrDrawBlendMode_Luminosity,
    PrDrawBlendMode_Count,
};
typedef uint8_t PrDrawBlendMode;

// ---------------------------------------------------------------------------------------------------------------------
// Paint

// The kinds of brush: a gradient, or an image source.
enum {
    PrDrawBrushKind_Linear,
    PrDrawBrushKind_Radial,
    PrDrawBrushKind_Conic,
    PrDrawBrushKind_Diamond,
    PrDrawBrushKind_Image,          // an image source, tiled
};
typedef uint8_t PrDrawBrushKind;

// The space that a brush's transform maps brush space into.
enum {
    PrDrawBrushSpace_User,          // the command's coordinates
    PrDrawBrushSpace_ObjectBounds,  // the unit square mapped onto the bounds of the fill geometry
    PrDrawBrushSpace_Target,        // the target's pixels, regardless of the current transforms
};
typedef uint8_t PrDrawBrushSpace;

// How a brush extends past its range: a gradient past parameters 0 and 1, and an image brush past its tile along an
// axis.
enum {
    PrDrawExtend_Pad,           // extends the end colors, or the edge texels
    PrDrawExtend_Repeat,
    PrDrawExtend_Reflect,       // every other copy is reflected
    PrDrawExtend_None,          // nothing past the range
};
typedef uint8_t PrDrawExtend;

// How an image is sampled between its texels, and the flags beside it.
//
// Every source is sampled the same way, whatever its kind: an image, a host texture, or a list or a projected group's
// layer rendered into a texture. Mipmap sampling reads the source's own mip levels. A backend gives an image and a list
// rendered for Mipmap sampling every level down to one texel, and reads a host texture's levels as the host made them,
// so a host texture with one level is minified from that level alone.
enum {
    PrDrawSampling_Nearest,
    PrDrawSampling_Linear,
    PrDrawSampling_Cubic,       // Catmull-Rom over the sixteen nearest texels
    PrDrawSampling_Mipmap,      // Linear, from the two levels that the footprint's minor axis falls between

    PrDrawSamplingFlag_Kind = 0x0f,     // the sampling, one of the values above

    // The caller allows hardware sampling of a source whose rect is part of its texels: the backend samples the whole
    // texture, and filtering reads the texels around the rect. Without this flag, a backend keeps every tap within the
    // rect.
    PrDrawSamplingFlag_Gutter = 0x10,
};
typedef uint8_t PrDrawSampling;

// A gradient's color at `offset`.
PR_NO_PADDING_BEGIN
typedef struct PrDrawColorStop {
    float offset;               // along the gradient's parameter
    PrColor16 color;            // straight alpha

    // Where between this stop and the next the two colors mix evenly, as a fraction of the gap, below one. Zero means
    // no hint.
    float hint;

    // The stop's hue in degrees, in the brush's interpolation space, used if `hasHue` is set and that space is polar. A
    // stop without one takes its hue from `color`. If `color` leaves that hue powerless, as for a gray, the hue is
    // missing, and the stop takes the hue of the stop that it mixes with. A stated hue applies even where it is
    // powerless, which `color` cannot express.
    float hue;
    bool hasHue;
    uint8_t _pad[3];
} PrDrawColorStop;
PR_NO_PADDING_END

// How an image's alpha relates to its color channels.
enum {
    PrDrawAlpha_Premultiplied,
    PrDrawAlpha_Straight,
    PrDrawAlpha_Opaque,         // every pixel is opaque, whatever its alpha channel holds
};
typedef uint8_t PrDrawAlpha;

// A texture of the host's, as Dear ImGui names a texture. It names a texture on the device of the renderer that renders
// the list (prDrawGpuTexture). Zero names no texture.
typedef uint64_t PrDrawTextureId;

// What a source draws from.
enum {
    PrDrawSourceKind_Image,
    PrDrawSourceKind_List,
    PrDrawSourceKind_Texture,
};
typedef uint8_t PrDrawSourceKind;

// What an image brush, a nine-slice or a filter's image draws from. Its kind decides which member of the union it uses.
//
// - Image: an image's texels.
// - List: an area of what a list draws, in the list's own space. A backend renders the area once into a layer at the
//   target scale, and samples the layer as an image.
// - Texture: the texels of the host's texture. The texture is on the device of the backend that renders the list, in
//   a format the backend samples, and `alpha` says how its alpha relates to its color. `extent` is its size in texels.
//   The host keeps it alive and unchanged until the frames that draw it are done. Its mip levels are the ones the host
//   gave it.
PR_NO_PADDING_BEGIN
typedef struct PrDrawSource {
    PrDrawSourceKind kind;
    PrDrawAlpha alpha;                          // Texture
    uint8_t _pad[6];
    PrRect rect;                                // texels, or for List an area of what the list draws
    PrUVec2 extent;                             // Texture
    union {
        const PrDrawImage* image;               // Image
        const PrDrawList* list;                 // List: frozen
        PrDrawTextureId texture;                // Texture
    };
} PrDrawSource;
PR_NO_PADDING_END

// Returns a source of each kind. A list source freezes the list.
PrDrawSource prDrawSourceImage(const PrDrawImage* image, PrRect rect);
PrDrawSource prDrawSourceList(PrDrawList* list, PrRect rect);
PrDrawSource prDrawSourceTexture(PrDrawTextureId texture, PrUVec2 extent, PrRect rect, PrDrawAlpha alpha);

// Returns true if the source names an object of its kind and its rect has nonzero area.
bool prDrawSourceIsValid(PrDrawSource source);

// How the copies of a repeated source are filtered.
enum {
    // Each copy is filtered on its own, as one tile that extends by Pad. A tap past a copy's edge reads the copy's edge
    // texels, and nothing paints between copies. A point belongs to the copy whose tile holds it. Without this flag,
    // the copies along a Repeat or Reflect axis form one periodic image. Its filter reads across a copy's edge into the
    // neighboring copy, and spaced copies fade into the gaps between them.
    PrDrawTileFlag_Clamped = 0x01,
};
typedef uint8_t PrDrawTileFlags;

// How an image brush repeats its source.
PR_NO_PADDING_BEGIN
typedef struct PrDrawTiles {
    PrRect tile;                // one tile, in brush space
    PrVec2 spacing;             // the gap between tiles
    PrDrawExtend x;             // how tiles extend past `tile` along each axis
    PrDrawExtend y;
    PrDrawTileFlags flags;
    uint8_t _pad;
} PrDrawTiles;
PR_NO_PADDING_END

// A linear gradient: the parameter is 0 at `start` and 1 at `end`, along the line between them.
PR_NO_PADDING_BEGIN
typedef struct PrDrawLinearGradient {
    PrVec2 start;
    PrVec2 end;
} PrDrawLinearGradient;
PR_NO_PADDING_END

// A two-point conical gradient between two circles.
PR_NO_PADDING_BEGIN
typedef struct PrDrawRadialGradient {
    PrVec2 start;               // the circle where the parameter is 0
    float startRadius;
    PrVec2 end;                 // the circle where the parameter is 1
    float endRadius;
} PrDrawRadialGradient;
PR_NO_PADDING_END

// A conic gradient: the parameter sweeps once around `center`.
PR_NO_PADDING_BEGIN
typedef struct PrDrawConicGradient {
    PrVec2 center;
    float angle;                // radians from +x, where the parameter is 0
} PrDrawConicGradient;
PR_NO_PADDING_END

// A diamond gradient: the parameter is 0 at `center` and 1 on the diamond whose half-diagonals are `radii`.
PR_NO_PADDING_BEGIN
typedef struct PrDrawDiamondGradient {
    PrVec2 center;
    PrVec2 radii;
} PrDrawDiamondGradient;
PR_NO_PADDING_END

// An image brush's source and how it repeats.
PR_NO_PADDING_BEGIN
typedef struct PrDrawImageFill {
    PrDrawSource source;        // what spans one tile
    PrDrawTiles tiles;

    // The brightest color that the image paints, as a multiple of SDR white, or zero for no limit. A texel past it is
    // scaled down to it (prColorLimit).
    float limit;

    PrDrawSampling sampling;
    uint8_t _pad[7];
} PrDrawImageFill;
PR_NO_PADDING_END

// A brush: paint that varies over the area it covers. Its kind decides which member of the union it uses. A brush
// starts from one of the functions below, since its neutral transform and opacity are not zero.
PR_NO_PADDING_BEGIN
typedef struct PrDrawBrush {
    PrDrawBrushKind kind;
    PrDrawBrushSpace space;
    PrDrawExtend extend;                    // gradients
    PrColorSpace interpolationSpace;        // gradients: the space that stops mix in
    PrColorHueMethod hueMethod;             // gradients interpolating in a polar space

    // Gradients: whether stops mix premultiplied, or with color and alpha interpolated separately (Straight), as SVG's
    // gradients mix them. In Straight, a stop next to transparent black mixes toward black.
    PrDrawAlpha interpolationAlpha;
    uint8_t _pad0[2];
    PrAffine2 transform;                    // brush space to the space that `space` names
    uint32_t stopCount;
    float opacity;                            // multiplies the brush's alpha
    const PrDrawColorStop* PR_NULLABLE stops; // gradients: offsets nondecreasing
    PR_PAD(0, 4)
    union {
        PrDrawLinearGradient linear;        // Linear
        PrDrawRadialGradient radial;        // Radial
        PrDrawConicGradient conic;          // Conic
        PrDrawDiamondGradient diamond;      // Diamond
        PrDrawImageFill image;              // Image
    };
} PrDrawBrush;
PR_NO_PADDING_END

// Returns a brush of `kind` in user space, with the identity transform, at an opacity of one, with no stops and no
// source. A gradient pads past its ends and interpolates premultiplied in sRGB, with hues taking the shorter way. An
// image brush pads its tile, with linear sampling.
PrDrawBrush prDrawBrushDefault(PrDrawBrushKind kind);

// Returns an image brush that places `source` on `destination`, in the command's space, and extends its edge texels
// beyond it. Its opacity is one.
PrDrawBrush prDrawBrushImage(PrDrawSource source, PrRect destination, PrDrawSampling sampling);

// Returns a gradient brush over `stops`, in user space with the identity transform. The brush pads past its ends and
// interpolates in sRGB, with hues taking the shorter way. Its opacity is one.
PrDrawBrush prDrawBrushLinear(PrVec2 start, PrVec2 end, const PrDrawColorStop* stops, uint32_t stopCount);
PrDrawBrush prDrawBrushRadial(PrVec2 start, float startRadius, PrVec2 end, float endRadius,
    const PrDrawColorStop* stops, uint32_t stopCount);
PrDrawBrush prDrawBrushConic(PrVec2 center, float angle, const PrDrawColorStop* stops, uint32_t stopCount);
PrDrawBrush prDrawBrushDiamond(PrVec2 center, PrVec2 radii, const PrDrawColorStop* stops, uint32_t stopCount);

// How a draw or a clip covers the pixels along its edges.
enum {
    PrDrawEdges_Antialiased,    // by the area of each pixel that it covers
    PrDrawEdges_Aliased,        // all of a pixel whose center it covers, and none of any other

    // By area, once each edge is rounded to the nearest target pixel, half up, where the snap space puts it
    // (prDrawListPushTransformSnapped). A rect, a corner rect's rect, a border's outer and inner edges and a clip rect
    // round. So does an image brush's tile: its start, and its far edge along an axis that does not repeat. Edges round
    // only where the snap space maps each axis onto an axis of the target.
    PrDrawEdges_Snapped,
};
typedef uint8_t PrDrawEdges;

// How a transform push takes part in snapping (PrDrawEdges_Snapped). The snap space is the composition of the transform
// pushes and embeddings from the root, except those pushed apart.
enum {
    PrDrawSnapFlag_X = 1 << 0,      // the transform's origin rounds to the nearest target pixel along x, half up
    PrDrawSnapFlag_Y = 1 << 1,      // and along y, both in the space that it is pushed in
    PrDrawSnapFlag_Apart = 1 << 2,  // the snap space inside is the one around it: snapped edges round where they would
                                    // lie without the transform, which then moves them

    // The transform maps a rectangle from the origin of the space inside it, whose four edges round to target pixels:
    // its translation and its scale change so that the rectangle fills the rounded edges. The transform must not rotate
    // or skew.
    PrDrawSnapFlag_Rect = 1 << 3,
};
typedef uint8_t PrDrawSnapFlags;

// ---------------------------------------------------------------------------------------------------------------------
// Strokes

// The position of a closed contour's stroke relative to the contour.
enum {
    PrDrawStrokeAlign_Center,
    PrDrawStrokeAlign_Inside,   // twice the width, cut to the shape's interior
    PrDrawStrokeAlign_Outside,  // twice the width, cut to the shape's exterior
};
typedef uint8_t PrDrawStrokeAlign;

// How a brush is placed on a stroke.
enum {
    PrDrawStrokeMapping_Plain,  // in its own space

    // On a unit square. x runs along the contour, from 0 at its start to 1 at its end. y runs across the stroke, from 0
    // on the left to 1 on the right.
    PrDrawStrokeMapping_Along,

    PrDrawStrokeMapping_Across, // as Along, with x and y swapped
};
typedef uint8_t PrDrawStrokeMapping;

// A stroke: its shape, and how it is measured, placed and painted. A width of zero or less strokes nothing.
typedef struct PrDrawStroke {
    PrPathStroke shape;
    bool nonScaling;            // width, dashes and caps are in target pixels
    PrDrawStrokeAlign align;    // ignored for open contours, which stroke centered
    PrDrawStrokeMapping mapping;
} PrDrawStroke;

// Returns a centered, scaling stroke with Plain mapping and the shape that prPathStrokeDefault returns.
PrDrawStroke prDrawStrokeDefault(void);

// ---------------------------------------------------------------------------------------------------------------------
// Paints

// How a draw paints: a solid color or a brush, over its fill or along its stroke, composited onto the target. A paint
// names its color or its brush. The other fields are zero for an antialiased fill that composites source-over with
// normal blending. Each recording function of draw_list.h says which fields its command reads.
typedef struct PrDrawPaint {
    PrColor16 color;                            // straight alpha; unused with a brush
    const PrDrawBrush* PR_NULLABLE brush;
    const PrDrawStroke* PR_NULLABLE stroke;     // null to fill
    PrDrawComposite composite;
    PrDrawBlendMode blendMode;
    PrDrawEdges edges;
} PrDrawPaint;

// ---------------------------------------------------------------------------------------------------------------------
// Boxes

// The styles of a side of a border. Hidden and none both paint nothing. They differ only for a client that resolves
// conflicts between borders, such as a table that collapses its borders.
enum {
    PrDrawBorderStyle_None,
    PrDrawBorderStyle_Solid,
    PrDrawBorderStyle_Double,
    PrDrawBorderStyle_Dotted,
    PrDrawBorderStyle_Dashed,
    PrDrawBorderStyle_Groove,
    PrDrawBorderStyle_Ridge,
    PrDrawBorderStyle_Inset,
    PrDrawBorderStyle_Outset,
    PrDrawBorderStyle_Hidden,
};
typedef uint8_t PrDrawBorderStyle;

// Returns true if a border side of `style` paints, which every style but none and hidden does.
static inline bool prDrawBorderStylePaints(PrDrawBorderStyle style) {
    return style != PrDrawBorderStyle_None && style != PrDrawBorderStyle_Hidden;
}

// The most times a pattern repeats along its length: a border side's dashes or dots, or a wavy decoration's wave.
enum { kPrDrawPatternMaxPeriods = 4096 };

// One side of a border, painted with its color or its brush.
typedef struct PrDrawBorderSide {
    float width;
    PrDrawBorderStyle style;
    PrColor16 color;                        // straight alpha; unused with a brush
    const PrDrawBrush* PR_NULLABLE brush;

    // Dashed and Dotted: how many times the pattern repeats along the side, from 1 through kPrDrawPatternMaxPeriods.
    // Zero fits the pattern to the side's length.
    uint32_t patternPeriods;
} PrDrawBorderSide;

// A border around a box, with a side for each edge.
typedef struct PrDrawBorder {
    PrCornerRect shape;                         // the outer edge
    PrDrawBorderSide sides[PrRectSide_Count];
} PrDrawBorder;

// A blurred shadow of a box.
typedef struct PrDrawShadow {
    PrCornerRect shape;                 // the caster, or the opening of an inner shadow
    PrVec2 offset;
    float sigma;                        // the blur's standard deviation
    float spread;
    bool nonScaling;                    // `sigma` and `offset` are in target pixels
} PrDrawShadow;

// The styles of a text decoration, as CSS text-decoration-style names them.
enum {
    PrDrawDecorationStyle_Solid,
    PrDrawDecorationStyle_Double,
    PrDrawDecorationStyle_Dotted,
    PrDrawDecorationStyle_Dashed,
    PrDrawDecorationStyle_Wavy,
};
typedef uint8_t PrDrawDecorationStyle;

// A line under, over or through text, running right from `origin`.
typedef struct PrDrawDecoration {
    PrVec2 origin;                     // the start of the center line
    float length;
    float thickness;
    PrDrawDecorationStyle style;
    float wavelength;                   // Wavy
    float amplitude;                    // Wavy
    float phase;                        // Wavy: how far into the wave the origin is
} PrDrawDecoration;

// ---------------------------------------------------------------------------------------------------------------------
// Images

// How a nine-slice fills an edge or its center.
enum {
    PrDrawSliceMode_Stretch,    // one copy scaled
    PrDrawSliceMode_Repeat,     // copies at their scaled size, centered, cut at both ends
    PrDrawSliceMode_Round,      // scaled so a whole number of copies fit
    PrDrawSliceMode_Space,      // a whole number of copies, with the remainder spread between them
};
typedef uint8_t PrDrawSliceMode;

// An image divided by four insets into corners, edges and a center, which stretch or repeat to fill a destination.
typedef struct PrDrawNineSlice {
    float slices[PrRectSide_Count];     // insets into the source's rect
    float widths[PrRectSide_Count];     // insets into the destination
    PrDrawSliceMode horizontal;         // the top and bottom edges, and the center
    PrDrawSliceMode vertical;           // the left and right edges, and the center
    bool center;                        // whether the center paints
} PrDrawNineSlice;

// ---------------------------------------------------------------------------------------------------------------------
// Groups

// Which value of a mask's pixels masks the group (PrDrawMask).
enum {
    PrDrawMaskMode_Alpha,
    PrDrawMaskMode_Luminance,
};
typedef uint8_t PrDrawMaskMode;

// A mask over a group, in the group's space. The mask's value is its brush. Alpha mode uses the alpha. Luminance mode
// uses the luminance in `space` (kPrColorLuminanceRed and the others), times the alpha. `invert` uses one minus the
// value.
typedef struct PrDrawMask {
    PrDrawBrush brush;          // an image brush over a list, for drawn content
    PrDrawMaskMode mode;
    PrColorSpace space;         // Luminance: sRGB or sRGB-linear
    bool invert;
} PrDrawMask;

// Returns a mask of `brush` with alpha mode, sRGB, and no inversion.
PrDrawMask prDrawMaskDefault(const PrDrawBrush* brush);

// Returns a mask of what `list` draws over `area`, read in `mode` and `space`, which is zero outside `area`. What the
// list draws outside `area` is not read.
PrDrawMask prDrawMaskFromList(PrDrawList* list, PrRect area, PrDrawMaskMode mode, PrColorSpace space);

// A texture of the host's that a group draws into in place of a layer that starts transparent (PrDrawGroup::layer). It
// keeps what the group drew into it before, from one frame to the next and from one renderer to another. The group's
// `bounds` map onto its texels.
//
// `*drawn` is the revision of the contents that the texture holds, which the host keeps, and `revision` is the newest
// revision of the groups inside the layer's group (PrDrawGroup::revision). Preparing a frame claims the groups past
// `*drawn`: it raises `*drawn` to `revision` atomically, and draws only the groups whose revisions are past the value
// that it raised. So contents that a host records once, in groups of rising revisions, draw into the texture once,
// however many frames and renderers draw them, and renderers on several threads may prepare lists that draw the same
// texture. On a device, each renderer's commands draw what it claimed, so work submitted after them sees it. In memory,
// the host draws a texture from one thread at a time.
typedef struct PrDrawGroupLayer {
    PrDrawTextureId texture;        // drawn into and sampled, in a format that the renderer draws layers in
    PrUVec2 extent;                 // in texels
    uint64_t* drawn;                // read and raised atomically
    uint64_t revision;
    PrDrawSampling sampling;        // how the texture is sampled where the group composites it
} PrDrawGroupLayer;

// A group draws its contents into a layer that starts transparent, or into the host's texture (`layer`). It then
// composites the result, times its opacity and mask, onto the target with its operator and blend mode. A group with no
// effect draws its contents straight onto the target. Such a group has full opacity, source-over and normal, no filter,
// backdrop, mask, projection or layer, and is neither isolated nor knockout.
//
// `bounds` is the area that the group may cover in the current space, or empty to use the bounds of the current clip.
typedef struct PrDrawGroup {
    PrRect bounds;
    float opacity;
    PrDrawComposite composite;
    PrDrawBlendMode blendMode;

    // Draws into a layer even with no other effect, so that blend modes inside the group do not reach what lies
    // beneath.
    bool isolated;

    bool knockout;                              // each child composites against the group's initial state
    const PrDrawFilter* PR_NULLABLE filter;     // applied to the contents
    const PrDrawFilter* PR_NULLABLE backdrop;   // applied to what lies beneath, drawn under the contents
    const PrDrawMask* PR_NULLABLE mask;

    // Draws the group's contents through a projection of their plane, or flat if null. The matrix maps a point (x, y,
    // 0, 1) of the plane to homogeneous coordinates of the space that is current at the push, which are divided by w.
    // The group is drawn by its x, y and w. z is kept for later use, and nothing reads it. Copied when the group is
    // pushed.
    //
    // The contents, `bounds`, `filter` and `mask` are in the plane. `bounds` must not be empty: it is the part of the
    // plane that draws. The contents play as a list source plays (PrDrawSourceKind_List): clips pushed inside are in
    // the plane, and commands in target pixels are in the pixels of the group's layer. `opacity`, `composite` and
    // `blendMode` apply where the projected plane lands, and clips open at the push cut the result.
    //
    // The plane is cut where w falls below a small fraction of the largest w over `bounds`. A group whose plane is
    // edge-on, lies behind the eye, or whose matrix is not finite draws nothing.
    //
    // A projection alone does not make a group a backdrop root (prDrawGroupIsBackdropRoot). The group's own backdrop
    // reads what lies beneath the plane on the target, mapped back through the projection into the plane, and filters
    // there. A backdrop inside a group that is not a root reads the same, beneath what the group drew before it.
    const PrMatrix4* PR_NULLABLE projection;

    // The space in which the group's result blends and composites onto what lies beneath: sRGB, or sRGB-linear for
    // SVG's color-interpolation. A group in sRGB-linear reads what lies beneath and has no backdrop, and its blend mode
    // applies only if it composites source-over. A projected group composites in sRGB.
    PrColorSpace space;

    // The host's texture that the group draws into and composites, or null. `bounds` must not be empty. Copied when the
    // group is pushed.
    const PrDrawGroupLayer* PR_NULLABLE layer;

    // The revision of the group's contents within the nearest group with a layer that holds it, or zero for contents
    // that always draw. A group with only a revision has no effect.
    uint64_t revision;
} PrDrawGroup;

// Returns a group with no effect.
PrDrawGroup prDrawGroupDefault(void);

// Returns true if the group draws into a layer instead of drawing its contents straight onto the target (PrDrawGroup).
bool prDrawGroupHasEffect(const PrDrawGroup* group);

// Returns true if a backdrop inside the group reads only what the group drew before it. Every group with an effect is a
// backdrop root, except a projected group with no other effect.
bool prDrawGroupIsBackdropRoot(const PrDrawGroup* group);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
