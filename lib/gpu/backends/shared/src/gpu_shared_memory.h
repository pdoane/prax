// Memory devices (PrGpuNativeKind_Pixels), which the memory and Canvas 2D backends share. Buffers and textures are in
// memory, and the work of a command buffer runs in order when the command buffer is submitted. Passes load, clear and
// draw natively into texels, and no shaders run. A backend presents in its own swap chains, through calls that it
// records in order with the work.
//
// A backend may keep copies of textures outside memory, such as the Canvas 2D backend's canvases. The device keeps each
// copy in step with its texture's texels. The copies are native objects of the backend's own kind, and the device draws
// through them too. Work reads and writes the side that it runs on. The device copies a texture's contents to the other
// side only when work on that side reads them. A clear is deferred until one side is read, and is applied only to that
// side.

#pragma once

#include "pr/base.h"
#include "pr/gpu_backend.h"

#include "gpu/backends/shared/src/gpu_shared_device.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A backend's copies of textures, each named by a nonzero handle. A copy holds the first level and layer of a 2D
// texture that has one level and one layer, in the same format as the texels. The copies hold only some formats.
struct GpuMemoryMirror {
    // The kind of native object that a copy is. A copy's object is its handle.
    PrGpuNativeKind kind;

    // Returns true if a copy holds textures of `format`.
    bool (*holds)(PrGpuFormat format);

    // Returns a new transparent copy of `extent`, or zero if the backend cannot make one.
    uint32_t (*create)(PrUVec2 extent);

    // Frees a copy, made by `create` or adopted (gpuMemoryAdoptMirror).
    void (*destroy)(uint32_t mirror);

    // Fills the copy with `color`, as a texel of `format` holds it.
    void (*clear)(uint32_t mirror, PrColor color, PrGpuFormat format);

    // Copies texels of `format` into the copy, or from it into `texels`.
    void (*write)(uint32_t mirror, PrPixelView texels, PrGpuFormat format);
    void (*read)(uint32_t mirror, PrPixels texels, PrGpuFormat format);
};

// Creates a memory device that reports `backend` as its backend's name. `backend` must live as long as the process. If
// `mirror` is not null, the device keeps copies of its textures through it. The device's swap chains present to no
// surface until the backend that created it sets a swap chain create of its own.
PrGpuDevice* gpuMemoryCreate(PrStringView backend, const GpuMemoryMirror* PR_NULLABLE mirror);

// Gives the texture `mirror`, a copy that the backend made itself, which holds what the texture holds. The copies must
// hold the texture's format, and the texture must have no copy yet. The device frees the copy with the texture.
void gpuMemoryAdoptMirror(PrGpuDevice* device, PrGpuTexture texture, uint32_t mirror);

// Creates a texture of `extent` and `format` over `mirror`, a copy that the backend made of an image that it imports.
// Shaders and native draws sample the texture. The copy holds the texture's contents, and they are read into the texels
// when work on the texels reads them. The device frees the copy with the texture, and then calls `release` if it is not
// null.
PrStatus gpuMemoryImport(PrGpuDevice* device, PrUVec2 extent, PrGpuFormat format, uint32_t mirror,
    PrGpuReleaseFn PR_NULLABLE release, void* PR_NULLABLE context, PrGpuTexture* out);

// Records `call`, which runs with `context` in order with the command buffer's other work once the command buffer is
// submitted.
void gpuMemoryRecordCall(PrGpuCommands* commands, void (*call)(void* PR_NULLABLE context), void* PR_NULLABLE context);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
