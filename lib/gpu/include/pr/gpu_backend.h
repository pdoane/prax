// The interface that GPU backends implement. Callers use the functions of pr/gpu.h, which check their arguments and
// call these. Only backends include this header. Buffers, textures, samplers and pipelines are values that name a
// resource of the device, so the device's functions take them.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The stage of a shader library's entry point.
enum {
    PrGpuShaderStage_Vertex,
    PrGpuShaderStage_Fragment,
    PrGpuShaderStage_Compute,
    PrGpuShaderStage_Count,
};
typedef uint8_t PrGpuShaderStage;

// The code of a shader library, by the backend that runs it (cmake/shader_library.cmake).
enum {
    PrGpuShaderTarget_Metal = 1,            // a metallib
    PrGpuShaderTarget_Spirv = 2,            // a SPIR-V module that does not read the target
    PrGpuShaderTarget_SpirvLocalRead = 3,   // a SPIR-V module that reads the target with dynamic rendering local read
    PrGpuShaderTarget_Dxil = 4,             // DXIL, one blob for each entry point
};
typedef uint32_t PrGpuShaderTarget;

// An entry point of a shader library, and its code for the device's backend. `groupSize` is a compute shader's group
// size, and zero otherwise.
typedef struct PrGpuShaderEntry {
    PrStringView name;
    PrGpuShaderStage stage;
    PrGpuExtent groupSize;
    const uint8_t* code;
    uint64_t codeSize;
} PrGpuShaderEntry;

// The functions of a shader library's backend, at the start of its state. `entriesFn` returns the library's entry
// points, whose names and code are the bytes that the library was created from, and writes how many there are.
struct PrGpuShaderLibrary {
    void (*destroyFn)(PrGpuShaderLibrary* library);
    const PrGpuShaderEntry* PR_NONNULL (*PR_NONNULL entriesFn)(const PrGpuShaderLibrary* library, uint32_t* count);
};

// The functions of a command buffer's backend, at the start of its state. Each implements the function of pr/gpu.h that
// it is named for, after pr/gpu.h has checked the arguments, and checks what depends on the command buffer's state,
// such as whether a pass is open. `generateMipmapsFn` is called only for a texture of more than one level, and takes it
// from `before` to PrGpuAccessFlag_ShaderRead as the API function does.
struct PrGpuCommands {
    PrGpuDevice* PR_NONNULL (*PR_NONNULL deviceFn)(const PrGpuCommands* commands);
    PrGpuFence (*submitFn)(PrGpuCommands* commands);
    void* PR_NONNULL (*PR_NONNULL streamFn)(PrGpuCommands* commands, uint64_t size, PrGpuBuffer* buffer, uint64_t* offset);
    void (*setPushFn)(PrGpuCommands* commands, const void* data, uint32_t size);
    void (*setUniformsFn)(PrGpuCommands* commands, const void* data, uint32_t size);
    void (*setPipelineFn)(PrGpuCommands* commands, PrGpuPipeline pipeline);
    void (*setBufferFn)(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer, uint64_t offset);
    void (*setRwBufferFn)(PrGpuCommands* commands, uint32_t slot, PrGpuBuffer buffer, uint64_t offset);
    void (*setTextureFn)(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture, PrGpuSampler sampler);
    void (*setRwTextureFn)(PrGpuCommands* commands, uint32_t slot, PrGpuTexture texture);
    void (*dispatchFn)(PrGpuCommands* commands, PrGpuExtent groups);
    void (*dispatchIndirectFn)(PrGpuCommands* commands, PrGpuBuffer buffer, uint64_t offset);
    void (*copyBufferFn)(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset, PrGpuBuffer target,
        uint64_t targetOffset, uint64_t size);
    void (*copyTextureFn)(PrGpuCommands* commands, PrGpuTexture source, const PrGpuTextureRegion* sourceRegion,
        PrGpuTexture target, const PrGpuTextureRegion* targetRegion);
    void (*copyBufferToTextureFn)(PrGpuCommands* commands, PrGpuBuffer source, uint64_t sourceOffset,
        uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target, const PrGpuTextureRegion* targetRegion);
    void (*copyTextureToBufferFn)(PrGpuCommands* commands, PrGpuTexture source, const PrGpuTextureRegion* sourceRegion,
        PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes, uint32_t layerBytes);
    void (*generateMipmapsFn)(PrGpuCommands* commands, PrGpuTexture texture, PrGpuAccessFlags before);
    void (*onDoneFn)(PrGpuCommands* commands, PrGpuDoneFn done, void* PR_NULLABLE context);
    void (*pushGroupFn)(PrGpuCommands* commands, PrStringView name);
    void (*popGroupFn)(PrGpuCommands* commands);
    void (*bufferBarrierFn)(PrGpuCommands* commands, PrGpuBuffer buffer, PrGpuAccessFlags before, PrGpuAccessFlags after);
    void (*textureBarrierFn)(PrGpuCommands* commands, PrGpuTexture texture, PrGpuSubresources subresources,
        PrGpuAccessFlags before, PrGpuAccessFlags after);
    void (*globalBarrierFn)(PrGpuCommands* commands, PrGpuAccessFlags before, PrGpuAccessFlags after);
    PrGpuNativeStateFlags (*nativeStateSyncFn)(PrGpuCommands* commands);
    PrGpuPass* PR_NONNULL (*PR_NONNULL passBeginFn)(PrGpuCommands* commands, const PrGpuPassDesc* desc);
};

