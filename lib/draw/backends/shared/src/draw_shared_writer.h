// What the writers of documents share: the walk of a page's list that draws each command as natively as the format
// allows, lowers what it can, and rasterizes the rest with the CPU renderer, and the text that formats are written in.

#pragma once

#include "pr/draw_commands.h"
#include "pr/draw_glyph.h"
#include "pr/draw_renderer.h"
#include "pr/draw_transform.h"
#include "pr/hash.h"
#include "pr/path.h"
#include "pr/path_polyline.h"
#include "pr/raw_array.h"
#include "pr/text_buffer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Text

// Appends a number to `out`, as drawSharedRound rounds it, in the fixed notation that every format reads
// (prF64FormatFixed). A number that is not finite is written as 0, and a magnitude past what 64 bits hold at `decimals`
// places keeps only its integer part, up to 9e18.
void drawSharedAppendNumber(PrTextBuffer& out, double value, uint32_t decimals = 4);

// Returns the number that drawSharedAppendNumber writes for `value` at `decimals` places.
double drawSharedRound(double value, uint32_t decimals = 4);

// Appends `value` in hexadecimal, as two digits for each of `bytes` bytes, most significant first.
void drawSharedAppendHex(PrTextBuffer& out, uint64_t value, uint32_t bytes);

// Returns the straight-alpha color clamped to [0, 1], as a format without extended range holds it.
PrColor drawSharedClamp(PrColor color);

// The texels of a texture that an image holds, and the alpha that they are read with. Two images with equal keys hold
// the same texels, so a format embeds them once.
PR_NO_PADDING_BEGIN
struct DrawSharedImageKey {
    PrGpuTexture texture;       // zero for texels that are not drawn again; a zero key matches no other key
    PrIRect rect;               // in texels of the texture
    PrTextureAlpha alpha;
    uint8_t _pad[3];
};
PR_NO_PADDING_END

// Returns true if the keys name the same texels of the same texture, read with the same alpha.
inline bool drawSharedImageKeyEquals(const DrawSharedImageKey& a, const DrawSharedImageKey& b) {
    return a.texture.id == b.texture.id && a.alpha == b.alpha && a.rect.left == b.rect.left &&
        a.rect.top == b.rect.top && a.rect.right == b.rect.right && a.rect.bottom == b.rect.bottom;
}

// Returns the hash of the key, for a hash index.
inline uint32_t drawSharedImageKeyHash(const DrawSharedImageKey& key) {
    return uint32_t(prHashBytes(&key, sizeof(key), 0));
}

// An image that a writer embeds: texels in memory, of a texture on the writer's device or of what it rasterized.
struct DrawSharedImage {
    DrawSharedImageKey key;
    PrPixelView pixels;
    PrPixelFormat format;       // Rgba8, Bgra8 or RgbaF16
    PrTextureAlpha alpha;
};

// Writes the image's pixels to `out` as straight-alpha sRGB Rgba8, clamped to [0, 1]. Returns true if any pixel is not
// opaque.
bool drawSharedImageRgba8(const DrawSharedImage& image, RawArray<uint8_t>& out);

// A color of a gradient at an offset along its parameter, as a format's stops hold it.
struct DrawSharedStop {
    float offset;               // in [0, 1], nondecreasing from one stop to the next
    PrColor color;              // straight alpha, clamped to [0, 1]
};

// Writes to `out` the stops that a format interpolating straight colors in sRGB between them draws the gradient with,
// times `color`, from 0 to 1. A stop that changes color abruptly is two stops at one offset.
void drawSharedGradientStops(const PrDrawBrush* brush, PrColor color, RawArray<DrawSharedStop>& out);

// ---------------------------------------------------------------------------------------------------------------------
// The walk

// How a format paints a fill or a stroke: a color, or the color times a gradient that the format draws natively.
struct DrawSharedPaint {
    PrColor color;                          // straight alpha, which multiplies the brush
    const PrDrawBrush* PR_NULLABLE brush;   // a gradient, or null
    PrAffine2 brushToUser;                  // the brush's space to the space of the geometry, with a brush
    PrDrawBlendMode blendMode;
};

// A group that a format draws natively: its contents composite source-over into a layer, which composites onto what
// lies beneath with the group's opacity and blend mode.
struct DrawSharedGroup {
    float opacity;
    PrDrawBlendMode blendMode;
    bool isolated;
    bool knockout;
    PrRect bounds;                          // the area that the group may cover, in page px, never empty
};

