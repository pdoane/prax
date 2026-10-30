// The Vulkan GPU backend: creating a device over the backend's own instance or the host's device, and the functions of
// pr/gpu_vulkan.h. The other sources of the backend implement the device's operations.

#include "pr/gpu_vulkan.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/mem.h"

#include "gpu/backends/vulkan/src/vulkan_commands.h"
#include "gpu/backends/vulkan/src/vulkan_pipeline.h"
#include "gpu/backends/vulkan/src/vulkan_platform.h"
#include "gpu/backends/vulkan/src/vulkan_resources.h"
#include "gpu/backends/vulkan/src/vulkan_swap_chain.h"

namespace pr_impl {

namespace {

constexpr uint32_t kDefaultResourceHandles = 65536;
constexpr uint32_t kDefaultSamplerHandles = 4096;

// The most pipelines that a device holds at once.
constexpr uint32_t kMaxPipelines = 65536;

const char* const kValidationLayer = "VK_LAYER_KHRONOS_validation";

// The device extensions that the backend needs, and those that it uses where the device has them.
const char* const kRequiredExtensions[] = {
    VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME
};
const char* const kOptionalExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME
};

// The chain of feature structures that a device is created with. The chain holds local reads only if the adapter has
// them.
struct FeatureChain {
    VkPhysicalDeviceFeatures2 features;
    VkPhysicalDeviceVulkan11Features vulkan11;
    VkPhysicalDeviceVulkan12Features vulkan12;
    VkPhysicalDeviceVulkan13Features vulkan13;
    VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mutableDescriptors;
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR localRead;
};

// Links a chain's structures in order, with local reads if `localRead`.
void link(FeatureChain& chain, bool localRead) {
    chain.features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    chain.vulkan11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    chain.vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    chain.vulkan13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    chain.mutableDescriptors.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT;
    chain.localRead.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR;
    chain.features.pNext = &chain.vulkan11;
    chain.vulkan11.pNext = &chain.vulkan12;
    chain.vulkan12.pNext = &chain.vulkan13;
    chain.vulkan13.pNext = &chain.mutableDescriptors;
    chain.mutableDescriptors.pNext = localRead ? &chain.localRead : nullptr;
    chain.localRead.pNext = nullptr;
}

// Sets the features that the backend needs in `chain`.
void require(FeatureChain& chain) {
    VkPhysicalDeviceFeatures& core = chain.features.features;
    core.samplerAnisotropy = VK_TRUE;
    core.independentBlend = VK_TRUE;
    core.multiDrawIndirect = VK_TRUE;
    core.drawIndirectFirstInstance = VK_TRUE;
    core.depthBiasClamp = VK_TRUE;
    core.fragmentStoresAndAtomics = VK_TRUE;
    core.vertexPipelineStoresAndAtomics = VK_TRUE;
    core.shaderStorageImageReadWithoutFormat = VK_TRUE;
    core.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    chain.vulkan11.shaderDrawParameters = VK_TRUE;
    VkPhysicalDeviceVulkan12Features& v12 = chain.vulkan12;
    v12.descriptorIndexing = VK_TRUE;
    v12.runtimeDescriptorArray = VK_TRUE;
    v12.descriptorBindingPartiallyBound = VK_TRUE;
    v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    v12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    v12.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    v12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    v12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    v12.timelineSemaphore = VK_TRUE;
    v12.scalarBlockLayout = VK_TRUE;
    chain.vulkan13.dynamicRendering = VK_TRUE;
    chain.vulkan13.synchronization2 = VK_TRUE;
    chain.vulkan13.maintenance4 = VK_TRUE;
    chain.mutableDescriptors.mutableDescriptorType = VK_TRUE;
}

// Returns true if every feature that `wanted` sets, `supported` has. Compares the chains structure by structure, as
// arrays of VkBool32 after each structure's header.
bool covers(const FeatureChain& supported, const FeatureChain& wanted) {
    struct Span {
        const void* wanted;
        const void* supported;
        size_t offset;
        size_t size;
    };
    const Span spans[] = {
        {&wanted.features.features, &supported.features.features, 0, sizeof(VkPhysicalDeviceFeatures)},
        {
            &wanted.vulkan11, &supported.vulkan11, offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess),
            sizeof(VkPhysicalDeviceVulkan11Features),
        },
        {
            &wanted.vulkan12, &supported.vulkan12, offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge),
            sizeof(VkPhysicalDeviceVulkan12Features),
        },
        {
            &wanted.vulkan13, &supported.vulkan13, offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess),
            sizeof(VkPhysicalDeviceVulkan13Features),
        },
        {
            &wanted.mutableDescriptors, &supported.mutableDescriptors,
            offsetof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT, mutableDescriptorType),
            sizeof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT),
        },
    };
    for (const Span& span : spans) {
        const uint8_t* w = static_cast<const uint8_t*>(span.wanted);
        const uint8_t* s = static_cast<const uint8_t*>(span.supported);
        for (size_t at = span.offset; at + sizeof(VkBool32) <= span.size; at += sizeof(VkBool32)) {
            VkBool32 want = 0;
            VkBool32 have = 0;
            memCopy(&want, w + at, sizeof(want));
            memCopy(&have, s + at, sizeof(have));
            if (want && !have)
                return false;
        }
    }
    return true;
}