// The functions of a pass's backend, at the start of its state. Each implements the function of pr/gpu.h that it is
// named for, after pr/gpu.h has checked the arguments. A null `nativeDrawFn` means that the device hands out no native
// objects.
struct PrGpuPass {
    PrGpuCommands* PR_NONNULL (*PR_NONNULL commandsFn)(const PrGpuPass* pass);
    PrGpuAttachmentFormats (*formatsFn)(const PrGpuPass* pass);
    PrUVec2 (*extentFn)(const PrGpuPass* pass);
    void (*endFn)(PrGpuPass* pass);
    void (*PR_NULLABLE nativeDrawFn)(PrGpuPass* pass, PrGpuNativeKind kind, PrGpuNativeDrawFn draw, void* PR_NULLABLE context);
    void (*setViewportFn)(PrGpuPass* pass, PrRect viewport, float minDepth, float maxDepth);
    void (*setScissorFn)(PrGpuPass* pass, PrIRect scissor);
    void (*setBlendConstantFn)(PrGpuPass* pass, PrColor constant);
    void (*setStencilReferenceFn)(PrGpuPass* pass, uint8_t reference);
    void (*setIndexBufferFn)(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, bool wide);
    void (*drawFn)(PrGpuPass* pass, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
        uint32_t firstInstance);
    void (*drawIndexedFn)(PrGpuPass* pass, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
        int32_t vertexOffset, uint32_t firstInstance);
    void (*drawIndirectFn)(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride);
    void (*drawIndexedIndirectFn)(PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, uint32_t count, uint32_t stride);
};

// The functions of a swap chain's backend, at the start of its state. Each implements the function of pr/gpu.h that it
// is named for.
struct PrGpuSwapChain {
    PrGpuDevice* PR_NONNULL (*PR_NONNULL deviceFn)(const PrGpuSwapChain* swapChain);
    PrGpuSwapChainInfo (*infoFn)(const PrGpuSwapChain* swapChain);
    void (*destroyFn)(PrGpuSwapChain* swapChain);
    void (*waitFn)(PrGpuSwapChain* swapChain);
    PrStatus (*acquireFn)(PrGpuSwapChain* swapChain, PrUVec2 extent, PrGpuTexture* out);
    void (*presentFn)(PrGpuSwapChain* swapChain, PrGpuCommands* commands);
};

// The functions of a device's backend, at the start of its state. Each implements the function of pr/gpu.h that it is
// named for, after pr/gpu.h has checked the arguments. pr/gpu.h implements the rest of its interface from these:
// writing and reading textures, reading a texture in a command buffer, and submitting and waiting.
//
// `shaderTargetFn` returns the code that the backend runs, `nativeKindsFn` the native objects that the device hands
// out, bit k for PrGpuNativeKind k, and `inPlaceImportsFn` the images that it imports in place, bit k for
// PrGpuImportKind k. `shaderLibraryCreateFn` takes the entries whose code pr/gpu.h found for the backend, and the
// library holds and frees them. The pipeline functions take entries that pr/gpu.h found and checked the stages of.
//
// A null `textureCreateOverPixelsFn` means that the device does not hand out PrGpuNativeKind_Pixels, and a null
// `textureNativeFn` that it hands out no native objects. Either is called only with a kind that the device hands out.
struct PrGpuDevice {
    void (*destroyFn)(PrGpuDevice* device);
    PrGpuDeviceInfo (*infoFn)(const PrGpuDevice* device);
    PrGpuShaderTarget (*shaderTargetFn)(const PrGpuDevice* device);
    uint32_t (*nativeKindsFn)(const PrGpuDevice* device);
    uint32_t (*inPlaceImportsFn)(const PrGpuDevice* device);
    bool (*isLostFn)(const PrGpuDevice* device);
    void (*simulateLossFn)(PrGpuDevice* device);
    bool (*isCompleteFn)(const PrGpuDevice* device, PrGpuFence fence);
    void (*waitFn)(PrGpuDevice* device, PrGpuFence fence);
    void (*waitIdleFn)(PrGpuDevice* device);
    bool (*supportsFormatFn)(const PrGpuDevice* device, PrGpuFormat format, PrGpuTextureUsageFlags usage);

