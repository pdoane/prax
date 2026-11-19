// What every GPU backend's objects hold after their functions, and the functions that the backends share: the getters
// of that state, sampler interning, imports that copy, and mip levels drawn with a pipeline.
//
// Each backend's device, command buffers, passes, swap chains and shader libraries derive from these. A backend
// installs the shared functions with gpuSharedDeviceInit and its objects' tables, and keeps the state that they read.

#pragma once

#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/base.h"
#include "pr/gpu_backend.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/thread.h"

#include "gpu/backends/shared/src/gpu_shared_table.h"

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

    // The backend's operations on the device's lifetime. `initialize` makes the objects that the device runs on, and
    // returns Ok or the status that the creation fails with. It runs on the creation's pool, or during the constructor.
    // `removed` returns true once the system has taken a ready device away. `destroy` frees the device after its
    // initialization succeeded or failed, so it frees only what was made.
    PrStatus (*initialize)(GpuSharedDevice* device);
    bool (*removed)(const GpuSharedDevice* device);
    void (*destroy)(GpuSharedDevice* device);

    // The creation's stage, a PrGpuDeviceStage, and the status that a failed creation returned. `stage` is written
    // after `failure` and after everything that `initialize` made. `created` is set under `stageMutex` after the ready
    // callback returns. A destroy waits on `stageSignal` until `created` is set.
    Atomic<uint8_t> stage;
    PrStatus failure;
    PrGpuDeviceReadyFn PR_NULLABLE ready;
    void* PR_NULLABLE readyContext;
    PrMutex stageMutex;
    PrCondition stageSignal;
    bool created;

    // True if the device was destroyed while a creation that the system finishes later ran (gpuSharedDeviceStartLater).
    // The creation frees the device once it finishes, without calling the ready callback.
    bool abandoned;
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
// work is done and before it destroys its own resources, since the state holds pipelines of the device. The shared
// functions include the device's `stageFn`, `lostStatusFn` and `destroyFn`, which build on the backend's `removed` and
// `destroy`.
void gpuSharedDeviceInit(GpuSharedDevice* device);
void gpuSharedDeviceReset(GpuSharedDevice* device);

// Returns the limits that a device's options ask for, each zero replaced by the default's (prGpuDeviceLimitsDefault). A
// backend whose API allows less lowers them, and sizes its tables by what it keeps.
PrGpuDeviceLimits gpuSharedDeviceLimits(PrGpuDeviceLimits asked);

// Starts the device's creation. The constructor calls it after filling the function table and the backend's
// `initialize`, `removed` and `destroy`. It runs `initialize` as a job of `pool`, or during the call if `pool` is null,
// then publishes the stage and calls `ready`.
void gpuSharedDeviceStart(GpuSharedDevice* device, PrTaskPool* PR_NULLABLE pool, PrGpuDeviceReadyFn PR_NULLABLE ready,
    void* PR_NULLABLE readyContext);

// Starts the creation of a device whose system creates devices asynchronously, as a web page's does. The backend calls
// gpuSharedDeviceFinish with the status that the creation reached once the system is done, on the thread that started
// the creation. The device has no `initialize`.
void gpuSharedDeviceStartLater(GpuSharedDevice* device, PrGpuDeviceReadyFn PR_NULLABLE ready,
    void* PR_NULLABLE readyContext);

// Publishes the stage that the device's initialization reached, Ready for Ok and Failed otherwise, and calls the ready
// callback. gpuSharedDeviceStart calls it once `initialize` returns.
void gpuSharedDeviceFinish(GpuSharedDevice* device, PrStatus status);

// Returns true if the device's creation succeeded, so that the backend may use what `initialize` made.
inline bool gpuSharedDeviceIsReady(const GpuSharedDevice* device) {
    return device->stage.load() == PrGpuDeviceStage_Ready;
}

// Forgets a texture that the device destroys, so that a later texture of the same id is not taken for an import's copy.
// The backend's textureReleaseFn calls it for the release that retires the texture.
void gpuSharedTextureForget(GpuSharedDevice* device, PrGpuTexture texture);

// The objects' functions that read the shared state.
PrGpuDevice* gpuSharedCommandsDevice(const PrGpuCommands* commands);
PrGpuCommands* gpuSharedPassCommands(const PrGpuPass* pass);
PrGpuAttachmentFormats gpuSharedPassFormats(const PrGpuPass* pass);
PrUVec2 gpuSharedPassExtent(const PrGpuPass* pass);
PrGpuDevice* gpuSharedSwapChainDevice(const PrGpuSwapChain* swapChain);
PrGpuSwapChainInfo gpuSharedSwapChainInfo(const PrGpuSwapChain* swapChain);
const PrGpuShaderEntry* gpuSharedLibraryEntries(const PrGpuShaderLibrary* library, uint32_t* count);

// Returns the alignment of an allocation of a command buffer's stream for `usage`, which pr/gpu.h states.
inline uint64_t gpuSharedStreamAlignment(const GpuSharedDevice* device, PrGpuBufferUsageFlags usage) {
    return (usage & PrGpuBufferUsageFlag_Storage) != 0 ? device->info.storageOffsetAlignment : 16;
}

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