bool sameName(const char* a, const char* b) {
    size_t i = 0;
    while (a[i] == b[i] && a[i] != '\0')
        ++i;
    return a[i] == b[i];
}

bool hasExtension(const Array<VkExtensionProperties>& extensions, const char* name) {
    for (uint32_t i = 0; i < extensions.count; ++i) {
        if (sameName(extensions.data[i].extensionName, name))
            return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL validationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* context) {
    (void)types;
    VulkanDevice* d = static_cast<VulkanDevice*>(context);
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        d->validationErrors.fetchAdd(1);
        vulkanLog("vulkan error: ", data->pMessage);
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        vulkanLog("vulkan warning: ", data->pMessage);
    }
    return VK_FALSE;
}

#define PR_VULKAN_LOAD_GLOBAL(name) \
    d->vk.name = reinterpret_cast<PFN_vk##name>(d->vk.GetInstanceProcAddr(nullptr, "vk" #name));
#define PR_VULKAN_LOAD_INSTANCE(name) \
    d->vk.name = reinterpret_cast<PFN_vk##name>(d->vk.GetInstanceProcAddr(d->instance, "vk" #name));
#define PR_VULKAN_LOAD_DEVICE(name) \
    d->vk.name = reinterpret_cast<PFN_vk##name>(d->vk.GetDeviceProcAddr(d->device, "vk" #name));

void loadGlobal(VulkanDevice* d) {
    PR_VULKAN_GLOBAL_FUNCTIONS(PR_VULKAN_LOAD_GLOBAL)
}

void loadInstance(VulkanDevice* d) {
    PR_VULKAN_INSTANCE_FUNCTIONS(PR_VULKAN_LOAD_INSTANCE)
}

void loadDevice(VulkanDevice* d) {
    PR_VULKAN_DEVICE_FUNCTIONS(PR_VULKAN_LOAD_DEVICE)
}

#undef PR_VULKAN_LOAD_GLOBAL
#undef PR_VULKAN_LOAD_INSTANCE
#undef PR_VULKAN_LOAD_DEVICE