// The operations of a format, which a walk calls in paint order. Geometry is given with the transform from its space to
// the page, in px with y down. Each push is closed by a pop of its kind, in nesting order.
struct DrawSharedWriterOps {
    void* PR_NULLABLE context;              // passed to every operation

    // What the format draws natively. A walk rasterizes what the format does not draw.
    bool knockout;                          // knockout groups
    bool nonScalingStrokes;                 // strokes measured in page px whatever the transform
    bool (*gradient)(void* PR_NULLABLE context, const PrDrawBrush* brush);
    bool (*blendMode)(void* PR_NULLABLE context, PrDrawBlendMode blendMode);

    // Fills a path by `rule`, or strokes it. A stroke is centered, maps its brush plainly, has one width along its
    // length, and caps its dashes as it caps its ends.
    void (*fill)(void* PR_NULLABLE context, const PrPath* path, PrPathFillRule rule, PrAffine2 transform,
        const DrawSharedPaint* paint);
    void (*stroke)(void* PR_NULLABLE context, const PrPath* path, const PrDrawStroke* stroke, PrAffine2 transform,
        const DrawSharedPaint* paint);

    // Draws the texels of `source` stretched over `destination`, with the image's alpha, times `opacity`.
    void (*image)(void* PR_NULLABLE context, const DrawSharedImage& image, PrRect source, PrRect destination,
        PrAffine2 transform, float opacity);

    // Draws a run whose glyphs are outlines, filled, or stroked by `stroke` as `stroke` describes.
    void (*glyphs)(void* PR_NULLABLE context, const PrFontGlyphRun* run, PrAffine2 transform,
        const PrDrawStroke* PR_NULLABLE stroke, const DrawSharedPaint* paint);

    void (*pushClip)(void* PR_NULLABLE context, const PrPath* path, PrPathFillRule rule, PrAffine2 transform);
    void (*popClip)(void* PR_NULLABLE context);
    void (*pushGroup)(void* PR_NULLABLE context, const DrawSharedGroup* group);
    void (*popGroup)(void* PR_NULLABLE context);

    // Opens or closes a mark. `bounds` is the area, in page px, that the mark's content covers, or empty.
    void (*pushMark)(void* PR_NULLABLE context, uint64_t key);
    void (*popMark)(void* PR_NULLABLE context, uint64_t key, PrRect bounds);
};

// A mark that a walk has open: its key, and the area of the page that its content covers so far.
struct DrawSharedMark {
    uint64_t key;
    PrRect bounds;
};

// A transform of the walk from the page's root: a transform push, or the offset of an embedded list.
struct DrawSharedStep {
    const PrDrawCommand* PR_NULLABLE push;
    PrDVec2 offset;                         // the offset of an embedded list, if `push` is null
};

// The state of a writer's walks, which it keeps from one page to the next.
struct DrawSharedWalker {
    DrawSharedWriterOps ops;
    PrDrawRenderer* cpu;
    float rasterScale;                      // target pixels per px of what the walk rasterizes

    // Scratch, kept for its storage.
    RawArray<DrawSharedStep> steps;         // the transforms in effect, from the page's root
    RawArray<PrRect> clips;                 // the areas of the page that the clips in effect let through
    RawArray<PrDrawTransform> transforms;
    RawArray<DrawSharedMark> marks;         // innermost last
    RawArray<uint8_t> pixels;
    PrPath* path;
    PrPathPolyline polyline;
    PrPathPolyline aligned;
    PrPathPolyline region;
    PrVec2 page;
};

// Prepares a walker that calls `ops`. The walker rasterizes with a renderer on `device`, or on a device of its own if
// `device` is null. The renderer decodes picture glyphs with `glyphDecoder`. The walker reads the textures that the
// pages draw from `device`.
void drawSharedWalkerInit(DrawSharedWalker& walker, const DrawSharedWriterOps& ops, PrGpuDevice* PR_NULLABLE device,
    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder, float rasterScale);

// Frees what the walker holds.
void drawSharedWalkerReset(DrawSharedWalker& walker);

// Walks `list`, drawn on a page of `extent` px, through the walker's operations.
void drawSharedWalk(DrawSharedWalker& walker, const PrDrawList* list, PrVec2 extent);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
