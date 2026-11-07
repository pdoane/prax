// The Vulkan backend's shader libraries and pipelines.
//
// A library is one SPIR-V module that holds every entry point, each named by its function. Pipelines are created with
// the device's pipeline layout, dynamic rendering, and dynamic viewport, scissor, blend constants and stencil
// reference. Vertex data is read by shaders, so pipelines have no vertex input.

#include "gpu/backends/vulkan/src/vulkan_pipeline.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/mem.h"

#include "gpu/backends/vulkan/src/vulkan_resources.h"

namespace pr_impl {

namespace {

// The functions of the device's objects, which their creation installs.
extern const PrGpuShaderLibrary kLibraryFunctions;

const VulkanLibrary* libraryOf(const PrGpuShaderLibrary* library) {
    return static_cast<const VulkanLibrary*>(library);
}

// The longest name of an entry point that a pipeline takes, with its terminator.
constexpr size_t kEntryNameMaxBytes = 128;

// Copies an entry point's name into `out`, terminated, for the stage that Vulkan reads it from.
void entryName(const PrGpuShaderEntry* entry, char (&out)[kEntryNameMaxBytes]) {
    size_t size = entry->name.size < kEntryNameMaxBytes - 1 ? entry->name.size : kEntryNameMaxBytes - 1;
    prMemCopy(out, entry->name.data, size);
    out[size] = '\0';
}

VkShaderStageFlagBits stageOf(PrGpuShaderStage stage) {
    switch (stage) {
        case PrGpuShaderStage_Fragment:
            return VK_SHADER_STAGE_FRAGMENT_BIT;
        case PrGpuShaderStage_Compute:
            return VK_SHADER_STAGE_COMPUTE_BIT;
        default:
            return VK_SHADER_STAGE_VERTEX_BIT;
    }
}

VkBlendFactor factorOf(PrGpuBlendFactor factor) {
    switch (factor) {
        case PrGpuBlendFactor_Zero:
            return VK_BLEND_FACTOR_ZERO;
        case PrGpuBlendFactor_One:
            return VK_BLEND_FACTOR_ONE;
        case PrGpuBlendFactor_Source:
            return VK_BLEND_FACTOR_SRC_COLOR;
        case PrGpuBlendFactor_OneMinusSource:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case PrGpuBlendFactor_SourceAlpha:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case PrGpuBlendFactor_OneMinusSourceAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case PrGpuBlendFactor_Target:
            return VK_BLEND_FACTOR_DST_COLOR;
        case PrGpuBlendFactor_OneMinusTarget:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case PrGpuBlendFactor_TargetAlpha:
            return VK_BLEND_FACTOR_DST_ALPHA;
        case PrGpuBlendFactor_OneMinusTargetAlpha:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case PrGpuBlendFactor_SourceAlphaSaturated:
            return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case PrGpuBlendFactor_Constant:
            return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case PrGpuBlendFactor_OneMinusConstant:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case PrGpuBlendFactor_Source1:
            return VK_BLEND_FACTOR_SRC1_COLOR;
        case PrGpuBlendFactor_OneMinusSource1:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        default:
            return VK_BLEND_FACTOR_ONE;
    }
}

// The alpha factor that matches a color factor: a color factor names the source's or the target's color, and its alpha
// counterpart names the same alpha.
VkBlendFactor alphaFactorOf(PrGpuBlendFactor factor) {
    switch (factor) {
        case PrGpuBlendFactor_Source:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case PrGpuBlendFactor_OneMinusSource:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case PrGpuBlendFactor_Target:
            return VK_BLEND_FACTOR_DST_ALPHA;
        case PrGpuBlendFactor_OneMinusTarget:
            return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case PrGpuBlendFactor_Constant:
            return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case PrGpuBlendFactor_OneMinusConstant:
            return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case PrGpuBlendFactor_Source1:
            return VK_BLEND_FACTOR_SRC1_ALPHA;
        case PrGpuBlendFactor_OneMinusSource1:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        default:
            return factorOf(factor);
    }
}

VkPrimitiveTopology topologyOf(PrGpuTopology topology) {
    switch (topology) {
        case PrGpuTopology_TriangleStrip:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PrGpuTopology_LineList:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrGpuTopology_LineStrip:
            return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case PrGpuTopology_PointList:
            return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        default:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

VkStencilOpState stencilOf(const PrGpuStencilFace& face, uint8_t readMask, uint8_t writeMask) {
    VkStencilOpState state{};
    state.failOp = VkStencilOp(face.fail);
    state.passOp = VkStencilOp(face.pass);
    state.depthFailOp = VkStencilOp(face.depthFail);
    state.compareOp = VkCompareOp(face.compare);
    state.compareMask = readMask;
    state.writeMask = writeMask;
    return state;
}

// Stores a created pipeline under a new id. Returns Limit, and destroys the pipeline, if no id is left.
PrStatus pipelineAdd(VulkanDevice* d, VkPipeline pipeline, VkPipelineBindPoint bindPoint, PrGpuExtent groupSize,
        bool readsTarget, PrGpuPipeline* out) {
    prMutexLock(&d->mutex);
    uint32_t id = 0;
    if (d->freePipelines.count != 0) {
        id = d->freePipelines.last();
        d->freePipelines.pop();
    } else if (d->pipelines.count < d->pipelines.capacity) {
        d->pipelines.push(VulkanPipeline{});
        id = d->pipelines.count;
    }
    if (id != 0)
        d->pipelines.data[id - 1] = VulkanPipeline{pipeline, bindPoint, groupSize, readsTarget};
    prMutexUnlock(&d->mutex);
    if (id == 0) {
        d->vk.DestroyPipeline(d->device, pipeline, nullptr);
        return PrStatus_Limit;
    }
    *out = PrGpuPipeline{id};
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Libraries

// Every entry point shares the one module, which the library's bytes hold.
PrStatus shaderLibraryCreate(PrGpuDevice* device, PrGpuShaderEntry* entries, uint32_t entryCount,
        PrGpuShaderLibrary* PR_NULLABLE* PR_NONNULL out) {
    VulkanDevice* d = vulkanDevice(device);
    if (entryCount == 0 || entries[0].codeSize % 4 != 0)
        return PrStatus_InvalidData;
    // SPIR-V is read as words, so the module is made from an aligned copy of the code.
    uint32_t* words = static_cast<uint32_t*>(prMemoryAlloc(size_t(entries[0].codeSize)));
    prMemCopy(words, entries[0].code, size_t(entries[0].codeSize));
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = size_t(entries[0].codeSize);
    info.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult result = d->vk.CreateShaderModule(d->device, &info, nullptr, &module);
    prMemoryFree(words);
    if (!vulkanCheck(d, result))
        return result == VK_ERROR_DEVICE_LOST ? PrStatus_Lost : PrStatus_InvalidData;
    VulkanLibrary* library = allocZeroed<VulkanLibrary>();
    static_cast<PrGpuShaderLibrary&>(*library) = kLibraryFunctions;
    library->device = device;
    library->entryCount = entryCount;
    library->entries = entries;
    library->module = module;
    *out = library;
    return PrStatus_Ok;
}

void shaderLibraryDestroy(PrGpuShaderLibrary* library) {
    PrGpuDevice* device = static_cast<GpuSharedLibrary*>(library)->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanLibrary* l = static_cast<VulkanLibrary*>(library);
    d->vk.DestroyShaderModule(d->device, l->module, nullptr);
    prMemoryFree(l->entries);
    prMemoryFree(l);
}

// ---------------------------------------------------------------------------------------------------------------------
// Pipelines

PrStatus graphicsPipelineCreate(PrGpuDevice* device, const PrGpuGraphicsPipelineDesc* desc,
        const PrGpuShaderEntry* vertex, const PrGpuShaderEntry* PR_NULLABLE fragment, PrGpuPipeline* out) {
    VulkanDevice* d = vulkanDevice(device);
    const VulkanLibrary* library = libraryOf(desc->library);
    VkPipelineShaderStageCreateInfo stages[2] = {};
    char names[2][kEntryNameMaxBytes];
    const PrGpuShaderEntry* entries[2] = {vertex, fragment};
    uint32_t stageCount = 0;
    for (const PrGpuShaderEntry* entry : entries) {
        if (entry == nullptr)
            continue;
        entryName(entry, names[stageCount]);
        VkPipelineShaderStageCreateInfo& stage = stages[stageCount];
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = stageOf(entry->stage);
        stage.module = library->module;
        stage.pName = names[stageCount];
        ++stageCount;
    }

    VkFormat colorFormats[kPrGpuMaxColorAttachments];
    VkPipelineColorBlendAttachmentState blends[kPrGpuMaxColorAttachments] = {};
    for (uint32_t i = 0; i < desc->formats.colorCount; ++i) {
        colorFormats[i] = vulkanFormat(desc->formats.color[i]);
        const PrGpuBlend& blend = desc->blend[i];
        VkPipelineColorBlendAttachmentState& state = blends[i];
        state.blendEnable = blend.enabled;
        state.srcColorBlendFactor = factorOf(blend.colorSource);
        state.dstColorBlendFactor = factorOf(blend.colorTarget);
        state.colorBlendOp = VkBlendOp(blend.colorOp);
        state.srcAlphaBlendFactor = alphaFactorOf(blend.alphaSource);
        state.dstAlphaBlendFactor = alphaFactorOf(blend.alphaTarget);
        state.alphaBlendOp = VkBlendOp(blend.alphaOp);
        // PrGpuColorWriteFlags' values are VkColorComponentFlags'.
        state.colorWriteMask = blend.write != 0 ? blend.write : PrGpuColorWriteFlag_All;
    }
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = desc->formats.colorCount;
    rendering.pColorAttachmentFormats = colorFormats;
    if (desc->formats.depthStencil != PrGpuFormat_None) {
        VkImageAspectFlags aspect = vulkanAspect(desc->formats.depthStencil);
        if ((aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
            rendering.depthAttachmentFormat = vulkanFormat(desc->formats.depthStencil);
        if ((aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0)
            rendering.stencilAttachmentFormat = vulkanFormat(desc->formats.depthStencil);
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = topologyOf(desc->topology);
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterization{};
    rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    if (desc->cull == PrGpuCull_Front)
        rasterization.cullMode = VK_CULL_MODE_FRONT_BIT;
    else if (desc->cull == PrGpuCull_Back)
        rasterization.cullMode = VK_CULL_MODE_BACK_BIT;
    else
        rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = desc->frontClockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.depthBiasEnable = desc->depthBias != 0 || desc->depthBiasSlope != 0;
    rasterization.depthBiasConstantFactor = desc->depthBias;
    rasterization.depthBiasSlopeFactor = desc->depthBiasSlope;
    rasterization.depthBiasClamp = desc->depthBiasClamp;
    rasterization.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VkSampleCountFlagBits(desc->formats.samples);
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    bool depth = desc->formats.depthStencil != PrGpuFormat_None;
    depthStencil.depthTestEnable = depth && desc->depthTest;
    depthStencil.depthWriteEnable = depth && desc->depthWrite;
    depthStencil.depthCompareOp = VkCompareOp(desc->depthCompare);
    depthStencil.stencilTestEnable = depth && desc->stencilTest;
    depthStencil.front = stencilOf(desc->stencilFront, desc->stencilReadMask, desc->stencilWriteMask);
    depthStencil.back = stencilOf(desc->stencilBack, desc->stencilReadMask, desc->stencilWriteMask);
    VkPipelineColorBlendStateCreateInfo colorBlend{};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = desc->formats.colorCount;
    colorBlend.pAttachments = blends;
    VkDynamicState dynamics[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE
    };
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = countOf(dynamics);
    dynamic.pDynamicStates = dynamics;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = stageCount;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = d->layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = d->vk.CreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (!vulkanCheck(d, result))
        return result == VK_ERROR_DEVICE_LOST ? PrStatus_Lost : PrStatus_NotSupported;
    vulkanName(d, VK_OBJECT_TYPE_PIPELINE, uint64_t(pipeline), desc->name);
    return pipelineAdd(d, pipeline, VK_PIPELINE_BIND_POINT_GRAPHICS, PrGpuExtent{0, 0, 0}, desc->readsTarget, out);
}

PrStatus computePipelineCreate(PrGpuDevice* device, const PrGpuShaderLibrary* library, const PrGpuShaderEntry* compute,
        PrStringView name, PrGpuPipeline* out) {
    VulkanDevice* d = vulkanDevice(device);
    char entry[kEntryNameMaxBytes];
    entryName(compute, entry);
    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = libraryOf(library)->module;
    info.stage.pName = entry;
    info.layout = d->layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult result = d->vk.CreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (!vulkanCheck(d, result))
        return result == VK_ERROR_DEVICE_LOST ? PrStatus_Lost : PrStatus_NotSupported;
    vulkanName(d, VK_OBJECT_TYPE_PIPELINE, uint64_t(pipeline), name);
    return pipelineAdd(d, pipeline, VK_PIPELINE_BIND_POINT_COMPUTE, compute->groupSize, false, out);
}

void pipelineDestroy(PrGpuDevice* device, PrGpuPipeline pipeline) {
    VulkanDevice* d = vulkanDevice(device);
    prMutexLock(&d->mutex);
    vulkanRetire(d, VulkanRetired{VulkanRetireKind_Pipeline, pipeline.id, 0});
    vulkanRetireDone(d);
    prMutexUnlock(&d->mutex);
}

PrGpuExtent pipelineGroupSize(const PrGpuDevice* device, PrGpuPipeline pipeline) {
    return vulkanDevice(device)->pipelines.data[pipeline.id - 1].groupSize;
}

// The functions of the device's objects.
constexpr PrGpuShaderLibrary kLibraryFunctions = {
    .destroyFn = shaderLibraryDestroy,
    .entriesFn = gpuSharedLibraryEntries,
};

} // namespace

void vulkanPipelineFunctions(VulkanDevice* d) {
    d->shaderLibraryCreateFn = shaderLibraryCreate;
    d->graphicsPipelineCreateFn = graphicsPipelineCreate;
    d->computePipelineCreateFn = computePipelineCreate;
    d->pipelineDestroyFn = pipelineDestroy;
    d->pipelineGroupSizeFn = pipelineGroupSize;
}

} // namespace pr_impl