// Creates the backend's own instance, with validation if `validation` and the layer is installed.
PrStatus createInstance(VulkanDevice* d, bool validation) {
    uint32_t version = VK_API_VERSION_1_0;
    if (d->vk.EnumerateInstanceVersion != nullptr)
        d->vk.EnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3)
        return PrStatus_NotSupported;
    uint32_t count = 0;
    d->vk.EnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    Array<VkExtensionProperties> available;
    available.resize(count);
    d->vk.EnumerateInstanceExtensionProperties(nullptr, &count, available.data);
    Array<const char*> extensions;
    const char* surface = vulkanSurfaceExtension();
    if (surface != nullptr && hasExtension(available, VK_KHR_SURFACE_EXTENSION_NAME) &&
            hasExtension(available, surface)) {
        extensions.push(VK_KHR_SURFACE_EXTENSION_NAME);
        extensions.push(surface);
    }
    if (hasExtension(available, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
        extensions.push(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    bool debugUtils = hasExtension(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debugUtils)
        extensions.push(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    available.reset();

    Array<const char*> layers;
    if (validation) {
        d->vk.EnumerateInstanceLayerProperties(&count, nullptr);
        Array<VkLayerProperties> found;
        found.resize(count);
        d->vk.EnumerateInstanceLayerProperties(&count, found.data);
        for (uint32_t i = 0; i < found.count; ++i) {
            if (sameName(found.data[i].layerName, kValidationLayer))
                layers.push(kValidationLayer);
        }
        found.reset();
    }

    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pEngineName = "Prax";
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = extensions.count;
    info.ppEnabledExtensionNames = extensions.data;
    info.enabledLayerCount = layers.count;
    info.ppEnabledLayerNames = layers.data;
    VkResult result = d->vk.CreateInstance(&info, nullptr, &d->instance);
    extensions.reset();
    bool validated = layers.count != 0;
    layers.reset();
    if (result != VK_SUCCESS)
        return PrStatus_NotSupported;
    d->ownsInstance = true;
    loadInstance(d);
    if (validated && debugUtils && d->vk.CreateDebugUtilsMessengerEXT != nullptr) {
        VkDebugUtilsMessengerCreateInfoEXT messenger{};
        messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        messenger.pfnUserCallback = validationMessage;
        messenger.pUserData = d;
        d->vk.CreateDebugUtilsMessengerEXT(d->instance, &messenger, nullptr, &d->messenger);
    }
    return PrStatus_Ok;
}

// Returns the device's extensions.
void deviceExtensions(VulkanDevice* d, VkPhysicalDevice physical, Array<VkExtensionProperties>& out) {
    uint32_t count = 0;
    d->vk.EnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr);
    out.resize(count);
    d->vk.EnumerateDeviceExtensionProperties(physical, nullptr, &count, out.data);
}

// Returns the queue family of a device that draws and computes, or UINT32_MAX if it has none.
uint32_t queueFamilyOf(VulkanDevice* d, VkPhysicalDevice physical) {
    uint32_t count = 0;
    d->vk.GetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    Array<VkQueueFamilyProperties> families;
    families.resize(count);
    d->vk.GetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data);
    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < count && found == UINT32_MAX; ++i) {
        VkQueueFlags flags = families.data[i].queueFlags;
        if ((flags & VK_QUEUE_GRAPHICS_BIT) != 0 && (flags & VK_QUEUE_COMPUTE_BIT) != 0)
            found = i;
    }
    families.reset();
    return found;
}

// Returns true if `physical` has what the backend needs.
bool suitable(VulkanDevice* d, VkPhysicalDevice physical) {
    VkPhysicalDeviceProperties properties;
    d->vk.GetPhysicalDeviceProperties(physical, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_3 || queueFamilyOf(d, physical) == UINT32_MAX)
        return false;
    Array<VkExtensionProperties> extensions;
    deviceExtensions(d, physical, extensions);
    bool found = true;
    for (const char* name : kRequiredExtensions)
        found = found && hasExtension(extensions, name);
    extensions.reset();
    if (!found)
        return false;
    FeatureChain supported{};
    link(supported, false);
    d->vk.GetPhysicalDeviceFeatures2(physical, &supported.features);
    FeatureChain wanted{};
    require(wanted);
    return covers(supported, wanted);
}

// Picks the adapter: the one at `index` if it is not negative, and otherwise the first discrete adapter that is
// suitable, or the first suitable one.
PrStatus pickAdapter(VulkanDevice* d, int32_t index) {
    uint32_t count = 0;
    d->vk.EnumeratePhysicalDevices(d->instance, &count, nullptr);
    if (count == 0)
        return PrStatus_NotFound;
    Array<VkPhysicalDevice> adapters;
    adapters.resize(count);
    d->vk.EnumeratePhysicalDevices(d->instance, &count, adapters.data);
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    if (index >= 0) {
        if (uint32_t(index) < count && suitable(d, adapters.data[index]))
            chosen = adapters.data[index];
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            if (!suitable(d, adapters.data[i]))
                continue;
            VkPhysicalDeviceProperties properties;
            d->vk.GetPhysicalDeviceProperties(adapters.data[i], &properties);
            if (chosen == VK_NULL_HANDLE || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                bool discrete = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
                chosen = adapters.data[i];
                if (discrete)
                    break;
            }
        }
    }
    adapters.reset();
    if (chosen == VK_NULL_HANDLE)
        return PrStatus_NotSupported;
    d->physical = chosen;
    return PrStatus_Ok;
}

