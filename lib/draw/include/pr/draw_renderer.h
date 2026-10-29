// Draw renderers: draw lists rendered into a target, which is the host's pixels in memory or a texture on a GPU device
// (pr/gpu.h).

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/draw_glyph.h"
#include "pr/draw_list.h"
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
// size of the render, not its position. On a device, preparing does the work that cannot happen in a render pass: it
// records the offscreen passes of groups and filters into the host's command buffer, outside any pass. A renderer in
// memory does no work while preparing. Rendering the frame then draws over what the target holds at the render's
// position. That position is in the host's pixels, or a rectangle of a pass that the host opens in the same command
// buffer after preparing. The host may draw anything into its pass before and after rendering, and clears what it wants
// cleared.
//
// On a device, a texture of the host's that a frame reads or draws into is in the accesses of
// PrGpuAccessFlag_ShaderRead when the frame is prepared, and stays in them after. The host submits the command buffers
// that frames record in the order that it prepared the frames.
//
// A frame is created by one renderer and used only with that renderer. It keeps its storage from one prepare to the
// next.
typedef struct PrDrawFrame PrDrawFrame;

// Redraws what lies beneath a render on a device, over `area` of the render's pixels. `pass` is a pass that the
// renderer owns, just begun over a transparent texture. The texture has the format of the render's pass, and the top
// left of `area` at its origin. The function may set any state of the pass, and the renderer then draws over it in the
// same pass. Called while preparing, once for each region that backdrops read.
typedef void (*PrDrawBeneathFn)(void* PR_NULLABLE context, PrGpuPass* pass, PrIRect area);

// Returns the texture id of a texture on the device of a renderer that renders there.
static inline PrDrawTextureId prDrawGpuTexture(PrGpuTexture texture) {
    return texture.id;
}

// A texture of the host's in CPU memory, for a renderer in memory. Its pixels are premultiplied extended sRGB, encoded.
// The host keeps the texture and its pixels alive while frames draw it.
typedef struct PrDrawCpuTexture {
    PrPixels pixels;
    PrPixelFormat format;
} PrDrawCpuTexture;

// Returns the texture id of a texture in CPU memory.
static inline PrDrawTextureId prDrawCpuTexture(PrDrawCpuTexture* texture) {
    return PR_VALUE_CAST(PrDrawTextureId, PR_POINTER_CAST(uintptr_t, texture));
}

// Where a render on a device finds what lies beneath it.
enum {
    PrDrawBeneathKind_Color,        // a solid color
    PrDrawBeneathKind_Texture,      // a texture that the renderer samples
    PrDrawBeneathKind_Draw,         // a function that draws it again
};
typedef uint8_t PrDrawBeneathKind;

// What lies beneath a render on a device, for a backdrop to read. Its kind decides which fields it uses. A
// zero-initialized value is a transparent color. A render in memory reads what its pixels held.
typedef struct PrDrawBeneath {
    PrDrawBeneathKind kind;
    PrColor16 color;                            // Color: straight alpha, like the colors in a list

    // Texture: sampled with the render's pixel (0, 0) at `origin`, `extent` texels in size. It is not the texture that
    // the render's pass draws into. The host keeps it alive and unchanged until the render's commands are done.
    PrDrawTextureId texture;
    PrUVec2 extent;
    PrIVec2 origin;

    PrDrawBeneathFn PR_NULLABLE draw;           // Draw
    void* PR_NULLABLE context;                  // Draw: passed to `draw`
} PrDrawBeneath;

// What a frame renders into: the extent of the render, the format of its pixels and, on a device, where the render
// draws and what lies beneath it.
//
// A render blends extended sRGB as encoded, and its target holds extended sRGB, encoded. An 8-bit target clamps colors
// to [0, 1], and an RgbaF16 target keeps colors outside that range. The renderer maps no colors for a display. A host
// that shows the render on a display tone maps it, maps it into the display's gamut and converts it to the display's
// encoding where those differ from the render's.
typedef struct PrDrawTarget {
    PrUVec2 extent;                             // the render's extent in pixels
    PrPixelFormat format;                       // device: the format of the texture that the render's pass draws into

    // device: the host's commands, which the render's pass records into after preparing.
    PrGpuCommands* PR_NULLABLE commands;

    const PrDrawBeneath* PR_NULLABLE beneath;   // device: null for transparent
} PrDrawTarget;

// The fields at the start of every frame's state.
struct PrDrawFrame {
    PrDrawRenderer* renderer;
    PrUVec2 extent;             // the extent of the render
    bool prepared;              // true if the last prepare succeeded
};

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

// The functions of a renderer, at the start of its backend's state. Each one carries out the function that it is named
// for. A renderer on a device has `frameRender`, and a renderer in memory has `frameRenderPixels`. `frameStats` is null
// for a renderer that counts nothing. `dropResources` is null for a renderer that keeps nothing on a device.
struct PrDrawRenderer {
    void (*PR_NULLABLE destroy)(PrDrawRenderer* renderer);
    PrDrawFrame* PR_NONNULL (*PR_NONNULL frameCreate)(PrDrawRenderer* renderer);
    void (*frameDestroy)(PrDrawRenderer* renderer, PrDrawFrame* frame);
    PrStatus (*framePrepare)(PrDrawRenderer* renderer, PrDrawFrame* frame, const PrDrawList* list,
                             const PrDrawTarget* target);
    void (*PR_NULLABLE frameRender)(PrDrawRenderer* renderer, PrDrawFrame* frame, PrGpuPass* pass, PrIVec2 origin);
    void (*PR_NULLABLE frameRenderPixels)(PrDrawRenderer* renderer, PrDrawFrame* frame, PrPixels pixels,
                                          PrPixelFormat format);
    void (*PR_NULLABLE frameStats)(PrDrawRenderer* renderer, const PrDrawFrame* frame, PrDrawStats* out);
    void (*PR_NULLABLE dropResources)(PrDrawRenderer* renderer);

