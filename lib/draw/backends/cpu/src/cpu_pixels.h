// Pixels for the CPU backend, and the operations that cover, paint and composite them.

#pragma once

#include "pr/array.h"
#include "pr/draw_list.h"
#include "pr/draw_lower.h"
#include "pr/draw_renderer.h"
#include "pr/hash_index.h"
#include "pr/path_measure.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if the polyline is one axis-aligned rectangle of four sides.
bool cpuIsRectPolyline(const PrPathPolyline& polyline);

// The coverage of the pixels of a box. Row `y` holds device row `box.top + y`. Without data, every pixel of the box is
// fully covered.
struct CpuCoverage {
    uint8_t* PR_NULLABLE data;
    size_t stride;
    PrIRect box;
};

// One device row of a coverage: its pixels from `left` to `right`, which are fully covered if the row has no data.
// Pixels outside that range are uncovered.
struct CpuCoverageRow {
    const uint8_t* PR_NULLABLE data;
    int32_t left;
    int32_t right;
};

// Returns the coverage's row for device row `y`. A row outside the box covers nothing.
CpuCoverageRow cpuCoverageRow(const CpuCoverage& c, int32_t y);

// Returns the row's coverage of device column `x`, in 255ths.
inline uint32_t cpuCoverageAt(const CpuCoverageRow& row, int32_t x) {
    if (x < row.left || x >= row.right)
        return 0;
    return row.data != nullptr ? row.data[x - row.left] : 255;
}

// Pixels that commands draw into: the target's, or a group's offscreen. Row `y` holds device row `box.top + y`. Its
// pixels are premultiplied extended sRGB, encoded, in `format`: Rgba8 or Bgra8, which hold [0, 1], or RgbaF16.
struct CpuTarget {
    uint8_t* data;
    size_t stride;
    PrIRect box;
    PrPixelFormat format;
    bool knockout;          // each draw composites over transparent and replaces the pixels that it covers
    PrIRect drawn;          // the area that draws have reached, within `box`
};

// Returns the size in bytes of a pixel in `format`, which is Rgba8, Bgra8 or RgbaF16.
inline uint32_t cpuPixelBytes(PrPixelFormat format) {
    return format == PrPixelFormat_RgbaF16 ? 8 : 4;
}

// Returns the target's pixels of device row `y`, or its pixel at device column `x` of that row.
uint8_t* cpuTargetRow(const CpuTarget& t, int32_t y);
uint8_t* cpuTargetPixel(const CpuTarget& t, int32_t x, int32_t y);

// Reads a pixel in `format` as premultiplied RGBA, or writes premultiplied RGBA into one. A write clamps alpha to [0,
// 1], and clamps color to [0, alpha] in an 8-bit format.
void cpuReadPixel(const uint8_t* p, PrPixelFormat format, float* out);
void cpuWritePixel(uint8_t* p, PrPixelFormat format, const float* rgba);

// Copies the part of `from` within `to`'s box into `to`.
void cpuCopyTarget(const CpuTarget& from, const CpuTarget& to);

// A premultiplied color in the order of an 8-bit pixel's bytes, in 255ths.
struct CpuPixel {
    uint32_t channel[4];
};

// Returns a premultiplied RGBA color as a pixel of the 8-bit format, clamped.
CpuPixel cpuPixelFromRgba(const float* rgba, PrPixelFormat format);

// Writes the color into the pixel at `p`.
void cpuStorePixel(uint8_t* p, CpuPixel color);

// Scales the channels of the pixel in `format` at `p` by `scale`, in 255ths.
void cpuScalePixel(uint8_t* p, PrPixelFormat format, uint32_t scale);

// Returns x / 255, rounded, for x up to 255 * 255.
inline uint32_t cpuDivide255(uint32_t x) {
    x += 128;
    return (x + (x >> 8)) >> 8;
}

// Composites the color, scaled by `coverage` in 255ths, source-over the pixel.
void cpuSourceOver(uint8_t* p, CpuPixel color, uint32_t coverage);

// Blends the premultiplied RGBA source with the destination and composites it onto the destination.
void cpuCompositeColors(const float* s, const float* d, PrDrawComposite op, PrDrawBlendMode blend,
                        float* out);

// Returns a coverage's values for `count` pixels of row `y` from `x`. Returns null if every value is 255, and the
// coverage's own row if it holds all the values. Otherwise returns `scratch`, filled with zero outside the coverage's
// box, and with 255 inside it if the coverage has no data.
const uint8_t* PR_NULLABLE cpuCoverageSpan(const CpuCoverage& coverage, int32_t x, int32_t y,
                                           uint32_t count, uint8_t* scratch);

// Composites premultiplied RGBA sources into `count` pixels in `format` from `p`. Each source is blended with its pixel
// and composited by the operator, and the product of the two coverages mixes the result with the pixel. A null coverage
// is 255 throughout. A `sourceStride` of zero uses one source for every pixel.
//
// With `layer`, the sources are the pixels of a layer that a group composites. Each source is scaled by the shape's
// coverage before compositing, and only the clip's coverage mixes the result. A pixel that the shape does not cover
// then composites a transparent source. With `knockout`, each source composites over transparent instead of its pixel,
// and the product of the two coverages mixes the result with the pixel, also for a layer.
void cpuCompositeSpan(uint8_t* p, PrPixelFormat format, const float* source,
                      uint32_t sourceStride, const uint8_t* PR_NULLABLE shape,
                      const uint8_t* PR_NULLABLE clip, uint32_t count, PrDrawComposite op,
                      PrDrawBlendMode blend, bool knockout, bool layer);

