// The triangles renderer's frame: the textured, colored triangles that preparing a list builds, which the renderer
// uploads and draws.

#pragma once

#include "pr/base.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/draw_triangles.h"
#include "pr/geom.h"
#include "pr/pixels.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A vertex, laid out as Dear ImGui's ImDrawVert.
PR_NO_PADDING_BEGIN
struct TrianglesVertex {
    PrVec2 position;           // in the target's pixels, y down
    PrVec2 uv;                 // in fractions of the texture's size
    uint32_t color;            // straight RGBA, with red in the lowest byte
};
PR_NO_PADDING_END

// How a draw samples its texture. Linear or Nearest selects how a point between texels reads them. Clamp, Repeat or
// Mirror selects what lies beyond the texture's edges. Clamp extends the edge texels, Repeat repeats the texture, and
// Mirror repeats it with every other copy mirrored.
enum {
    TrianglesSampler_LinearClamp,
    TrianglesSampler_NearestClamp,
    TrianglesSampler_LinearRepeat,
    TrianglesSampler_NearestRepeat,
    TrianglesSampler_LinearMirror,
    TrianglesSampler_NearestMirror,
    TrianglesSampler_Count,
};
typedef uint8_t TrianglesSampler;

// A run of the frame's indices, drawn as triangles with one texture, sampler and scissor, regardless of winding.
//
// A pixel's color is the interpolated vertex color times the texture sampled at the interpolated uv. The color blends
// over the target by source alpha and one minus source alpha, and the alpha by one and one minus source alpha. On a
// target that holds premultiplied values, the result is premultiplied. Pixels outside `scissor` are left alone.
struct TrianglesDraw {
    PrDrawTextureId hostTexture;           // the host's texture, sampled as straight alpha, or zero to use `texture`
    uint32_t texture;                      // the number of a texture that the requests created
    TrianglesSampler sampler;
    PrIRect scissor;                  // within the target
    uint32_t indexOffset;
    uint32_t indexCount;
};

// What a texture request does.
enum {
    TrianglesTextureOp_Create,    // creates a texture of the texels' size, holding them
    TrianglesTextureOp_Update,    // replaces the texels of a region
    TrianglesTextureOp_Destroy,   // destroys the texture
};
typedef uint8_t TrianglesTextureOp;

// A change to a texture. A texture is 8-bit RGBA with straight alpha, sRGB-encoded, and is sampled without conversion.
// Textures are named by numbers, and a number is never reused. A texture is destroyed only after it has gone unused for
// several prepares, so that the requests of the next frame never change the textures that a frame draws with.
struct TrianglesTextureRequest {
    TrianglesTextureOp op;
    uint32_t texture;                       // the texture's number
    PrUVec2 origin;                         // Update: the region's top left
    PrPixelView texels;                     // Create: the size and the texels; Update: the region's texels
};

// The data that preparing builds for the driver. The renderer carries out the requests in order, then the draws in
// order. The indices index `vertices` from its start. A frame stays valid until the PrDrawFrame that holds it is
// destroyed. The requests' texels stay valid until the next prepare.
struct TrianglesFrame {
    PrUVec2 extent;                         // the target's extent in pixels
    const TrianglesTextureRequest* PR_NULLABLE requests;
    uint32_t requestCount;
    const TrianglesVertex* PR_NULLABLE vertices;
    uint32_t vertexCount;
    const uint32_t* PR_NULLABLE indices;
    uint32_t indexCount;
    const TrianglesDraw* PR_NULLABLE draws;
    uint32_t drawCount;
};

} // namespace pr_impl

PR_ASSUME_NONNULL_END
