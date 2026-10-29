// The fragment renderer's work on a GPU device: the textures that frames request, the layers that they draw into, and
// the passes that draw their instances.

#pragma once

#include "pr/base.h"
#include "pr/draw_renderer.h"
#include "pr/gpu.h"

#include "draw/backends/fragment/src/draw_fragment_frame.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What the renderer keeps on a device: its pipelines and samplers, the textures that frames requested, the pool of
// layers, and the resources of each prepared frame.
struct FragmentDevice;

// Creates the renderer's state on `device`. Returns NotSupported if the device cannot run the renderer's shaders, and
// Lost if it is lost.
PrStatus fragmentDeviceCreate(PrGpuDevice* device, FragmentDevice* PR_NULLABLE* PR_NONNULL out);

// Frees the state. The device may still be executing work that reads it.
void fragmentDeviceDestroy(FragmentDevice* PR_NULLABLE device);

// Carries out the frame's requests and its passes into layers, recording into `commands`, outside any pass. The
// render's pass draws into a texture of `format`. `beneath` gives the texture of the frame's beneath layer, or the
// function that draws into the layers of passes marked beneath. The frame's data lives in the command buffer's stream,
// so the frame renders into a pass of the same command buffer.
void fragmentDevicePrepare(FragmentDevice* d, const FragmentFrame* frame, PrGpuCommands* commands,
                           PrPixelFormat format, const PrDrawBeneath* PR_NULLABLE beneath);

// Draws the frame's passes into the target, with the frame's pixel (0, 0) at `origin` of the host pass's attachments.
// Leaves the pass's viewport and scissor over the whole attachments.
void fragmentDeviceRender(FragmentDevice* d, const FragmentFrame* frame, PrGpuPass* pass, PrIVec2 origin);

// Releases what the state keeps for a frame that is being destroyed, which may never have been prepared.
void fragmentDeviceReleaseFrame(FragmentDevice* d, const FragmentFrame* frame);

// Releases the textures and layers that the state created for frames. No frame is alive when it is called.
void fragmentDeviceDropResources(FragmentDevice* d);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