// Composites a layer as cpuCompositeSpan does, with sources that are the layer's pixels in the target's format. There
// are at most kShadeSpan sources.
void cpuCompositePixels(uint8_t* p, PrPixelFormat format, const uint8_t* source,
                        const uint8_t* PR_NULLABLE shape, const uint8_t* PR_NULLABLE clip,
                        uint32_t count, PrDrawComposite op, PrDrawBlendMode blend, bool knockout);

// ---------------------------------------------------------------------------------------------------------------------
// Textures

// One level of a texture: premultiplied extended sRGB, encoded, rows packed.
struct CpuTextureLevel {
    uint8_t* PR_NULLABLE data;
    uint32_t width;
    uint32_t height;
    PrPixelFormat format;       // Rgba8, or RgbaF16
};

// An image or list in the form that the backend samples, with its mip levels once they are requested.
struct CpuTexture {
    uint64_t key;               // the id of the image or list
    PrRect region;              // List: the area of its space that the texture holds
    bool fromList;              // true if it holds what a list draws instead of an image's texels
    bool ready;                 // List: true if it was rendered in this render
    uint64_t lastUsed;          // the render that last used the texture
    Array<CpuTextureLevel> levels;
};

// Textures kept between renders, and dropped when a render does not use them.
struct CpuTextureCache {
    Array<CpuTexture*> textures;
    KeyIndex images;            // the textures of images, by image id
    uint64_t render;
};

// Returns the image's texture, which is created on first use. An image in half floats keeps them.
CpuTexture* cpuTextureFromImage(CpuTextureCache& cache, const PrDrawImage* image);

// Returns a list's texture of an area of its space at a size, in `format`. The caller renders the texture unless it is
// ready.
CpuTexture* cpuListTexture(CpuTextureCache& cache, uint64_t id, PrRect region, uint32_t width, uint32_t height,
                           PrPixelFormat format);

// Creates every level down to one pixel, if the sampling uses them.
void cpuPrepareMips(CpuTexture& texture, PrDrawSampling sampling);

// Begins and ends a render's use of the cache. The end releases the textures that the render did not use.
void cpuBeginTextures(CpuTextureCache& cache);
void cpuEndTextures(CpuTextureCache& cache);

// Releases every texture.
void cpuDestroyTextures(CpuTextureCache& cache);

// ---------------------------------------------------------------------------------------------------------------------
// Paint
//
// A painting command's pixels come from three independent parts, in the same way as in a fragment shader: the clip's
// coverage, the shape's coverage and the paint. The paint is the color times up to two stages: the brush, and an image
// command's texels. Each stage has a placement, which maps a pixel's center to a point in the stage's space, and a
// source, which gives the color at that point.

// The maximum number of pixels that one call of shade covers.
constexpr uint32_t kShadeSpan = 256;

// How a placement maps a pixel's center into a stage's space.
enum {
    PlacementKind_Affine,       // target pixels through `transform`
    PlacementKind_Projective,   // target pixels through `unproject` into user space, then `transform`

    // The nearest point of a stroke's centerline, as a point of the unit square along the stroke, then `transform`.
    PlacementKind_Stroke,
};
typedef uint8_t PlacementKind;

// The placement of a stage's space on the device.
struct CpuPlacement {
    PlacementKind kind;
    PrAffine2 transform;
    PrMatrix3 unproject;                        // Projective
    const PrPathMeasure* PR_NULLABLE measure;   // Stroke: the centerline in target pixels
    const PrPathStroke* PR_NULLABLE stroke;     // Stroke
    PrDrawStrokeMapping mapping;                // Stroke: Along or Across
    float strokeScale;                          // Stroke: the stroke's lengths to target pixels
    float reach;                                // Stroke: the widest half width, in target pixels
};

// The kind of data that a source reads its colors from.
enum {
    SourceKind_Gradient,
    SourceKind_Texture,
    SourceKind_Sides,           // the colors of a border's sides (PrDrawBorderSides)
};
typedef uint8_t SourceKind;

// The source of a stage's premultiplied colors.
struct CpuSource {
    SourceKind kind;
    const PrDrawBrush* PR_NULLABLE brush;          // Gradient
    const PrDrawRampPiece* PR_NULLABLE rampPieces; // Gradient
    const PrColor* PR_NULLABLE ramp;               // Gradient: the pieces' colors, premultiplied, without the color
    const PrDrawBorderSides* PR_NULLABLE sides;    // Sides: its colors premultiplied
    const CpuTexture* PR_NULLABLE texture;         // Texture
    PrRect texels;                                 // Texture: the texels that one tile holds
    PrDrawTiles tiles;                             // Texture: the tile in the stage's space, and how it repeats
    PrDrawSampling sampling;                       // Texture
    float limit;                                   // Texture: the brightest that a texel paints, or zero for no limit

    // Texture: a nine-slice's pieces, used instead of `texels` and `tiles`. Each piece maps its destination onto its
    // own texels.
    const PrDrawImageRect* PR_NULLABLE pieces;
    uint32_t pieceCount;
};

// A stage of a paint: where its space lies on the device, and its colors.
struct CpuStage {
    CpuPlacement placement;
    CpuSource source;
};

// A painting command's paint: the color times each stage.
struct CpuPaint {
    float color[4];             // premultiplied RGBA
    uint32_t stageCount;
    CpuStage stages[2];            // the brush, then an image command's texels
};

// Writes the paint's premultiplied RGBA at the centers of `count` pixels, at most kShadeSpan, from (x, y) rightward.
void cpuShade(const CpuPaint& paint, int32_t x, int32_t y, uint32_t count, float* out);

// Returns a mask's value for a premultiplied RGBA color painted by its brush, before inversion.
float cpuMaskValue(const PrDrawMask* mask, const float* rgba);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
