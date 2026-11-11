// The fragment renderer's frame: the records that preparing a list builds, which the renderer uploads and its shaders
// evaluate for each pixel.
//
// Positions are in the target's pixels with y down, including in a layer. Colors, ramps and texels are premultiplied
// extended sRGB, encoded, and blend as they are stored. A frame for an 8-bit target holds colors in [0, 1]. A frame for
// an RgbaF16 target keeps colors outside that range, and draws every layer in half floats.
//
// This header defines the frame and what the shaders (draw_fragment.slang) compute from it. The constants and the
// records that the shaders read are draw_fragment_constants.h's and draw_fragment_records.h's, which the shaders
// include too.

#pragma once

#include "pr/base.h"
#include "pr/draw_fragment.h"
#include "pr/draw_glyph.h"
#include "pr/draw_list.h"
#include "pr/draw_lower.h"
#include "pr/draw_renderer.h"
#include "pr/float.h"
#include "pr/geom.h"
#include "pr/pixels.h"
#include "pr/vec.h"

#include "draw/backends/fragment/src/draw_fragment_constants.h"
#include "draw/backends/fragment/src/draw_fragment_records.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A surface over a rectangle of the target's pixels. The first pass into a layer clears it to transparent, and only
// later passes read it. The frame's beneath layer has no such pass. A precise layer holds half floats. Any other layer
// holds 8 bits per channel. Every layer of a frame for an RgbaF16 target is precise.
//
// The layer's pixel (0, 0) is its texture's texel (0, 0). Its texture may be larger than its extent, and the texels
// beyond the extent are transparent, so a sampler that filters across the layer's edge reads transparent texels there.
struct FragmentLayer {
    uint32_t texture;               // its texture's number
    PrIVec2 origin;                 // its position and extent in the target's pixels
    PrUVec2 extent;
    bool precise;
    bool exact;                     // true if its texture is exactly its extent, so a sampler wraps at its edges

    // True if its texture is exactly its extent, with every mip level down to one texel. A pass with `mipmaps` fills
    // the levels after the first.
    bool mipmapped;

    // The host's texture that is the layer's texture, as its index in the frame's `hostTextures` plus one, or zero for
    // a texture of the driver's. The host's texture keeps what it holds: a pass into it loads it, and the texture is at
    // the layer's origin, at its extent.
    uint32_t hostTexture;
};

// A run of the frame's instances drawn into the target or a layer. A pass with a filter instead carries out the filter
// step over its instances' pixels, and the result replaces what the layer held there. A pass with `mipmaps` has no
// instances. It fills each mip level of its mipmapped layer after the first from the level before it, by the device's
// filter.
//
// On a device that cannot read its target, an instance that reads the destination reads a snapshot of its layer
// instead: a layer of its own at the same place, into which a copy pass copies a box of the layer between the passes
// that draw into it. The instances that read one copy cover disjoint pixels within its box, so that each reads what the
// instances before the copy left. The list's own layer is never the target on such a device, since a list that reads
// the destination draws into a layer of its own (prDrawListReadsDestination).
struct FragmentPass {
    uint32_t layer;                 // the layer's index plus one, or zero for the target
    bool clear;                     // whether the layer is cleared to transparent first
    bool beneath;                   // whether the host first draws what lies beneath the target into the layer
    bool mipmaps;
    uint32_t filter;                // the filter step's index plus one, or zero for instances that paint
    uint32_t first;                 // the index of its first instance
    uint32_t count;

    // A copy pass copies `box`, in the target's pixels, from the layer into the snapshot layer `copy`, as its index
    // plus one, and draws nothing. A pass that draws reads the snapshot layer `snapshot`, as its index plus one, where
    // its instances read the destination, or reads the target if it is zero.
    uint32_t copy;
    PrIRect box;
    uint32_t snapshot;
};

// The pixels of the pass's attachment that the instance can draw, within `limit`: the bounds of its corners, moved by
// `offset` from the target's pixels to the attachment's, and widened by a pixel. An instance whose corners are not
// finite may draw anywhere within `limit`.
inline PrIRect fragmentInstancePixels(const FragmentInstance& i, PrVec2 offset, const PrIRect& limit) {
    PrVec2 corners[4];
    uint32_t count = 0;
    if ((i.flags & FragmentInstanceFlag_Coverage) == FragmentCoverage_Triangle) {
        for (uint32_t k = 0; k < 3; ++k)
            corners[count++] = i.corners[k];
    } else {
        const PrAffine2& m = i.transform;
        const float xs[2] = {i.bounds.left, i.bounds.right};
        const float ys[2] = {i.bounds.top, i.bounds.bottom};
        for (float y : ys) {
            for (float x : xs)
                corners[count++] = PrVec2{m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f};
        }
    }
    float lo[2] = {corners[0].x, corners[0].y};
    float hi[2] = {corners[0].x, corners[0].y};
    for (uint32_t k = 0; k < count; ++k) {
        const float p[2] = {corners[k].x, corners[k].y};
        for (uint32_t axis = 0; axis < 2; ++axis) {
            // A comparison with NaN is false, so a corner that is not finite fails both tests and the limit stands.
            if (!(p[axis] > -1e9f && p[axis] < 1e9f))
                return limit;
            lo[axis] = p[axis] < lo[axis] ? p[axis] : lo[axis];
            hi[axis] = p[axis] > hi[axis] ? p[axis] : hi[axis];
        }
    }
    PrIRect box{
        int32_t(prF32Floor(lo[0] + offset.x)) - 1, int32_t(prF32Floor(lo[1] + offset.y)) - 1,
        int32_t(prF32Ceil(hi[0] + offset.x)) + 1, int32_t(prF32Ceil(hi[1] + offset.y)) + 1,
    };
    return prIRectIntersect(box, limit);
}

