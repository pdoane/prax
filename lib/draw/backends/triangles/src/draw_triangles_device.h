// The triangles renderer's work on a GPU device: the textures that frames request, and the draws of their triangles.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"

#include "draw/backends/triangles/src/draw_triangles_frame.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What the renderer keeps on a device: its pipelines and samplers, and the textures that frames requested.
struct TrianglesDevice;

// Creates the renderer's state on `device`. Returns NotSupported if the device cannot run the renderer's shaders, and
// Lost if it is lost.
PrStatus trianglesDeviceCreate(PrGpuDevice* device, TrianglesDevice* PR_NULLABLE* PR_NONNULL out);

// Frees the state. The device may still be executing work that reads it.
void trianglesDeviceDestroy(TrianglesDevice* PR_NULLABLE device);

// Carries out the frame's requests, which the work submitted after the call sees.
void trianglesDevicePrepare(TrianglesDevice* d, const TrianglesFrame* frame, PrGpuCommands* commands);

// Draws the frame's draws into the pass, with the frame's pixel (0, 0) at `origin` of the pass's attachments. Leaves
// the pass's viewport and scissor over the whole attachments.
void trianglesDeviceRender(TrianglesDevice* d, const TrianglesFrame* frame, PrGpuPass* pass, PrIVec2 origin);

// Releases the textures that the state created for frames. The frames that follow recreate theirs.
void trianglesDeviceDropResources(TrianglesDevice* d);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
