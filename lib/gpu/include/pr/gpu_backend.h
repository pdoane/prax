// The interface that GPU backends implement: the state at the start of each object, and the functions of a device.
// Callers use the functions of pr/gpu.h, which check their arguments and call these.

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
    PrGpuShaderTarget_Spirv = 2,            // a SPIR-V module that reads a copy of the target
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

// The start of every shader library's state. The entries' names and code are the bytes that the library was created
// from.
struct PrGpuShaderLibrary {
    PrGpuDevice* device;
    uint32_t entryCount;
    PrGpuShaderEntry* entries;      // by entry point
};

// The start of every command buffer's state.
struct PrGpuCommands {
    PrGpuDevice* device;
    bool host;                      // records into the host's native command buffer
    bool inPass;
    bool submitted;
};

// The start of every pass's state.
struct PrGpuPass {
    PrGpuCommands* commands;
    PrGpuAttachmentFormats formats;
    PrUVec2 extent;
    bool host;                      // records into a pass that the host has open
};

// The start of every swap chain's state.
struct PrGpuSwapChain {
    PrGpuDevice* device;
    PrGpuSwapChainInfo info;
};

// The functions of a device's backend, at the start of the device's state. Each member implements the function of
// pr/gpu.h that it is named for, after pr/gpu.h has checked the arguments. A null `destroy` leaves nothing to free.
//
// pr/gpu.h implements the rest of its interface from these: samplers, writing and reading textures, reading a texture
// in a command buffer, submitting and waiting, and mipmaps.
struct PrGpuDevice {
    void (*PR_NULLABLE destroy)(PrGpuDevice* device);
    bool (*isLost)(const PrGpuDevice* device);
    void (*simulateLoss)(PrGpuDevice* device);
    bool (*isComplete)(const PrGpuDevice* device, PrGpuFence fence);
    void (*wait)(PrGpuDevice* device, PrGpuFence fence);
    void (*waitIdle)(PrGpuDevice* device);
    bool (*supportsFormat)(const PrGpuDevice* device, PrGpuFormat format, PrGpuTextureUsageFlags usage);

    PrStatus (*bufferCreate)(PrGpuDevice* device, const PrGpuBufferDesc* desc, PrGpuBuffer* out);
    void (*bufferDestroy)(PrGpuDevice* device, PrGpuBuffer buffer);
    PrGpuBufferDesc (*bufferDesc)(const PrGpuDevice* device, PrGpuBuffer buffer);
    uint32_t (*bufferStorageHandle)(const PrGpuDevice* device, PrGpuBuffer buffer);
    void* PR_NONNULL (*PR_NONNULL bufferMap)(const PrGpuDevice* device, PrGpuBuffer buffer);
    void (*bufferUpload)(PrGpuDevice* device, PrGpuBuffer buffer, uint64_t offset, const void* data, uint64_t size);

    PrStatus (*textureCreate)(PrGpuDevice* device, const PrGpuTextureDesc* desc, PrGpuTexture* out);
    PrStatus (*textureCreateView)(PrGpuDevice* device, PrGpuTexture texture, PrGpuTextureKind kind,
        PrGpuSubresources subresources, PrGpuTexture* out);
    void (*textureDestroy)(PrGpuDevice* device, PrGpuTexture texture);
    PrGpuTextureDesc (*textureDesc)(const PrGpuDevice* device, PrGpuTexture texture);
    uint32_t (*textureStorageHandle)(const PrGpuDevice* device, PrGpuTexture texture);
    void (*textureUpload)(PrGpuDevice* device, PrGpuTexture texture, const PrGpuTextureRegion* region,
        const void* data, uint32_t rowBytes, uint32_t layerBytes);

    // Creates the sampler for a description that the device has not seen. pr/gpu.h keeps one sampler for each.
    PrStatus (*samplerCreate)(PrGpuDevice* device, const PrGpuSamplerDesc* desc, PrGpuSampler* out);

    // Creates a library over entries whose code pr/gpu.h has found for the backend, and frees one. The library holds
    // the entries, which pr/gpu.h frees after the library.
    PrStatus (*shaderLibraryCreate)(PrGpuDevice* device, PrGpuShaderEntry* entries, uint32_t entryCount,
        PrGpuShaderLibrary* PR_NULLABLE* PR_NONNULL out);
    void (*shaderLibraryDestroy)(PrGpuDevice* device, PrGpuShaderLibrary* library);

    // Create pipelines from entries that pr/gpu.h has found in the library and checked the stages of.
    PrStatus (*graphicsPipelineCreate)(PrGpuDevice* device, const PrGpuGraphicsPipelineDesc* desc,
        const PrGpuShaderEntry* vertex, const PrGpuShaderEntry* PR_NULLABLE fragment, PrGpuPipeline* out);
    PrStatus (*computePipelineCreate)(PrGpuDevice* device, const PrGpuShaderLibrary* library,
        const PrGpuShaderEntry* compute, PrStringView name, PrGpuPipeline* out);
    void (*pipelineDestroy)(PrGpuDevice* device, PrGpuPipeline pipeline);
    PrGpuExtent (*pipelineGroupSize)(const PrGpuDevice* device, PrGpuPipeline pipeline);