    // True if `frameRenderPixels` renders into RgbaF16. Without it, the interface renders into Rgba8 and widens the
    // result.
    bool rendersHalfFloat;
};

// The library's backends of renderers, which the build turns on (README.md), and Default: the fragment renderer, or the
// triangles renderer, given a device, and the CPU renderer without one.
enum {
    PrDrawRendererBackend_Default,
    PrDrawRendererBackend_Cpu,
    PrDrawRendererBackend_Triangles,
    PrDrawRendererBackend_Fragment,
    PrDrawRendererBackend_Count,
};
typedef uint8_t PrDrawRendererBackend;

// Returns the backend that `name` names, as prDrawRendererBackendName spells it, ignoring case, or Count if it names
// none.
PrDrawRendererBackend prDrawRendererBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prDrawRendererBackendName(PrDrawRendererBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prDrawRendererBackendIsBuilt(PrDrawRendererBackend backend);

// Returns true if the backend renders on a GPU device.
bool prDrawRendererBackendIsDevice(PrDrawRendererBackend backend);

// How prDrawRendererCreate creates a renderer: what every backend takes. The rest of a backend's own options keep their
// defaults.
typedef struct PrDrawRendererOptions {
    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder;   // draws the glyphs that are pictures; without it, they draw nothing
} PrDrawRendererOptions;

// Returns options with no glyph decoder.
PrDrawRendererOptions prDrawRendererOptionsDefault(void);

// Creates a renderer by the backend, on `device` if the backend renders on one. Null options mean the defaults. Returns
// NotSupported if the build does not have the backend, if the backend renders on a device and `device` is null, or if
// the device cannot run the renderer's shaders, and Lost if the device is lost. On failure, `*out` is null.
PrStatus prDrawRendererCreate(PrDrawRendererBackend backend, PrGpuDevice* PR_NULLABLE device,
                              const PrDrawRendererOptions* PR_NULLABLE options,
                              PrDrawRenderer* PR_NULLABLE* PR_NONNULL out);

// Frees the renderer. Its frames must be destroyed first.
void prDrawRendererDestroy(PrDrawRenderer* PR_NULLABLE renderer);

// Returns true if the renderer renders on a device, into the host's passes, instead of into pixels in memory.
bool prDrawRendererIsOnDevice(const PrDrawRenderer* renderer);

// Releases every resource that the renderer created on the device for its frames, such as textures and buffers, as a
// host does to give the device's memory back. The next prepare recreates the resources that it draws with. The
// renderer's frames must be destroyed first. Does nothing for a renderer in memory. A lost device is not recovered this
// way: its renderer is destroyed with it.
void prDrawRendererDropResources(PrDrawRenderer* renderer);

// The maximum side length of a render, in pixels.
enum { kPrDrawRenderMaxSize = 16384 };

// Creates a frame with nothing prepared, or frees one.
PrDrawFrame* prDrawFrameCreate(PrDrawRenderer* renderer);
void prDrawFrameDestroy(PrDrawFrame* PR_NULLABLE frame);

// Prepares the list's commands, in order, as a render of the extent in `target`, and replaces what the frame held. The
// list must have no open pushes, and must stay unchanged until the frame is prepared again or destroyed. For a renderer
// on a device, `target` must have `commands`, and `target->beneath` must set the fields that its kind uses. Returns
// Limit for a render larger than kPrDrawRenderMaxSize on a side, and NotSupported for a target of a format other than
// Rgba8, Bgra8 and RgbaF16. On failure, the frame has nothing prepared.
PrStatus prDrawFramePrepare(PrDrawFrame* frame, const PrDrawList* list, const PrDrawTarget* target);

// Returns the counts of the work that preparing the frame produced. All counts are zero for a renderer that counts
// nothing.
PrDrawStats prDrawFrameStats(const PrDrawFrame* frame);

// Draws the prepared frame of a renderer on a device into the host's pass, in the command buffer that preparing
// recorded into. It draws over the rectangle of the frame's size at `origin` of the pass's attachments, which must lie
// within them. Leaves the pass's viewport and scissor over the whole attachments, and any other state as the renderer
// set it.
void prDrawFrameRender(PrDrawFrame* frame, PrGpuPass* pass, PrIVec2 origin);

// Draws the prepared frame of a renderer in memory into `pixels`, which are the frame's size, premultiplied extended
// sRGB, encoded, in Rgba8, Bgra8 or RgbaF16, whatever the prepared target's `format`.
void prDrawFrameRenderPixels(PrDrawFrame* frame, PrPixels pixels, PrPixelFormat format);

// Prepares the list and renders it into `pixels`, for a renderer in memory. Returns what preparing returns.
PrStatus prDrawRendererRenderPixels(PrDrawRenderer* renderer, const PrDrawList* list, PrPixels pixels,
                                    PrPixelFormat format);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
