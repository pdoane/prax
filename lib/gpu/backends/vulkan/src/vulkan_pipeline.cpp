// The Vulkan backend's shader libraries and pipelines.
//
// A library is one SPIR-V module that holds every entry point, each named by its function. Pipelines are created with
// the device's pipeline layout, dynamic rendering, and dynamic viewport, scissor, blend constants and stencil
// reference. Vertex data is read by shaders, so pipelines have no vertex input.
//
// A device without bindless refuses a pipeline whose entry point reaches a resource by handle. The module says which
// do: an entry point of SPIR-V 1.4 or later lists every global variable that it reaches, and the variables of the
// bindless set are the ones that handles index.

#include "gpu/backends/vulkan/src/vulkan_pipeline.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/mem.h"

#include "gpu/backends/vulkan/src/vulkan_resources.h"

namespace pr_impl {

namespace {

VkFormat vertexFormatOf(PrGpuVertexFormat format) {
    switch (format) {
        case PrGpuVertexFormat_Float: return VK_FORMAT_R32_SFLOAT;
        case PrGpuVertexFormat_Float2: return VK_FORMAT_R32G32_SFLOAT;
        case PrGpuVertexFormat_Float3: return VK_FORMAT_R32G32B32_SFLOAT;
        case PrGpuVertexFormat_Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case PrGpuVertexFormat_Half2: return VK_FORMAT_R16G16_SFLOAT;
        case PrGpuVertexFormat_Half4: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case PrGpuVertexFormat_Uint: return VK_FORMAT_R32_UINT;
        case PrGpuVertexFormat_Uint2: return VK_FORMAT_R32G32_UINT;
        case PrGpuVertexFormat_Uint3: return VK_FORMAT_R32G32B32_UINT;
        case PrGpuVertexFormat_Uint4: return VK_FORMAT_R32G32B32A32_UINT;
        case PrGpuVertexFormat_Sint: return VK_FORMAT_R32_SINT;
        case PrGpuVertexFormat_Sint2: return VK_FORMAT_R32G32_SINT;
        case PrGpuVertexFormat_Sint3: return VK_FORMAT_R32G32B32_SINT;
        case PrGpuVertexFormat_Sint4: return VK_FORMAT_R32G32B32A32_SINT;
        case PrGpuVertexFormat_Unorm8x4: return VK_FORMAT_R8G8B8A8_UNORM;
        case PrGpuVertexFormat_Snorm8x4: return VK_FORMAT_R8G8B8A8_SNORM;
        case PrGpuVertexFormat_Uint8x4: return VK_FORMAT_R8G8B8A8_UINT;
        case PrGpuVertexFormat_Unorm16x2: return VK_FORMAT_R16G16_UNORM;
        case PrGpuVertexFormat_Unorm16x4: return VK_FORMAT_R16G16B16A16_UNORM;
        case PrGpuVertexFormat_Snorm16x2: return VK_FORMAT_R16G16_SNORM;
        case PrGpuVertexFormat_Snorm16x4: return VK_FORMAT_R16G16B16A16_SNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}

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
    const uint32_t id = d->pipelines.take();
    if (id != 0)
        d->pipelines[id] = VulkanPipeline{pipeline, bindPoint, groupSize, readsTarget};
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

// The parts of SPIR-V that spirvReadsHandles reads.
constexpr uint32_t kSpirvHeaderWords = 5;
constexpr uint32_t kSpirvOpEntryPoint = 15;
constexpr uint32_t kSpirvOpDecorate = 71;
constexpr uint32_t kSpirvDecorationDescriptorSet = 34;

// Calls `visit` with each instruction of a SPIR-V module of `count` words: its opcode, its operands and their number.
// Stops at an instruction that does not fit the module.
template <typename Visit>
void spirvEachInstruction(const uint32_t* words, size_t count, Visit visit) {
    for (size_t at = kSpirvHeaderWords; at < count;) {
        size_t length = words[at] >> 16;
        if (length == 0 || length > count - at)
            return;
        visit(words[at] & 0xffff, words + at + 1, uint32_t(length - 1));
        at += length;
    }
}

// Sets `out[i]` to whether entry point `i` of a SPIR-V module reaches a variable of the bindless set. An entry point
// that the module does not hold reaches none.
void spirvReadsHandles(const uint32_t* words, size_t count, const PrGpuShaderEntry* entries, uint32_t entryCount, bool* out) {
    RawArray<uint32_t> bindless{};  // the ids of the bindless set's variables
    spirvEachInstruction(words, count, [&](uint32_t opcode, const uint32_t* operands, uint32_t operandCount) {
        if (opcode == kSpirvOpDecorate && operandCount == 3 && operands[1] == kSpirvDecorationDescriptorSet &&
                operands[2] == VulkanSet_Bindless)
            bindless.push(operands[0]);
    });
    spirvEachInstruction(words, count, [&](uint32_t opcode, const uint32_t* operands, uint32_t operandCount) {
        // An entry point's operands are its execution model, its id, its name and the ids of its variables. The name is
        // terminated, and padded with zeros to a whole word.
        if (opcode != kSpirvOpEntryPoint || operandCount < 3)
            return;
        const char* name = reinterpret_cast<const char*>(operands + 2);
        size_t nameCapacity = size_t(operandCount - 2) * 4;
        size_t nameSize = 0;
        while (nameSize < nameCapacity && name[nameSize] != '\0')
            ++nameSize;
        if (nameSize == nameCapacity)
            return;
        uint32_t firstVariable = 2 + uint32_t(nameSize / 4) + 1;
        bool reads = false;
        for (uint32_t i = firstVariable; i < operandCount && !reads; ++i) {
            for (uint32_t k = 0; k < bindless.count && !reads; ++k)
                reads = bindless.data[k] == operands[i];
        }
        for (uint32_t i = 0; i < entryCount; ++i) {
            if (entries[i].name == PrStringView{name, nameSize})
                out[i] = reads;
        }
    });
    bindless.reset();
}

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
    if (!vulkanCheck(d, result)) {
        prMemoryFree(words);
        return result == VK_ERROR_DEVICE_LOST ? PrStatus_Lost : PrStatus_InvalidData;
    }
    VulkanLibrary* library = allocZeroed<VulkanLibrary>();
    static_cast<PrGpuShaderLibrary&>(*library) = kLibraryFunctions;
    library->device = device;
    library->entryCount = entryCount;
    library->entries = entries;
    library->module = module;
    library->readsHandles = static_cast<bool*>(prMemoryAlloc(entryCount * sizeof(bool)));
    prMemFill(library->readsHandles, 0, entryCount * sizeof(bool));
    spirvReadsHandles(words, size_t(entries[0].codeSize) / 4, entries, entryCount, library->readsHandles);
    prMemoryFree(words);
    *out = library;
    return PrStatus_Ok;
}

void shaderLibraryDestroy(PrGpuShaderLibrary* library) {
    PrGpuDevice* device = static_cast<GpuSharedLibrary*>(library)->device;
    VulkanDevice* d = vulkanDevice(device);
    VulkanLibrary* l = static_cast<VulkanLibrary*>(library);
    d->vk.DestroyShaderModule(d->device, l->module, nullptr);
    prMemoryFree(l->readsHandles);
    prMemoryFree(l->entries);
    prMemoryFree(l);
}

// ---------------------------------------------------------------------------------------------------------------------
// Pipelines

// Returns true if the device runs an entry point of the library. A device without bindless runs only the entry points
// that reach their resources through slots.
bool runsEntry(const VulkanDevice* d, const VulkanLibrary* library, const PrGpuShaderEntry* entry) {
    return d->bindless || !library->readsHandles[entry - library->entries];
}

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
        if (!runsEntry(d, library, entry))
            return PrStatus_NotSupported;
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
    rendering.viewMask = vulkanViewMask(desc->formats.viewCount);
    rendering.colorAttachmentCount = desc->formats.colorCount;
    rendering.pColorAttachmentFormats = colorFormats;
    if (desc->formats.depthStencil != PrGpuFormat_None) {
        VkImageAspectFlags aspect = vulkanAspect(desc->formats.depthStencil);
        if ((aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
            rendering.depthAttachmentFormat = vulkanFormat(desc->formats.depthStencil);
        if ((aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0)
            rendering.stencilAttachmentFormat = vulkanFormat(desc->formats.depthStencil);
    }

    // Attribute k is the vertex shader's input at location k.
    const PrGpuVertexLayout& layout = desc->vertexLayout;
    VkVertexInputBindingDescription bindings[kPrGpuMaxVertexBuffers]{};
    for (uint32_t i = 0; i < layout.bufferCount; ++i) {
        bindings[i] = VkVertexInputBindingDescription{.binding = i, .stride = layout.buffers[i].stride};
        if (layout.buffers[i].step == PrGpuVertexStep_Instance)
            bindings[i].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    }
    VkVertexInputAttributeDescription attributes[kPrGpuMaxVertexAttributes]{};
    for (uint32_t i = 0; i < layout.attributeCount; ++i) {
        const PrGpuVertexAttribute& a = layout.attributes[i];
        attributes[i] = VkVertexInputAttributeDescription{
            .location = i,
            .binding = a.buffer,
            .format = vertexFormatOf(a.format),
            .offset = a.offset,
        };
    }
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = layout.bufferCount;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = layout.attributeCount;
    vertexInput.pVertexAttributeDescriptions = attributes;
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
    if (!runsEntry(d, libraryOf(library), compute))
        return PrStatus_NotSupported;
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

void pipelineRetain(PrGpuDevice* device, PrGpuPipeline pipeline) {
    vulkanDevice(device)->pipelines.retain(pipeline.id);
}

void pipelineRelease(PrGpuDevice* device, PrGpuPipeline pipeline) {
    if (!vulkanDevice(device)->pipelines.release(pipeline.id))
        return;
    VulkanDevice* d = vulkanDevice(device);
    prMutexLock(&d->mutex);
    vulkanRetire(d, VulkanRetired{VulkanRetireKind_Pipeline, pipeline.id, 0});
    vulkanRetireDone(d);
    prMutexUnlock(&d->mutex);
}

PrGpuExtent pipelineGroupSize(const PrGpuDevice* device, PrGpuPipeline pipeline) {
    return vulkanDevice(device)->pipelines[pipeline.id].groupSize;
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
    d->pipelineRetainFn = pipelineRetain;
    d->pipelineReleaseFn = pipelineRelease;
    d->pipelineGroupSizeFn = pipelineGroupSize;
}

} // namespace pr_impl
