// What every GPU backend's objects hold after their functions, and the functions that the backends share: the getters
// of that state, sampler interning, imports that copy, and mip levels drawn with a pipeline.
//
// Each backend's device, command buffers, passes, swap chains and shader libraries derive from these. A backend
// installs the shared functions with gpuSharedDeviceInit and its objects' tables, and keeps the state that they read.

#pragma once

#include "pr/assert.h"
#include "pr/base.h"
#include "pr/gpu_backend.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A sampler that the device made for a description.
struct GpuSharedSampler {
    PrGpuSamplerDesc desc;
    PrGpuSampler sampler;
};

// A pipeline that draws mip levels into a format.
struct GpuSharedMipmapPipeline {
    PrGpuFormat format;
    PrGpuPipeline pipeline;
};

// A device: what it reports, the backend's own operations that the shared functions build on, and what they keep.
struct GpuSharedDevice : PrGpuDevice {
    PrGpuShaderTarget shaderTarget;     // the code that the backend runs
    PrGpuDeviceInfo info;
    uint32_t nativeKinds;               // bit k is set if the device hands out PrGpuNativeKind k
    uint32_t inPlaceImports;            // bit k is set if the device imports PrGpuImportKind k in place

    // Creates the sampler for a description that the device has not seen, and imports an image in place. A null
    // `textureImportInPlace` means that the device imports nothing in place.
    PrStatus (*samplerCreate)(PrGpuDevice* device, const PrGpuSamplerDesc* desc, PrGpuSampler* out);
    PrStatus (*PR_NULLABLE textureImportInPlace)(PrGpuDevice* device, const PrGpuImport* image,
        PrGpuReleaseFn PR_NULLABLE release, void* PR_NULLABLE context, PrGpuTexture* out);

    // Samplers by description, what draws mip levels, and the ids of the textures that imports made by copying, which a
    // later import may write its copy into. `mutex` guards them.
    PrMutex mutex;
    RawArray<GpuSharedSampler> samplers;
    PrGpuShaderLibrary* PR_NULLABLE mipmapLibrary;
    RawArray<GpuSharedMipmapPipeline> mipmapPipelines;
    RawArray<uint32_t> importCopies;
};

// A command buffer: its device, and what the checks of its calls read.
struct GpuSharedCommands : PrGpuCommands {
    PrGpuDevice* device;
    bool native;                        // records into the application's native command buffer
    bool inPass;
    bool submitted;
};

// A pass of a command buffer, and its attachments.
struct GpuSharedPass : PrGpuPass {
    GpuSharedCommands* commands;
    PrGpuAttachmentFormats formats;
    PrUVec2 extent;
    bool native;                        // records into a pass that the application has open
};

// A swap chain of a device.
struct GpuSharedSwapChain : PrGpuSwapChain {
    PrGpuDevice* device;
    PrGpuSwapChainInfo info;
};

// A shader library of a device, and its entry points, which it frees.
struct GpuSharedLibrary : PrGpuShaderLibrary {
    PrGpuDevice* device;
    uint32_t entryCount;
    PrGpuShaderEntry* entries;          // by entry point
};

// Installs the device's shared functions and initializes their state, or frees the state. The device frees it after its
// work is done and before it destroys its own resources, since the state holds pipelines of the device.
void gpuSharedDeviceInit(GpuSharedDevice* device);
void gpuSharedDeviceReset(GpuSharedDevice* device);

// Forgets a texture that the device destroys, so that a later texture of the same id is not taken for an import's copy.
// The backend's textureDestroyFn calls it.
void gpuSharedTextureForget(GpuSharedDevice* device, PrGpuTexture texture);

// The objects' functions that read the shared state.
PrGpuDevice* gpuSharedCommandsDevice(const PrGpuCommands* commands);
PrGpuCommands* gpuSharedPassCommands(const PrGpuPass* pass);
PrGpuAttachmentFormats gpuSharedPassFormats(const PrGpuPass* pass);
PrUVec2 gpuSharedPassExtent(const PrGpuPass* pass);
PrGpuDevice* gpuSharedSwapChainDevice(const PrGpuSwapChain* swapChain);
PrGpuSwapChainInfo gpuSharedSwapChainInfo(const PrGpuSwapChain* swapChain);
const PrGpuShaderEntry* gpuSharedLibraryEntries(const PrGpuShaderLibrary* library, uint32_t* count);

// Records the mip levels of a texture, each drawn from the one before, for a backend that has no operation of its own.
void gpuSharedGenerateMipmaps(PrGpuCommands* commands, PrGpuTexture texture, PrGpuAccessFlags before);

// Checks that no pass of the command buffer is open, as a copy, a dispatch or a barrier needs.
inline void gpuSharedCheckOutsidePass(const PrGpuCommands* commands) {
    PR_ASSERT(!static_cast<const GpuSharedCommands*>(commands)->inPass);
    (void)commands;
}

// Records that a pass of the command buffer began, and checks that none was open and the command buffer was not
// submitted.
inline void gpuSharedPassBegan(GpuSharedCommands* commands) {
    PR_ASSERT(!commands->inPass && !commands->submitted);
    commands->inPass = true;
}

// Records that the command buffer's pass ended, and checks that one was open.
inline void gpuSharedPassEnded(GpuSharedCommands* commands) {
    PR_ASSERT(commands->inPass);
    commands->inPass = false;
}

// Records that the command buffer was submitted, and checks that no pass is open and it was not submitted before.
inline void gpuSharedSubmitted(GpuSharedCommands* commands) {
    PR_ASSERT(!commands->inPass && !commands->submitted);
    commands->submitted = true;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