// What a texture request does.
enum {
    FragmentTextureOp_Create,     // creates a texture of the texels' size, holding them
    FragmentTextureOp_Update,     // replaces the texels of a region
    FragmentTextureOp_Destroy,    // destroys the texture
};
typedef uint8_t FragmentTextureOp;

// A change to a texture. Textures are named by numbers, and a number is never reused. A texture is destroyed only after
// it has gone unused for several prepares.
struct FragmentTextureRequest {
    FragmentTextureOp op;

    // Create: Rgba8 or RgbaF16, premultiplied and read without conversion, or R8 for coverage.
    PrPixelFormat format;

    // Create: true if the texture has every mip level down to one texel, which the driver fills from the first level by
    // the device's filter.
    bool mipmapped;

    uint32_t texture;                       // the texture's number
    PrUVec2 origin;                         // Update: the region's top left
    PrPixelView texels;                     // Create: the size and the texels; Update: the region's texels
};

// The data that preparing builds for the driver. The driver carries out the requests first. It then runs the passes in
// order: the passes into layers while preparing, and the passes into the target while rendering. A frame stays valid
// until the PrDrawFrame that holds it is destroyed. The requests' texels stay valid until the next prepare.
struct FragmentFrame {
    PrUVec2 extent;                         // the target's extent in pixels
    const FragmentTextureRequest* PR_NULLABLE requests;
    uint32_t requestCount;
    const FragmentTexture* PR_NULLABLE textures;
    uint32_t textureCount;
    const PrDrawTextureId* PR_NULLABLE hostTextures;  // the host's textures that the frame reads
    uint32_t hostTextureCount;

    // The gradients' ramps. A ramp's table holds its colors at the kFragmentRampResolution + 1 even steps of the
    // parameter from 0 to 1, premultiplied, which a shader interpolates between within a step. A ramp's cells hold, for
    // each step, zero if no two of its pieces meet within the step or at its end, and otherwise the index plus one in
    // `rampPieces` of the piece that holds the step's start, then the same for its pieces before 0 and past 1. Such a
    // point takes its color from the ramp's pieces (prDrawRampPieceAt, prDrawRampColor), each ramp's in order, and the
    // colors that they name.
    const PrColor16* PR_NULLABLE rampTables;
    const uint32_t* PR_NULLABLE rampCells;
    uint32_t rampCount;
    const PrDrawRampPiece* PR_NULLABLE rampPieces;
    uint32_t rampPieceCount;
    const PrColor16* PR_NULLABLE rampColors;
    uint32_t rampColorCount;

    const FragmentInstance* PR_NULLABLE instances;
    uint32_t instanceCount;
    const float* PR_NULLABLE values;        // the records that coverage from Rect to Shadow reads
    uint32_t valueCount;
    const uint8_t* PR_NULLABLE coverage;    // the bytes that Bytes instances read
    uint32_t coverageCount;
    const float* PR_NULLABLE tileData;      // the tile records that Tiles instances read
    uint32_t tileDataCount;
    const FragmentPaint* PR_NULLABLE paints;
    uint32_t paintCount;
    const FragmentClip* PR_NULLABLE clips;
    uint32_t clipCount;
    const uint32_t* PR_NULLABLE clipShapes; // indices into `shapes`
    uint32_t clipShapeCount;
    const FragmentClipShape* PR_NULLABLE shapes;
    uint32_t shapeCount;
    const FragmentPiece* PR_NULLABLE pieces;
    uint32_t pieceCount;
    const FragmentMap* PR_NULLABLE maps;
    uint32_t mapCount;
    const float* PR_NULLABLE mapData;       // the maps' segments and profiles
    uint32_t mapDataCount;
    const uint32_t* PR_NULLABLE mapIndices; // the maps' cell starts and segment numbers
    uint32_t mapIndexCount;
    const FragmentFilter* PR_NULLABLE filters;
    uint32_t filterCount;
    const float* PR_NULLABLE filterData;
    uint32_t filterDataCount;
    const FragmentLayer* PR_NULLABLE layers;
    uint32_t layerCount;
    const FragmentPass* PR_NULLABLE passes;
    uint32_t passCount;

    // The layer that holds what lies beneath the target, as its index plus one, or zero. Its texture is the host's
    // texture (PrDrawBeneath), and no pass draws into it.
    uint32_t beneath;
};

} // namespace pr_impl

PR_ASSUME_NONNULL_END