    bool (*commandsBegin)(PrGpuDevice* device, const PrGpuCommandsOptions* options,
        PrGpuCommands* PR_NULLABLE* PR_NONNULL out);
    PrGpuFence (*commandsSubmit)(PrGpuDevice* device, PrGpuCommands* commands);
    void* PR_NONNULL (*PR_NONNULL commandsStream)(PrGpuDevice* device, PrGpuCommands* commands, uint64_t size,
        PrGpuBuffer* buffer, uint64_t* offset);
    void (*commandsSetPush)(PrGpuDevice* device, PrGpuCommands* commands, const void* data, uint32_t size);
    void (*commandsSetUniforms)(PrGpuDevice* device, PrGpuCommands* commands, const void* data, uint32_t size);
    void (*commandsSetPipeline)(PrGpuDevice* device, PrGpuCommands* commands, PrGpuPipeline pipeline);
    void (*commandsDispatch)(PrGpuDevice* device, PrGpuCommands* commands, PrGpuExtent groups);
    void (*commandsDispatchIndirect)(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer buffer, uint64_t offset);
    void (*commandsCopyBuffer)(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer source,
        uint64_t sourceOffset, PrGpuBuffer target, uint64_t targetOffset, uint64_t size);
    void (*commandsCopyTexture)(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuTexture target, const PrGpuTextureRegion* targetRegion);
    void (*commandsCopyBufferToTexture)(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer source,
        uint64_t sourceOffset, uint32_t rowBytes, uint32_t layerBytes, PrGpuTexture target,
        const PrGpuTextureRegion* targetRegion);
    void (*commandsCopyTextureToBuffer)(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture source,
        const PrGpuTextureRegion* sourceRegion, PrGpuBuffer target, uint64_t targetOffset, uint32_t rowBytes,
        uint32_t layerBytes);

    // Optional: prGpuCommandsGenerateMipmaps of a texture with more than one level, by the device's own operation. It
    // takes the texture from `before` to PrGpuAccessFlag_ShaderRead as the API function does. Without it, the core
    // draws each level from the one before.
    void (*commandsGenerateMipmaps)(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture texture, PrGpuAccessFlags before);
    void (*commandsOnDone)(PrGpuDevice* device, PrGpuCommands* commands, PrGpuDoneFn done, void* PR_NULLABLE context);
    void (*commandsPushGroup)(PrGpuDevice* device, PrGpuCommands* commands, PrStringView name);
    void (*commandsPopGroup)(PrGpuDevice* device, PrGpuCommands* commands);
    void (*commandsBufferBarrier)(PrGpuDevice* device, PrGpuCommands* commands, PrGpuBuffer buffer,
        PrGpuAccessFlags before, PrGpuAccessFlags after);
    void (*commandsTextureBarrier)(PrGpuDevice* device, PrGpuCommands* commands, PrGpuTexture texture,
        PrGpuSubresources subresources, PrGpuAccessFlags before, PrGpuAccessFlags after);
    void (*commandsGlobalBarrier)(PrGpuDevice* device, PrGpuCommands* commands, PrGpuAccessFlags before,
        PrGpuAccessFlags after);
    PrGpuHostStateFlags (*hostStateSync)(PrGpuDevice* device, PrGpuCommands* commands);

    PrGpuPass* PR_NONNULL (*PR_NONNULL passBegin)(PrGpuDevice* device, PrGpuCommands* commands, const PrGpuPassDesc* desc);
    void (*passEnd)(PrGpuDevice* device, PrGpuPass* pass);
    void (*passSetViewport)(PrGpuDevice* device, PrGpuPass* pass, PrRect viewport, float minDepth, float maxDepth);
    void (*passSetScissor)(PrGpuDevice* device, PrGpuPass* pass, PrIRect scissor);
    void (*passSetBlendConstant)(PrGpuDevice* device, PrGpuPass* pass, PrColor constant);
    void (*passSetStencilReference)(PrGpuDevice* device, PrGpuPass* pass, uint8_t reference);
    void (*passSetIndexBuffer)(PrGpuDevice* device, PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset, bool wide);
    void (*passDraw)(PrGpuDevice* device, PrGpuPass* pass, uint32_t vertexCount, uint32_t instanceCount,
        uint32_t firstVertex, uint32_t firstInstance);
    void (*passDrawIndexed)(PrGpuDevice* device, PrGpuPass* pass, uint32_t indexCount, uint32_t instanceCount,
        uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance);
    void (*passDrawIndirect)(PrGpuDevice* device, PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset,
        uint32_t count, uint32_t stride);
    void (*passDrawIndexedIndirect)(PrGpuDevice* device, PrGpuPass* pass, PrGpuBuffer buffer, uint64_t offset,
        uint32_t count, uint32_t stride);

    PrStatus (*swapChainCreate)(PrGpuDevice* device, const PrAppSurface* surface, const PrGpuSwapChainDesc* desc,
        PrGpuSwapChain* PR_NULLABLE* PR_NONNULL out);
    void (*swapChainDestroy)(PrGpuDevice* device, PrGpuSwapChain* swapChain);
    void (*swapChainWait)(PrGpuDevice* device, PrGpuSwapChain* swapChain);
    PrStatus (*swapChainAcquire)(PrGpuDevice* device, PrGpuSwapChain* swapChain, PrUVec2 extent, PrGpuTexture* out);
    void (*swapChainPresent)(PrGpuDevice* device, PrGpuSwapChain* swapChain, PrGpuCommands* commands);

    PrGpuShaderTarget shaderTarget;               // the code that the backend runs
    PrGpuDeviceInfo info;                         // filled in by the backend
    struct PrGpuDeviceShared* PR_NULLABLE shared; // pr/gpu.h's own state of the device, made on first use
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