// Creates the backend's own device on the adapter that it picked, with the features that it needs and those of its
// optional extensions that the adapter has.
PrStatus createDevice(VulkanDevice* d) {
    d->queueFamily = queueFamilyOf(d, d->physical);
    Array<VkExtensionProperties> available;
    deviceExtensions(d, d->physical, available);
    Array<const char*> extensions;
    for (const char* name : kRequiredExtensions)
        extensions.push(name);
    for (const char* name : kOptionalExtensions) {
        if (hasExtension(available, name))
            extensions.push(name);
    }
    bool localRead = hasExtension(available, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    available.reset();

    FeatureChain supported{};
    link(supported, localRead);
    d->vk.GetPhysicalDeviceFeatures2(d->physical, &supported.features);
    d->localRead = localRead && supported.localRead.dynamicRenderingLocalRead;
    FeatureChain enabled{};
    link(enabled, localRead);
    require(enabled);
    enabled.localRead.dynamicRenderingLocalRead = d->localRead;
    VkPhysicalDeviceFeatures& core = enabled.features.features;
    core.fillModeNonSolid = supported.features.features.fillModeNonSolid;
    core.dualSrcBlend = supported.features.features.dualSrcBlend;
    core.textureCompressionBC = supported.features.features.textureCompressionBC;
    core.textureCompressionASTC_LDR = supported.features.features.textureCompressionASTC_LDR;
    core.shaderInt64 = supported.features.features.shaderInt64;
    core.shaderInt16 = supported.features.features.shaderInt16;
    enabled.vulkan12.shaderFloat16 = supported.vulkan12.shaderFloat16;
    enabled.vulkan12.vulkanMemoryModel = supported.vulkan12.vulkanMemoryModel;
    enabled.vulkan12.vulkanMemoryModelDeviceScope = supported.vulkan12.vulkanMemoryModelDeviceScope;
    enabled.vulkan13.textureCompressionASTC_HDR = supported.vulkan13.textureCompressionASTC_HDR;

    float priority = 1;
    VkDeviceQueueCreateInfo queue{};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = d->queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.pNext = &enabled.features;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = extensions.count;
    info.ppEnabledExtensionNames = extensions.data;
    VkResult result = d->vk.CreateDevice(d->physical, &info, nullptr, &d->device);
    extensions.reset();
    if (result != VK_SUCCESS)
        return PrStatus_NotSupported;
    d->ownsDevice = true;
    loadDevice(d);
    d->vk.GetDeviceQueue(d->device, d->queueFamily, 0, &d->queue);

    PrGpuFeatureFlags& features = d->info.features;
    if (core.textureCompressionBC)
        features |= PrGpuFeatureFlag_TextureCompressionBc;
    if (core.textureCompressionASTC_LDR)
        features |= PrGpuFeatureFlag_TextureCompressionAstc;
    return PrStatus_Ok;
}

// Creates a descriptor set layout of `bindings`, extended as `next` says.
bool createSetLayout(VulkanDevice* d, const VkDescriptorSetLayoutBinding* PR_NULLABLE bindings, uint32_t count,
        VkDescriptorSetLayoutCreateFlags flags, const void* PR_NULLABLE next, VkDescriptorSetLayout* out) {
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.pNext = next;
    info.flags = flags;
    info.bindingCount = count;
    info.pBindings = bindings;
    return vulkanCheck(d, d->vk.CreateDescriptorSetLayout(d->device, &info, nullptr, out));
}

// Creates the device's own bindless set, its push descriptor layout, and the pipeline layout over them.
PrStatus createDescriptors(VulkanDevice* d, uint32_t resources, uint32_t samplers) {
    const VkPhysicalDeviceLimits& limits = d->limits;
    uint32_t resourceLimit = limits.maxPerStageDescriptorSampledImages;
    if (resources > resourceLimit)
        resources = resourceLimit;
    if (samplers > limits.maxPerStageDescriptorSamplers)
        samplers = limits.maxPerStageDescriptorSamplers;
    d->ownsDescriptors = true;
    d->firstResource = 0;
    d->resourceCount = resources;
    d->firstSampler = 0;
    d->samplerCount = samplers;

    const VkDescriptorType mutableTypes[] = {
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
    };
    VkMutableDescriptorTypeListEXT lists[2] = {};
    lists[1].descriptorTypeCount = countOf(mutableTypes);
    lists[1].pDescriptorTypes = mutableTypes;
    VkMutableDescriptorTypeCreateInfoEXT mutableInfo{};
    mutableInfo.sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT;
    mutableInfo.mutableDescriptorTypeListCount = 2;
    mutableInfo.pMutableDescriptorTypeLists = lists;

    VkDescriptorBindingFlags bindingFlags[2] = {};
    for (VkDescriptorBindingFlags& flags : bindingFlags)
        flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{};
    flagsInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flagsInfo.pNext = &mutableInfo;
    flagsInfo.bindingCount = 2;
    flagsInfo.pBindingFlags = bindingFlags;
    const VkDescriptorSetLayoutBinding bindless[2] = {
        {VulkanBinding_Samplers, VK_DESCRIPTOR_TYPE_SAMPLER, samplers, VK_SHADER_STAGE_ALL, nullptr},
        {VulkanBinding_Resources, VK_DESCRIPTOR_TYPE_MUTABLE_EXT, resources, VK_SHADER_STAGE_ALL, nullptr},
    };
    const VkDescriptorSetLayoutBinding pushed[2] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
        {1, vulkanTargetDescriptor(d), 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    if (!createSetLayout(d, bindless, 2, VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, &flagsInfo,
            &d->setLayout) || !createSetLayout(d, pushed, 2, VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, nullptr,
                &d->pushLayout) || !createSetLayout(d, nullptr, 0, 0, nullptr, &d->emptyLayout))
        return PrStatus_NotSupported;

    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, samplers},
        {VK_DESCRIPTOR_TYPE_MUTABLE_EXT, resources},
    };
    VkMutableDescriptorTypeListEXT poolLists[2] = {VkMutableDescriptorTypeListEXT{}, lists[1]};
    VkMutableDescriptorTypeCreateInfoEXT poolMutable{};
    poolMutable.sType = VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT;
    poolMutable.mutableDescriptorTypeListCount = 2;
    poolMutable.pMutableDescriptorTypeLists = poolLists;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.pNext = &poolMutable;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    if (!vulkanCheck(d, d->vk.CreateDescriptorPool(d->device, &poolInfo, nullptr, &d->descriptorPool)))
        return PrStatus_NotSupported;
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = d->descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &d->setLayout;
    if (!vulkanCheck(d, d->vk.AllocateDescriptorSets(d->device, &allocateInfo, &d->set)))
        return PrStatus_NotSupported;

    VkDescriptorSetLayout layouts[VulkanSet_Count] = {d->emptyLayout, d->pushLayout, d->setLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_ALL, 0, kPrGpuMaxPushBytes};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = VulkanSet_Count;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (!vulkanCheck(d, d->vk.CreatePipelineLayout(d->device, &layoutInfo, nullptr, &d->layout)))
        return PrStatus_NotSupported;
    return PrStatus_Ok;
}