    PrStatus (*bufferCreateFn)(PrGpuDevice* device, const PrGpuBufferDesc* desc, PrGpuBuffer* out);
    void (*bufferDestroyFn)(PrGpuDevice* device, PrGpuBuffer buffer);
    PrGpuBufferDesc (*bufferDescFn)(const PrGpuDevice* device, PrGpuBuffer buffer);
    uint32_t (*bufferStorageHandleFn)(const PrGpuDevice* device, PrGpuBuffer buffer);
    void* PR_NONNULL (*PR_NONNULL bufferMapFn)(const PrGpuDevice* device, PrGpuBuffer buffer);
    void (*bufferUploadFn)(PrGpuDevice* device, PrGpuBuffer buffer, uint64_t offset, const void* data, uint64_t size);

    PrStatus (*textureCreateFn)(PrGpuDevice* device, const PrGpuTextureDesc* desc, PrGpuTexture* out);
    PrStatus (*textureCreateViewFn)(PrGpuDevice* device, PrGpuTexture texture, PrGpuTextureKind kind,
        PrGpuSubresources subresources, PrGpuTexture* out);
    void (*textureDestroyFn)(PrGpuDevice* device, PrGpuTexture texture);
    PrGpuTextureDesc (*textureDescFn)(const PrGpuDevice* device, PrGpuTexture texture);
    uint32_t (*textureStorageHandleFn)(const PrGpuDevice* device, PrGpuTexture texture);
    void (*textureUploadFn)(PrGpuDevice* device, PrGpuTexture texture, const PrGpuTextureRegion* region,
        const void* data, uint32_t rowBytes, uint32_t layerBytes);
    PrStatus (*textureImportFn)(PrGpuDevice* device, const PrGpuImport* image, PrGpuReleaseFn PR_NULLABLE release,
        void* PR_NULLABLE context, PrGpuTexture* texture);
    PrStatus (*PR_NULLABLE textureCreateOverPixelsFn)(PrGpuDevice* device, PrGpuFormat format, PrPixels pixels,
        PrGpuTextureUsageFlags usage, PrGpuTexture* out);
    bool (*PR_NULLABLE textureNativeFn)(PrGpuDevice* device, PrGpuTexture texture, PrGpuNativeKind kind, void* out);

    PrStatus (*samplerInternFn)(PrGpuDevice* device, const PrGpuSamplerDesc* desc, PrGpuSampler* out);

    PrStatus (*shaderLibraryCreateFn)(PrGpuDevice* device, PrGpuShaderEntry* entries, uint32_t entryCount,
        PrGpuShaderLibrary* PR_NULLABLE* PR_NONNULL out);
    PrStatus (*graphicsPipelineCreateFn)(PrGpuDevice* device, const PrGpuGraphicsPipelineDesc* desc,
        const PrGpuShaderEntry* vertex, const PrGpuShaderEntry* PR_NULLABLE fragment, PrGpuPipeline* out);
    PrStatus (*computePipelineCreateFn)(PrGpuDevice* device, const PrGpuShaderLibrary* library,
        const PrGpuShaderEntry* compute, PrStringView name, PrGpuPipeline* out);
    void (*pipelineDestroyFn)(PrGpuDevice* device, PrGpuPipeline pipeline);
    PrGpuExtent (*pipelineGroupSizeFn)(const PrGpuDevice* device, PrGpuPipeline pipeline);

    bool (*commandsBeginFn)(PrGpuDevice* device, const PrGpuCommandsOptions* options,
        PrGpuCommands* PR_NULLABLE* PR_NONNULL out);
    PrStatus (*swapChainCreateFn)(PrGpuDevice* device, const PrSurface* surface, const PrGpuSwapChainDesc* desc,
        PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
