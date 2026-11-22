// Draw renderers, which render draw lists into a texture on a GPU device (pr/gpu.h). A renderer draws with the device's
// shaders, or on the CPU on a memory device (PrGpuNativeKind_Pixels).

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw_glyph.h"
#include "pr/draw_list.h"
#include "pr/draw_transform.h"
#include "pr/geom.h"
#include "pr/gpu.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A renderer of draw lists, created by prDrawRendererCreate or by its backend's own constructor.
typedef struct PrDrawRenderer PrDrawRenderer;

// A list prepared for rendering. Rendering takes two steps. Preparing a list fills the frame, and is given only the
// size of the render, not its position. Preparing does the work that cannot happen in a render pass: it records the
// offscreen passes of groups and filters into the caller's command buffer, outside any pass. Rendering the frame then
// draws over what the target holds at the render's position, which is a rectangle of a pass that the caller opens in
// the same command buffer after preparing. The caller may draw anything into its pass before and after rendering, and
// clears what it wants cleared.
//
// A texture of the caller's that a frame reads or draws into must be in the accesses of PrGpuAccessFlag_ShaderRead when
// the frame is prepared, and stays in them after. The caller submits the command buffers that frames record in the
// order that it prepared the frames. It submits a frame's command buffer before it prepares the frame again, because a
// renderer may draw while the work runs, as the CPU renderer does.
//
// A frame is created by one renderer and used only with that renderer. It keeps its storage from one prepare to the
// next.
typedef struct PrDrawFrame PrDrawFrame;

// Redraws what lies beneath a render, over `area` of the render's pixels. `pass` is a pass that the renderer owns, just
// begun over a transparent texture. The texture has the format of the render's pass, and the top left of `area` at its
// origin. The function may set any state of the pass, and the renderer then draws over it in the same pass. Called
// while preparing, once for each region that backdrops read.
typedef void (*PrDrawBeneathFn)(void* PR_NULLABLE context, PrGpuPass* pass, PrIRect area);

// Where a render finds what lies beneath it.
enum {
    PrDrawBeneathKind_Color,        // a solid color
    PrDrawBeneathKind_Texture,      // a texture that the renderer samples
    PrDrawBeneathKind_Draw,         // a function that draws it again
};
typedef uint8_t PrDrawBeneathKind;

// What lies beneath a render, for a backdrop to read. Its kind decides which fields it uses. A zero-initialized value
// is a transparent color. A renderer that draws through a native object of the target, such as the Canvas 2D renderer,
// reads what the target holds instead.
typedef struct PrDrawBeneath {
    PrDrawBeneathKind kind;
    PrColor16 color;                            // Color: straight alpha, like the colors in a list

    // Texture: sampled with the render's pixel (0, 0) at `origin`, `extent` texels in size. It is not the texture that
    // the render's pass draws into. The host keeps it alive and unchanged until the render's commands are done.
    PrGpuTexture texture;
    PrUVec2 extent;
    PrIVec2 origin;

    PrDrawBeneathFn PR_NULLABLE draw;           // Draw
    void* PR_NULLABLE context;                  // Draw: passed to `draw`
} PrDrawBeneath;

// What a frame renders into: the extent of the render, the format of its pixels, the commands that the render records
// into, and what lies beneath it.
//
// A render blends encoded extended sRGB, and its target holds encoded extended sRGB. An 8-bit target clamps colors to
// [0, 1], and an RgbaF16 target keeps colors outside that range. The renderer maps no colors for a display. A caller
// that shows the render on a display tone maps it, maps it into the display's gamut, and converts it to the display's
// encoding, if those differ from the render's.
typedef struct PrDrawTarget {
    PrUVec2 extent;                             // the render's extent in pixels
    PrPixelFormat format;                       // the format of the texture that the render's pass draws into

    // The caller's commands. The render's pass records into them after preparing.
    PrGpuCommands* commands;

    const PrDrawBeneath* PR_NULLABLE beneath;   // null for transparent
} PrDrawTarget;

// A part of a list that a render plays: `count` of the list's commands from `first`, which close every scope that they
// open. `transform` maps the commands' space to the render's pixels. It is the transform that a walk of the list holds
// where the commands lie, placed in the render, so that the commands snap as they do in place. No clip or group around
// the commands plays, and commands in target pixels are in the render's pixels. A part lets a consumer of a list render
// a group or a command of it into pixels of its own, as a writer rasterizes what its format cannot draw, without
// copying the commands into another list.
typedef struct PrDrawPart {
    uint32_t first;
    uint32_t count;
    PrDrawTransform transform;
} PrDrawPart;