PrStatus createTimelines(VulkanDevice* d) {
    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    info.pNext = &type;
    if (!vulkanCheck(d, d->vk.CreateSemaphore(d->device, &info, nullptr, &d->timeline)))
        return PrStatus_NotSupported;
    return PrStatus_Ok;
}

bool isLost(const PrGpuDevice* device) {
    return vulkanDevice(device)->lost.load();
}

// Vulkan has no call that removes a device, so the device only reports that it is lost and refuses new work.
void simulateLoss(PrGpuDevice* device) {
    vulkanDevice(device)->lost.store(true);
}

void destroy(PrGpuDevice* device) {
    VulkanDevice* d = vulkanDevice(device);
    if (d->running) {
        if (d->ownsDevice)
            d->vk.DeviceWaitIdle(d->device);
        else
            d->waitIdle(d);
    }
    if (d->running) {
        prMutexLock(&d->doneMutex);
        d->stopping = true;
        prConditionSignal(&d->pendingSignal);
        prMutexUnlock(&d->doneMutex);
        prThreadJoin(&d->completion);
    }
    if (d->device != VK_NULL_HANDLE) {
        prMutexLock(&d->mutex);
        vulkanCommandsReset(d);
        vulkanResourcesReset(d);
        prMutexUnlock(&d->mutex);
        d->vk.DestroySemaphore(d->device, d->timeline, nullptr);
        if (d->ownsDescriptors) {
            d->vk.DestroyPipelineLayout(d->device, d->layout, nullptr);
            d->vk.DestroyDescriptorPool(d->device, d->descriptorPool, nullptr);
            d->vk.DestroyDescriptorSetLayout(d->device, d->setLayout, nullptr);
            d->vk.DestroyDescriptorSetLayout(d->device, d->pushLayout, nullptr);
            d->vk.DestroyDescriptorSetLayout(d->device, d->emptyLayout, nullptr);
        }
        if (d->ownsDevice)
            d->vk.DestroyDevice(d->device, nullptr);
    }
    if (d->messenger != VK_NULL_HANDLE)
        d->vk.DestroyDebugUtilsMessengerEXT(d->instance, d->messenger, nullptr);
    if (d->ownsInstance)
        d->vk.DestroyInstance(d->instance, nullptr);
    vulkanLoaderClose(d->loader);
    d->freeSlots.reset();
    d->openSerials.reset();
    d->pending.reset();
    prConditionReset(&d->pendingSignal);
    prConditionReset(&d->reachedSignal);
    prMutexReset(&d->doneMutex);
    prMutexReset(&d->mutex);
    prMemoryFree(d);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrGpuVulkanOptions prGpuVulkanOptionsDefault(void) {
    PrGpuVulkanOptions options{};
    options.resourceHandles = kDefaultResourceHandles;
    options.samplerHandles = kDefaultSamplerHandles;
    options.adapter = vulkanEnvironment("PR_VULKAN_ADAPTER", -1);
    options.validation = vulkanEnvironment("PR_VULKAN_DEBUG", 0) != 0;
    return options;
}

PrStatus prGpuVulkanCreate(const PrGpuVulkanOptions* PR_NULLABLE options, PrGpuDevice* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrGpuVulkanOptions defaults = prGpuVulkanOptionsDefault();
    const PrGpuVulkanOptions& o = options != nullptr ? *options : defaults;
    PR_ASSERT((o.device == nullptr) == (o.descriptors == nullptr));
    PR_ASSERT((o.allocate == nullptr) == (o.free == nullptr));

    VulkanDevice* d = allocZeroed<VulkanDevice>();
    prMutexInit(&d->mutex);
    prMutexInit(&d->doneMutex);
    prConditionInit(&d->pendingSignal);
    prConditionInit(&d->reachedSignal);
    d->loader = vulkanLoaderOpen(&d->vk.GetInstanceProcAddr);
    if (d->loader == nullptr) {
        destroy(d);
        return PrStatus_NotFound;
    }
    loadGlobal(d);

    PrStatus status = PrStatus_Ok;
    if (o.instance != nullptr) {
        d->instance = static_cast<VkInstance>(o.instance);
        loadInstance(d);
    } else {
        status = createInstance(d, o.validation);
    }
    if (status == PrStatus_Ok && o.physicalDevice != nullptr)
        d->physical = static_cast<VkPhysicalDevice>(o.physicalDevice);
    else if (status == PrStatus_Ok)
        status = pickAdapter(d, o.adapter);
    if (status == PrStatus_Ok) {
        VkPhysicalDeviceProperties properties;
        d->vk.GetPhysicalDeviceProperties(d->physical, &properties);
        d->limits = properties.limits;
        d->vk.GetPhysicalDeviceMemoryProperties(d->physical, &d->memoryProperties);
        memCopy(d->adapter, properties.deviceName, sizeof(d->adapter));
        d->adapter[sizeof(d->adapter) - 1] = '\0';
    }
    if (status == PrStatus_Ok && o.device != nullptr) {
        d->device = static_cast<VkDevice>(o.device);
        d->queue = static_cast<VkQueue>(o.queue);
        d->queueFamily = o.queueFamily;
        d->localRead = o.localRead;
        loadDevice(d);
    } else if (status == PrStatus_Ok) {
        status = createDevice(d);
    }
    if (status == PrStatus_Ok && o.descriptors != nullptr) {
        const PrGpuVulkanDescriptors& host = *o.descriptors;
        d->set = reinterpret_cast<VkDescriptorSet>(host.set);
        d->layout = reinterpret_cast<VkPipelineLayout>(host.pipelineLayout);
        d->firstResource = host.firstResource;
        d->resourceCount = host.resourceCount;
        d->firstSampler = host.firstSampler;
        d->samplerCount = host.samplerCount;
    } else if (status == PrStatus_Ok) {
        status = createDescriptors(d, o.resourceHandles, o.samplerHandles);
    }
    if (status == PrStatus_Ok)
        status = createTimelines(d);
    if (status != PrStatus_Ok) {
        destroy(d);
        return status;
    }

    d->allocate = o.allocate;
    d->free = o.free;
    d->memoryContext = o.memoryContext;
    d->resources.prepare(d->firstResource + d->resourceCount);
    d->resources.count = 0;
    while (d->resources.count <= d->firstResource) {
        VulkanResource empty{};
        d->resources.push(empty);
    }
    d->nextSlot = d->firstResource + 1;
    d->samplers.push(VulkanSampler{});
    d->pipelines.prepare(kMaxPipelines);
    d->pipelines.count = 0;
    d->openPage = UINT32_MAX;
    d->nextSerial = 1;

    d->info.adapter = PrStringView{d->adapter, 0};
    while (d->adapter[d->info.adapter.size] != '\0')
        ++d->info.adapter.size;
    d->info.backend = "Vulkan";
    if (d->limits.timestampComputeAndGraphics)
        d->info.features |= PrGpuFeatureFlag_Timestamps;
    d->info.maxTextureSize = d->limits.maxImageDimension2D;
    d->info.maxResourceHandles = d->resourceCount - 1;
    d->info.maxSamplerHandles = d->samplerCount - 1;
    d->shaderTarget = d->localRead ? PrGpuShaderTarget_SpirvLocalRead : PrGpuShaderTarget_Spirv;

    d->destroy = destroy;
    d->isLost = isLost;
    d->simulateLoss = simulateLoss;
    vulkanResourceFunctions(d);
    vulkanPipelineFunctions(d);
    vulkanCommandsFunctions(d);
    vulkanSwapChainFunctions(d);
    prThreadStart(&d->completion, vulkanCompletionRun, d);
    d->running = true;
    *out = d;
    return PrStatus_Ok;
}

uint32_t prGpuVulkanValidationErrors(const PrGpuDevice* device) {
    return vulkanDevice(device)->validationErrors.load();
}

uint32_t prGpuVulkanDeviceExtensions(const char* const* PR_NULLABLE* PR_NONNULL out) {
    *out = kRequiredExtensions;
    return countOf(kRequiredExtensions);
}

const void* prGpuVulkanDeviceFeatures(void) {
    static FeatureChain chain = [] {
        FeatureChain c{};
        link(c, false);
        require(c);
        return c;
    }();
    return &chain.features;
}

void* prGpuVulkanInstance(const PrGpuDevice* device) {
    return vulkanDevice(device)->instance;
}

void* prGpuVulkanPhysicalDevice(const PrGpuDevice* device) {
    return vulkanDevice(device)->physical;
}

void* prGpuVulkanDevice(const PrGpuDevice* device) {
    return vulkanDevice(device)->device;
}

void* prGpuVulkanQueue(const PrGpuDevice* device) {
    return vulkanDevice(device)->queue;
}

uint64_t prGpuVulkanBuffer(const PrGpuDevice* device, PrGpuBuffer buffer) {
    return reinterpret_cast<uint64_t>(vulkanDevice(device)->resources.data[buffer.id].buffer);
}

uint64_t prGpuVulkanImage(const PrGpuDevice* device, PrGpuTexture texture) {
    return reinterpret_cast<uint64_t>(vulkanDevice(device)->resources.data[texture.id].image);
}

void* prGpuVulkanGetInstanceProcAddr(const PrGpuDevice* device) {
    return reinterpret_cast<void*>(vulkanDevice(device)->vk.GetInstanceProcAddr);
}

PrStatus prGpuVulkanTextureWrap(PrGpuDevice* device, uint64_t image, const PrGpuTextureDesc* desc, PrGpuTexture* out) {
    return vulkanTextureWrap(vulkanDevice(device), reinterpret_cast<VkImage>(image), desc, out);
}

PrStatus prGpuVulkanBufferWrap(PrGpuDevice* device, uint64_t buffer, const PrGpuBufferDesc* desc, PrGpuBuffer* out) {
    return vulkanBufferWrap(vulkanDevice(device), reinterpret_cast<VkBuffer>(buffer), desc, out);
}

uint32_t prGpuVulkanLayout(PrGpuAccessFlags access) {
    return uint32_t(vulkanLayoutFromAccess(access));
}