// Counts of the work that preparing a frame produced, for measuring a renderer. A renderer fills the counts that apply
// to how it renders, and leaves the rest zero.
typedef struct PrDrawStats {
    uint32_t passes;            // render passes on the device, including the target's pass
    uint32_t layers;            // offscreen surfaces drawn into
    uint32_t draws;             // draw calls on the device
    uint32_t primitives;        // triangles and instances drawn
    uint32_t records;           // records that the shaders read for each draw, such as paint and clips
    uint32_t pathFills;         // paths whose coverage the CPU prepared, tessellated or rasterized
    uint64_t uploadBytes;       // bytes copied to the device: the frame's data and texels
} PrDrawStats;

// A backend's own constructor creates a renderer (pr/draw_cpu.h, pr/draw_fragment.h, pr/draw_triangles.h,
// pr/draw_canvas.h). A program can also create one through the selector (pr/draw_select.h).

// Options that every renderer backend takes. A backend's other options keep their defaults.
typedef struct PrDrawRendererOptions {
    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder;   // draws the glyphs that are pictures; without it, they draw nothing
} PrDrawRendererOptions;

// Returns options with no glyph decoder.
PrDrawRendererOptions prDrawRendererOptionsDefault(void);

// Frees the renderer. Its frames must be destroyed first.
void prDrawRendererDestroy(PrDrawRenderer* PR_NULLABLE renderer);

// Returns the name of the renderer's backend in lowercase, such as "fragment".
PrStringView prDrawRendererName(const PrDrawRenderer* renderer);

// Returns the device that the renderer renders on.
PrGpuDevice* prDrawRendererDevice(const PrDrawRenderer* renderer);

// Releases every resource that the renderer created on the device for its frames, such as textures and buffers, so that
// the device's memory is given back. The next prepare recreates the resources that it draws with. The renderer's frames
// must be destroyed first. A lost device is not recovered this way: its renderer is destroyed with it.
void prDrawRendererDropResources(PrDrawRenderer* renderer);

// The maximum side length of a render, in pixels.
enum { kPrDrawRenderMaxSize = 16384 };

// Creates a frame with nothing prepared, or frees one.
PrDrawFrame* prDrawFrameCreate(PrDrawRenderer* renderer);
void prDrawFrameDestroy(PrDrawFrame* PR_NULLABLE frame);

// Prepares the list's commands, in order, as a render of the extent in `target`, and replaces what the frame held. The
// list must have no open pushes, and must stay unchanged until the frame is prepared again or destroyed. `target` must
// have `commands`, and `target->beneath` must set the fields that its kind uses. Returns Limit for a render larger than
// kPrDrawRenderMaxSize on a side, and NotSupported for a target of a format other than Rgba8, Bgra8 and RgbaF16. On
// failure, the frame has nothing prepared.
PrStatus prDrawFramePrepare(PrDrawFrame* frame, const PrDrawList* list, const PrDrawTarget* target);

// Prepares a part of the list as prDrawFramePrepare prepares a whole list, and returns what it returns. The part lies
// within the list's commands.
PrStatus prDrawFramePreparePart(PrDrawFrame* frame, const PrDrawList* list, const PrDrawPart* part, const PrDrawTarget* target);

// Returns the counts of the work that preparing the frame produced. All counts are zero for a renderer that counts
// nothing.
PrDrawStats prDrawFrameStats(const PrDrawFrame* frame);

// Draws the prepared frame into the caller's pass, in the command buffer that preparing recorded into. It draws over
// the rectangle of the frame's size at `origin` of the pass's attachments, which must lie within them. Leaves the
// pass's viewport and scissor over the whole attachments, and any other state as the renderer set it.
void prDrawFrameRender(PrDrawFrame* frame, PrGpuPass* pass, PrIVec2 origin);

// Renders the list over the caller's `pixels` in place, on a renderer whose device draws into pixels
// (PrGpuNativeKind_Pixels), such as a memory device. The pixels hold the render when the call returns. They hold
// premultiplied, encoded, extended sRGB, in Rgba8, Bgra8 or RgbaF16. Returns NotSupported for a renderer on any other
// device, and otherwise what preparing returns.
PrStatus prDrawRendererRenderPixels(PrDrawRenderer* renderer, const PrDrawList* list, PrPixels pixels, PrPixelFormat format);

// Renders a part of the list over `pixels` as prDrawRendererRenderPixels renders a whole list, and returns what it
// returns.
PrStatus prDrawRendererRenderPartPixels(PrDrawRenderer* renderer, const PrDrawList* list, const PrDrawPart* part,
    PrPixels pixels, PrPixelFormat format);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
