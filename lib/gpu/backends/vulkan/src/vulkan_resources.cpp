// The Vulkan backend's memory, resources and uploads, and the retirement of what the device destroys.
//
// A buffer or a texture takes a slot of the bindless array when it is created, and writes its descriptor there: a
// buffer's storage buffer, which shaders read and write through, or a texture's sampled image in the read-only layout.
// A texture that shaders write takes a second slot for its storage image in the general layout. An image is held by the
// texture that made it and by each view of it, and is freed with the last of them.
//
// Uploads copy into pages of staging memory at once, and the copies into their targets wait for the next command
// buffer, which records them before its own work and frees the pages once it is done. The same batch zeroes each new
// buffer in device memory, and moves each new texture into the read-only layout.

#include "gpu/backends/vulkan/src/vulkan_resources.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/integer.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// The alignment of an upload within its page, which a copy into a texture of any format accepts.
constexpr uint64_t kUploadAlignment = 16;

// Returns the index of a memory type in `bits` with every property of `required`, preferring one that also has those of
// `preferred`, or UINT32_MAX if none has them.
uint32_t memoryType(const VulkanDevice* d, uint32_t bits, VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred) {
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < d->memoryProperties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) == 0)
            continue;
        VkMemoryPropertyFlags flags = d->memoryProperties.memoryTypes[i].propertyFlags;
        if ((flags & required) != required)
            continue;
        if ((flags & preferred) == preferred)
            return i;
        if (fallback == UINT32_MAX)
            fallback = i;
    }
    return fallback;
}

void writeImage(VulkanDevice* d, uint32_t slot, VkImageView view, bool storage) {
    VkDescriptorImageInfo image{};
    image.imageView = view;
    image.imageLayout = storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = d->set;
    write.dstBinding = VulkanBinding_Resources;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    d->vk.UpdateDescriptorSets(d->device, 1, &write, 0, nullptr);
}

void releaseSlot(VulkanDevice* d, uint32_t slot) {
    VulkanResource& r = vulkanResourceAt(d, slot);
    r.attachments.reset();
    prMemZero(&r, sizeof(r));
    d->freeSlots.push(slot);
}

// Drops a reference to an image, and frees it with its last. Called with the device's mutex held.
void releaseImage(VulkanDevice* d, VulkanImage* held) {
    if (--held->references != 0)
        return;
    if (held->owned) {
        d->vk.DestroyImage(d->device, held->image, nullptr);
        vulkanMemoryFree(d, held->memory);
    }
    prMemoryFree(held);
}

// Frees what a slot holds, and the slot. Called with the device's mutex held.
void freeSlot(VulkanDevice* d, uint32_t slot) {
    VulkanResource& r = vulkanResourceAt(d, slot);
    for (uint32_t i = 0; i < r.attachments.count; ++i)
        d->vk.DestroyImageView(d->device, r.attachments.data[i].view, nullptr);
    if (r.storage != 0) {
        d->vk.DestroyImageView(d->device, vulkanResourceAt(d, r.storage).view, nullptr);
        releaseSlot(d, r.storage);
    }
    if (r.view != VK_NULL_HANDLE)
        d->vk.DestroyImageView(d->device, r.view, nullptr);
    if (r.kind == VulkanSlotKind_Buffer && !r.wrapped) {
        d->vk.DestroyBuffer(d->device, r.buffer, nullptr);
        vulkanMemoryFree(d, r.memory);
    }
    if (r.held != nullptr)
        releaseImage(d, r.held);
    releaseSlot(d, slot);
}

// Returns staging memory for `size` bytes of an upload, in `*page` at `*offset`, or null if no memory is left. Called
// with the device's mutex held.
uint8_t* PR_NULLABLE stagingTake(VulkanDevice* d, uint64_t size, uint32_t* page, uint64_t* offset) {
    if (d->openPage != UINT32_MAX) {
        VulkanStagingPage& open = d->pages.data[d->openPage];
        uint64_t at = alignUp(open.used, kUploadAlignment);
        if (at + size <= open.size) {
            open.used = at + size;
            *page = d->openPage;
            *offset = at;
            return open.memory.mapped + at;
        }
    }
    uint64_t pageSize = size > kStagingPageBytes ? alignUp(size, kUploadAlignment) : kStagingPageBytes;
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = pageSize;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VulkanStagingPage created{};
    created.size = pageSize;
    if (!vulkanCheck(d, d->vk.CreateBuffer(d->device, &info, nullptr, &created.buffer)))
        return nullptr;
    VkMemoryRequirements requirements;
    d->vk.GetBufferMemoryRequirements(d->device, created.buffer, &requirements);
    VkMemoryPropertyFlags mapped = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!vulkanMemoryAllocate(d, requirements, mapped, 0, &created.memory)) {
        d->vk.DestroyBuffer(d->device, created.buffer, nullptr);
        return nullptr;
    }
    d->vk.BindBufferMemory(d->device, created.buffer, created.memory.memory, created.memory.offset);
    uint32_t index = UINT32_MAX;
    for (uint32_t i = 0; i < d->pages.count; ++i) {
        if (d->pages.data[i].buffer == VK_NULL_HANDLE) {
            index = i;
            break;
        }
    }
    if (index == UINT32_MAX) {
        index = d->pages.count;
        d->pages.push(created);
    } else {
        d->pages.data[index] = created;
    }
    // A page of its own for a large upload leaves the open page open.
    if (pageSize == kStagingPageBytes)
        d->openPage = index;
    d->pages.data[index].used = size;
    *page = index;
    *offset = 0;
    return d->pages.data[index].memory.mapped;
}

VkImageViewType viewTypeOf(PrGpuTextureKind kind) {
    switch (kind) {
        case PrGpuTextureKind_2dArray:
            return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case PrGpuTextureKind_Cube:
            return VK_IMAGE_VIEW_TYPE_CUBE;
        case PrGpuTextureKind_CubeArray:
            return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        case PrGpuTextureKind_3d:
            return VK_IMAGE_VIEW_TYPE_3D;
        default:
            return VK_IMAGE_VIEW_TYPE_2D;
    }
}

VkResult createView(VulkanDevice* d, VkImage image, PrGpuFormat format, VkImageViewType type, uint32_t firstLevel,
        uint32_t levels, uint32_t firstLayer, uint32_t layers, VkImageView* out) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = type;
    info.format = vulkanFormat(format);
    // A view that shaders read takes the depth of a format with depth and stencil.
    VkImageAspectFlags aspect = vulkanAspect(format);
    if ((aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
        info.subresourceRange.aspectMask = VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT);
    else
        info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.baseMipLevel = firstLevel;
    info.subresourceRange.levelCount = levels;
    info.subresourceRange.baseArrayLayer = firstLayer;
    info.subresourceRange.layerCount = layers;
    return d->vk.CreateImageView(d->device, &info, nullptr, out);
}

// Writes a texture's views and descriptors into its slots. Returns Limit if no slot is left for its storage image.
// Called with the device's mutex held.
PrStatus describeTexture(VulkanDevice* d, uint32_t slot) {
    VulkanResource& r = vulkanResourceAt(d, slot);
    const PrGpuTextureDesc& desc = r.textureDesc;
    uint32_t layers = desc.kind == PrGpuTextureKind_3d ? 1 : desc.extent.depth;
    if ((desc.usage & PrGpuTextureUsageFlag_Sampled) != 0) {
        if (!vulkanCheck(d, createView(d, r.image, desc.format, viewTypeOf(desc.kind), r.firstLevel, desc.levels,
                r.firstLayer, layers, &r.view)))
            return PrStatus_Limit;
        writeImage(d, slot, r.view, false);
    }
    if ((desc.usage & PrGpuTextureUsageFlag_Storage) != 0) {
        uint32_t storage = vulkanSlotTake(d);
        if (storage == 0)
            return PrStatus_Limit;
        VulkanResource& s = vulkanResourceAt(d, storage);
        s.kind = VulkanSlotKind_StorageView;
        s.owner = slot;
        VkImageViewType type;
        if (desc.kind == PrGpuTextureKind_Cube || desc.kind == PrGpuTextureKind_CubeArray)
            type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        else
            type = viewTypeOf(desc.kind);
        VkResult result = createView(d, r.image, desc.format, type, r.firstLevel, 1, r.firstLayer, layers, &s.view);
        if (!vulkanCheck(d, result)) {
            releaseSlot(d, storage);
            return PrStatus_Limit;
        }
        vulkanResourceAt(d, slot).storage = storage;
        writeImage(d, storage, s.view, true);
    }
    return PrStatus_Ok;
}

// Returns the usage flags of an image. Every image may be copied from and into, because a draw that reads its target
// may copy it. Every image is also sampled, because every texture begins in the accesses of shader reads. Vulkan allows
// a color image in their layout only if it is sampled or an input attachment. A render target is also an input
// attachment, because a pipeline that reads its target reads it as one on a device with local read.
VkImageUsageFlags imageUsageOf(PrGpuTextureUsageFlags usage) {
    VkImageUsageFlags flags =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((usage & PrGpuTextureUsageFlag_Storage) != 0)
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    if ((usage & PrGpuTextureUsageFlag_RenderTarget) != 0)
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    if ((usage & PrGpuTextureUsageFlag_DepthStencil) != 0)
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    return flags;
}

// ---------------------------------------------------------------------------------------------------------------------
// Buffers

PrStatus bufferCreate(PrGpuDevice* device, const PrGpuBufferDesc* desc, PrGpuBuffer* out) {
    VulkanDevice* d = vulkanDevice(device);
    // Every buffer is read through its handle, and may be copied from and into.
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = alignUp(desc->size, uint64_t(16));
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if ((desc->usage & PrGpuBufferUsageFlag_Index) != 0)
        info.usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if ((desc->usage & PrGpuBufferUsageFlag_Indirect) != 0)
        info.usage |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (!vulkanCheck(d, d->vk.CreateBuffer(d->device, &info, nullptr, &buffer)))
        return PrStatus_Limit;
    VkMemoryRequirements requirements;
    d->vk.GetBufferMemoryRequirements(d->device, buffer, &requirements);
    VkMemoryPropertyFlags required = 0;
    VkMemoryPropertyFlags preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (desc->memory == PrGpuMemory_Upload)
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (desc->memory == PrGpuMemory_Readback) {
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    VulkanMemory memory{};
    if (!vulkanMemoryAllocate(d, requirements, required, preferred, &memory)) {
        d->vk.DestroyBuffer(d->device, buffer, nullptr);
        return PrStatus_Limit;
    }
    d->vk.BindBufferMemory(d->device, buffer, memory.memory, memory.offset);
    if (memory.mapped != nullptr)
        prMemZero(memory.mapped, size_t(info.size));
    vulkanName(d, VK_OBJECT_TYPE_BUFFER, uint64_t(buffer), desc->name);

    prMutexLock(&d->mutex);
    uint32_t slot = vulkanSlotTake(d);
    if (slot == 0) {
        prMutexUnlock(&d->mutex);
        d->vk.DestroyBuffer(d->device, buffer, nullptr);
        vulkanMemoryFree(d, memory);
        return PrStatus_Limit;
    }
    VulkanResource& r = vulkanResourceAt(d, slot);
    r.kind = VulkanSlotKind_Buffer;
    r.buffer = buffer;
    r.memory = memory;
    r.bufferDesc = *desc;
    r.bufferDesc.name = PrStringView{};
    vulkanSlotWriteBuffer(d, slot, buffer);
    // Device memory is zeroed by the next command buffer, ahead of the uploads to it.
    if (memory.mapped == nullptr) {
        VulkanUpload fill{};
        fill.kind = VulkanUploadKind_Fill;
        fill.target = slot;
        d->uploads.push(fill);
    }
    prMutexUnlock(&d->mutex);
    *out = PrGpuBuffer{slot};
    return PrStatus_Ok;
}

void bufferDestroy(PrGpuDevice* device, PrGpuBuffer buffer) {
    VulkanDevice* d = vulkanDevice(device);
    prMutexLock(&d->mutex);
    PR_ASSERT(vulkanResourceAt(d, buffer.id).kind == VulkanSlotKind_Buffer);
    vulkanRetire(d, VulkanRetired{VulkanRetireKind_Slot, buffer.id, 0});
    vulkanRetireDone(d);
    prMutexUnlock(&d->mutex);
}

PrGpuBufferDesc bufferDesc(const PrGpuDevice* device, PrGpuBuffer buffer) {
    return vulkanResourceAt(vulkanDevice(device), buffer.id).bufferDesc;
}

// Shaders read and write a buffer through the same storage buffer.
uint32_t bufferStorageHandle(const PrGpuDevice*, PrGpuBuffer buffer) {
    return buffer.id;
}

void* bufferMap(const PrGpuDevice* device, PrGpuBuffer buffer) {
    const VulkanResource& r = vulkanResourceAt(vulkanDevice(device), buffer.id);
    PR_ASSERT(r.memory.mapped != nullptr);
    return r.memory.mapped;
}

void bufferUpload(PrGpuDevice* device, PrGpuBuffer buffer, uint64_t offset, const void* data, uint64_t size) {
    VulkanDevice* d = vulkanDevice(device);
    prMutexLock(&d->mutex);
    uint32_t page = 0;
    uint64_t at = 0;
    uint8_t* staging = stagingTake(d, size, &page, &at);
    if (staging != nullptr) {
        prMemCopy(staging, data, size_t(size));
        VulkanUpload upload{};
        upload.page = page;
        upload.offset = at;
        upload.target = buffer.id;
        upload.targetOffset = offset;
        upload.size = size;
        d->uploads.push(upload);
    }
    prMutexUnlock(&d->mutex);
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures

PrStatus textureCreate(PrGpuDevice* device, const PrGpuTextureDesc* desc, PrGpuTexture* out) {
    VulkanDevice* d = vulkanDevice(device);
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = desc->kind == PrGpuTextureKind_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = vulkanFormat(desc->format);
    if (info.format == VK_FORMAT_UNDEFINED)
        return PrStatus_NotSupported;
    info.extent = VkExtent3D{
        desc->extent.width, desc->extent.height,
        desc->kind == PrGpuTextureKind_3d ? desc->extent.depth : 1,
    };
    info.mipLevels = desc->levels;
    info.arrayLayers = desc->kind == PrGpuTextureKind_3d ? 1 : desc->extent.depth;
    info.samples = VkSampleCountFlagBits(desc->samples);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (desc->kind == PrGpuTextureKind_Cube || desc->kind == PrGpuTextureKind_CubeArray)
        info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    info.usage = imageUsageOf(desc->usage);
    VkImage image = VK_NULL_HANDLE;
    if (!vulkanCheck(d, d->vk.CreateImage(d->device, &info, nullptr, &image)))
        return PrStatus_Limit;
    VkMemoryRequirements requirements;
    d->vk.GetImageMemoryRequirements(d->device, image, &requirements);
    VulkanMemory memory{};
    if (!vulkanMemoryAllocate(d, requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &memory)) {
        d->vk.DestroyImage(d->device, image, nullptr);
        return PrStatus_Limit;
    }
    d->vk.BindImageMemory(d->device, image, memory.memory, memory.offset);
    vulkanName(d, VK_OBJECT_TYPE_IMAGE, uint64_t(image), desc->name);

    VulkanImage* held = alloc<VulkanImage>();
    *held = VulkanImage{image, memory, 1, true};
    prMutexLock(&d->mutex);
    uint32_t slot = vulkanSlotTake(d);
    if (slot == 0) {
        releaseImage(d, held);
        prMutexUnlock(&d->mutex);
        return PrStatus_Limit;
    }
    VulkanResource& r = vulkanResourceAt(d, slot);
    r.kind = VulkanSlotKind_Texture;
    r.held = held;
    r.image = image;
    r.aspect = vulkanAspect(desc->format);
    r.textureDesc = *desc;
    r.textureDesc.name = PrStringView{};
    PrStatus status = describeTexture(d, slot);
    if (status == PrStatus_Ok) {
        // The next command buffer moves the texture into the layout of the accesses that it begins in.
        VulkanUpload initialize{};
        initialize.kind = VulkanUploadKind_Initialize;
        initialize.target = slot;
        d->uploads.push(initialize);
    } else {
        freeSlot(d, slot);
    }
    prMutexUnlock(&d->mutex);
    *out = PrGpuTexture{status == PrStatus_Ok ? slot : 0};
    return status;
}

PrStatus textureCreateView(PrGpuDevice* device, PrGpuTexture texture, PrGpuTextureKind kind,
        PrGpuSubresources subresources, PrGpuTexture* out) {
    VulkanDevice* d = vulkanDevice(device);
    prMutexLock(&d->mutex);
    const VulkanResource& parent = vulkanResourceAt(d, texture.id);
    PR_ASSERT(parent.kind == VulkanSlotKind_Texture);
    PrGpuTextureDesc desc = parent.textureDesc;
    uint8_t levels = subresources.levels != 0 ? subresources.levels : uint8_t(desc.levels - subresources.firstLevel);
    uint32_t layerCount = desc.kind == PrGpuTextureKind_3d ? 1 : desc.extent.depth;
    uint16_t layers = subresources.layers != 0 ? subresources.layers : uint16_t(layerCount - subresources.firstLayer);
    VulkanImage* held = parent.held;
    VkImage image = parent.image;
    VkImageAspectFlags aspect = parent.aspect;
    uint8_t firstLevel = uint8_t(parent.firstLevel + subresources.firstLevel);
    uint16_t firstLayer = desc.kind == PrGpuTextureKind_3d ? 0 : uint16_t(parent.firstLayer + subresources.firstLayer);
    uint32_t slot = vulkanSlotTake(d);
    if (slot == 0) {
        prMutexUnlock(&d->mutex);
        return PrStatus_Limit;
    }
    uint32_t width = desc.extent.width >> subresources.firstLevel;
    uint32_t height = desc.extent.height >> subresources.firstLevel;
    desc.kind = kind;
    desc.levels = levels;
    desc.extent.width = width != 0 ? width : 1;
    desc.extent.height = height != 0 ? height : 1;
    if (desc.kind != PrGpuTextureKind_3d)
        desc.extent.depth = layers;
    ++held->references;
    VulkanResource& r = vulkanResourceAt(d, slot);
    r.kind = VulkanSlotKind_Texture;
    r.held = held;
    r.image = image;
    r.aspect = aspect;
    r.textureDesc = desc;
    r.firstLevel = firstLevel;
    r.firstLayer = firstLayer;
    PrStatus status = describeTexture(d, slot);
    if (status != PrStatus_Ok)
        freeSlot(d, slot);
    prMutexUnlock(&d->mutex);
    *out = PrGpuTexture{status == PrStatus_Ok ? slot : 0};
    return status;
}

void textureDestroy(PrGpuDevice* device, PrGpuTexture texture) {
    VulkanDevice* d = vulkanDevice(device);
    gpuSharedTextureForget(d, texture);
    prMutexLock(&d->mutex);
    PR_ASSERT(vulkanResourceAt(d, texture.id).kind == VulkanSlotKind_Texture);
    vulkanRetire(d, VulkanRetired{VulkanRetireKind_Slot, texture.id, 0});
    vulkanRetireDone(d);
    prMutexUnlock(&d->mutex);
}

PrGpuTextureDesc textureDesc(const PrGpuDevice* device, PrGpuTexture texture) {
    return vulkanResourceAt(vulkanDevice(device), texture.id).textureDesc;
}

// Shaders write a texture through its storage image, in a slot of its own. A texture's native object is its VkImage.
bool textureNative(PrGpuDevice* device, PrGpuTexture texture, PrGpuNativeKind, void* out) {
    *static_cast<uint64_t*>(out) = reinterpret_cast<uint64_t>(vulkanDevice(device)->resources.data[texture.id].image);
    return true;
}

uint32_t textureStorageHandle(const PrGpuDevice* device, PrGpuTexture texture) {
    const VulkanResource& r = vulkanResourceAt(vulkanDevice(device), texture.id);
    PR_ASSERT(r.storage != 0);
    return r.storage;
}

void textureUpload(PrGpuDevice* device, PrGpuTexture texture, const PrGpuTextureRegion* region, const void* data,
        uint32_t rowBytes, uint32_t layerBytes) {
    VulkanDevice* d = vulkanDevice(device);
    const VulkanResource& r = vulkanResourceAt(d, texture.id);
    PrGpuFormatBlock block = prGpuFormatBlock(r.textureDesc.format);
    uint32_t blockRows = (region->extent.height + block.height - 1) / block.height;
    uint32_t blockColumns = (region->extent.width + block.width - 1) / block.width;
    uint32_t packedRow = blockColumns * block.bytes;
    PR_ASSERT(rowBytes >= packedRow && rowBytes % block.bytes == 0);
    uint64_t images = region->extent.depth;
    uint64_t imageBytes = layerBytes != 0 ? layerBytes : uint64_t(rowBytes) * blockRows;
    uint64_t size = imageBytes * (images - 1) + uint64_t(rowBytes) * (blockRows - 1) + packedRow;
    prMutexLock(&d->mutex);
    uint32_t page = 0;
    uint64_t at = 0;
    uint8_t* staging = stagingTake(d, size, &page, &at);
    if (staging != nullptr) {
        prMemCopy(staging, data, size_t(size));
        VulkanUpload upload{};
        upload.page = page;
        upload.offset = at;
        upload.target = texture.id;
        upload.region = *region;
        upload.rowTexels = rowBytes / block.bytes * block.width;
        upload.imageRows = uint32_t(imageBytes / rowBytes) * block.height;
        d->uploads.push(upload);
    }
    prMutexUnlock(&d->mutex);
}

// ---------------------------------------------------------------------------------------------------------------------
// Samplers

VkFilter filterOf(PrGpuFilter filter) {
    return filter == PrGpuFilter_Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

VkSamplerAddressMode addressOf(PrGpuAddress address) {
    switch (address) {
        case PrGpuAddress_Repeat:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case PrGpuAddress_Mirror:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case PrGpuAddress_Border:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

// PrGpuCompare's values are VkCompareOp's.
PrStatus samplerCreate(PrGpuDevice* device, const PrGpuSamplerDesc* desc, PrGpuSampler* out) {
    VulkanDevice* d = vulkanDevice(device);
    VkSamplerCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = filterOf(desc->magnify);
    info.minFilter = filterOf(desc->minify);
    info.mipmapMode = desc->mip == PrGpuFilter_Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = addressOf(desc->address[0]);
    info.addressModeV = addressOf(desc->address[1]);
    info.addressModeW = addressOf(desc->address[2]);
    info.mipLodBias = desc->lodBias;
    info.anisotropyEnable = desc->maxAnisotropy > 1;
    info.maxAnisotropy = float(desc->maxAnisotropy > 1 ? desc->maxAnisotropy : 1);
    info.compareEnable = desc->compare != PrGpuCompare_Never;
    info.compareOp = VkCompareOp(desc->compare);
    info.minLod = desc->minLod;
    info.maxLod = desc->maxLod;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    prMutexLock(&d->mutex);
    if (d->samplers.count >= d->samplerCount) {
        prMutexUnlock(&d->mutex);
        return PrStatus_Limit;
    }
    VulkanSampler sampler{};
    if (!vulkanCheck(d, d->vk.CreateSampler(d->device, &info, nullptr, &sampler.sampler))) {
        prMutexUnlock(&d->mutex);
        return PrStatus_Limit;
    }
    uint32_t slot = d->firstSampler + d->samplers.count;
    d->samplers.push(sampler);
    VkDescriptorImageInfo image{};
    image.sampler = sampler.sampler;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = d->set;
    write.dstBinding = VulkanBinding_Samplers;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &image;
    d->vk.UpdateDescriptorSets(d->device, 1, &write, 0, nullptr);
    prMutexUnlock(&d->mutex);
    *out = PrGpuSampler{slot};
    return PrStatus_Ok;
}

bool supportsFormat(const PrGpuDevice* device, PrGpuFormat format, PrGpuTextureUsageFlags usage) {
    const VulkanDevice* d = vulkanDevice(device);
    if (vulkanFormat(format) == VK_FORMAT_UNDEFINED)
        return false;
    bool compressed = format >= PrGpuFormat_Bc1;
    if (compressed && (usage & ~PrGpuTextureUsageFlag_Sampled))
        return false;
    bool depth = prGpuFormatIsDepthStencil(format);
    if ((usage & PrGpuTextureUsageFlag_DepthStencil) && !depth)
        return false;
    if ((usage & (PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Storage)) && depth)
        return false;
    VkFormatProperties properties;
    d->vk.GetPhysicalDeviceFormatProperties(d->physical, vulkanFormat(format), &properties);
    VkFormatFeatureFlags features = properties.optimalTilingFeatures;
    VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((usage & PrGpuTextureUsageFlag_Sampled) != 0)
        needed |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((usage & PrGpuTextureUsageFlag_Storage) != 0)
        needed |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if ((usage & PrGpuTextureUsageFlag_RenderTarget) != 0)
        needed |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if ((usage & PrGpuTextureUsageFlag_DepthStencil) != 0)
        needed |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    return (features & needed) == needed;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Shared

bool vulkanCheck(VulkanDevice* d, VkResult result) {
    if (result == VK_ERROR_DEVICE_LOST)
        d->lost.store(true);
    return result == VK_SUCCESS;
}

void vulkanName(VulkanDevice* d, VkObjectType type, uint64_t handle, PrStringView name) {
    if (name.size == 0 || d->vk.SetDebugUtilsObjectNameEXT == nullptr || d->messenger == VK_NULL_HANDLE)
        return;
    char text[128];
    size_t size = name.size < sizeof(text) - 1 ? name.size : sizeof(text) - 1;
    prMemCopy(text, name.data, size);
    text[size] = '\0';
    VkDebugUtilsObjectNameInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = text;
    d->vk.SetDebugUtilsObjectNameEXT(d->device, &info);
}

VkFormat vulkanFormat(PrGpuFormat format) {
    switch (format) {
        case PrGpuFormat_R8:
            return VK_FORMAT_R8_UNORM;
        case PrGpuFormat_Rg8:
            return VK_FORMAT_R8G8_UNORM;
        case PrGpuFormat_Rgba8:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case PrGpuFormat_Rgba8Srgb:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case PrGpuFormat_Bgra8:
            return VK_FORMAT_B8G8R8A8_UNORM;
        case PrGpuFormat_Bgra8Srgb:
            return VK_FORMAT_B8G8R8A8_SRGB;
        case PrGpuFormat_Rgb10A2:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case PrGpuFormat_Rg11B10F:
            return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case PrGpuFormat_R16F:
            return VK_FORMAT_R16_SFLOAT;
        case PrGpuFormat_Rg16F:
            return VK_FORMAT_R16G16_SFLOAT;
        case PrGpuFormat_Rgba16F:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case PrGpuFormat_R32F:
            return VK_FORMAT_R32_SFLOAT;
        case PrGpuFormat_Rg32F:
            return VK_FORMAT_R32G32_SFLOAT;
        case PrGpuFormat_Rgba32F:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case PrGpuFormat_R8Uint:
            return VK_FORMAT_R8_UINT;
        case PrGpuFormat_R16Uint:
            return VK_FORMAT_R16_UINT;
        case PrGpuFormat_R32Uint:
            return VK_FORMAT_R32_UINT;
        case PrGpuFormat_Rg32Uint:
            return VK_FORMAT_R32G32_UINT;
        case PrGpuFormat_Rgba32Uint:
            return VK_FORMAT_R32G32B32A32_UINT;
        case PrGpuFormat_D16:
            return VK_FORMAT_D16_UNORM;
        case PrGpuFormat_D32F:
            return VK_FORMAT_D32_SFLOAT;
        case PrGpuFormat_D32FS8:
            return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case PrGpuFormat_Bc1:
            return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case PrGpuFormat_Bc1Srgb:
            return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case PrGpuFormat_Bc3:
            return VK_FORMAT_BC3_UNORM_BLOCK;
        case PrGpuFormat_Bc3Srgb:
            return VK_FORMAT_BC3_SRGB_BLOCK;
        case PrGpuFormat_Bc4:
            return VK_FORMAT_BC4_UNORM_BLOCK;
        case PrGpuFormat_Bc5:
            return VK_FORMAT_BC5_UNORM_BLOCK;
        case PrGpuFormat_Bc6HUfloat:
            return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case PrGpuFormat_Bc7:
            return VK_FORMAT_BC7_UNORM_BLOCK;
        case PrGpuFormat_Bc7Srgb:
            return VK_FORMAT_BC7_SRGB_BLOCK;
        case PrGpuFormat_Astc4x4:
            return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
        case PrGpuFormat_Astc4x4Srgb:
            return VK_FORMAT_ASTC_4x4_SRGB_BLOCK;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

PrGpuFormat vulkanGpuFormat(VkFormat format) {
    for (uint8_t f = 1; f < PrGpuFormat_Count; ++f) {
        if (vulkanFormat(PrGpuFormat(f)) == format)
            return PrGpuFormat(f);
    }
    return PrGpuFormat_None;
}

VkImageAspectFlags vulkanAspect(PrGpuFormat format) {
    switch (format) {
        case PrGpuFormat_D16:
        case PrGpuFormat_D32F:
            return VK_IMAGE_ASPECT_DEPTH_BIT;
        case PrGpuFormat_D32FS8:
            return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default:
            return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

VkImageLayout vulkanLayoutFromAccess(PrGpuAccessFlags access) {
    access &= ~uint32_t(PrGpuAccessFlag_Discard);
    if (access == 0)
        return VK_IMAGE_LAYOUT_UNDEFINED;
    if (access == PrGpuAccessFlag_Present)
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    if (access == PrGpuAccessFlag_CopyRead)
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (access == PrGpuAccessFlag_CopyWrite)
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    constexpr PrGpuAccessFlags attachment = PrGpuAccessFlag_ColorAttachment | PrGpuAccessFlag_DepthStencilRead |
        PrGpuAccessFlag_DepthStencilWrite;
    if ((access & ~attachment) == 0 && (access & (PrGpuAccessFlag_ColorAttachment | PrGpuAccessFlag_DepthStencilWrite)))
        return VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    constexpr PrGpuAccessFlags reads = PrGpuAccessFlag_ShaderRead | PrGpuAccessFlag_DepthStencilRead;
    if ((access & ~reads) == 0)
        return VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
    return VK_IMAGE_LAYOUT_GENERAL;
}

bool vulkanMemoryAllocate(VulkanDevice* d, VkMemoryRequirements requirements, VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred, VulkanMemory* out) {
    *out = VulkanMemory{};
    bool hostVisible = (required & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    if (!hostVisible && d->allocate != nullptr) {
        uint64_t memory = 0;
        uint64_t offset = 0;
        if (!d->allocate(d->memoryContext, requirements.size, requirements.alignment, requirements.memoryTypeBits,
                &memory, &offset))
            return false;
        out->memory = reinterpret_cast<VkDeviceMemory>(memory);
        out->offset = offset;
        out->application = true;
        return true;
    }
    uint32_t type = memoryType(d, requirements.memoryTypeBits, required, preferred);
    if (type == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = type;
    if (!vulkanCheck(d, d->vk.AllocateMemory(d->device, &info, nullptr, &out->memory)))
        return false;
    if (hostVisible) {
        void* mapped = nullptr;
        if (!vulkanCheck(d, d->vk.MapMemory(d->device, out->memory, 0, VK_WHOLE_SIZE, 0, &mapped))) {
            d->vk.FreeMemory(d->device, out->memory, nullptr);
            *out = VulkanMemory{};
            return false;
        }
        out->mapped = static_cast<uint8_t*>(mapped);
    }
    return true;
}

void vulkanMemoryFree(VulkanDevice* d, VulkanMemory& memory) {
    if (memory.memory == VK_NULL_HANDLE)
        return;
    if (memory.application)
        d->free(d->memoryContext, reinterpret_cast<uint64_t>(memory.memory), memory.offset);
    else
        d->vk.FreeMemory(d->device, memory.memory, nullptr);
    memory = VulkanMemory{};
}

uint32_t vulkanSlotTake(VulkanDevice* d) {
    uint32_t slot = 0;
    if (d->freeSlots.count != 0) {
        slot = d->freeSlots.last();
        d->freeSlots.pop();
    } else if (d->nextSlot < d->firstResource + d->resourceCount) {
        slot = d->nextSlot++;
        while (d->resources.count <= slot) {
            VulkanResource empty{};
            d->resources.push(empty);
        }
    }
    if (slot != 0)
        prMemZero(&d->resources.data[slot], sizeof(VulkanResource));
    return slot;
}

void vulkanSlotWriteBuffer(VulkanDevice* d, uint32_t slot, VkBuffer buffer) {
    VkDescriptorBufferInfo info{};
    info.buffer = buffer;
    info.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = d->set;
    write.dstBinding = VulkanBinding_Resources;
    write.dstArrayElement = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    d->vk.UpdateDescriptorSets(d->device, 1, &write, 0, nullptr);
}

void vulkanRetire(VulkanDevice* d, VulkanRetired retired) {
    retired.serial = d->nextSerial;
    d->retired.push(retired);
}

void vulkanRetireDone(VulkanDevice* d) {
    uint64_t oldestOpen = UINT64_MAX;
    for (uint32_t i = 0; i < d->openSerials.count; ++i) {
        if (d->openSerials.data[i] < oldestOpen)
            oldestOpen = d->openSerials.data[i];
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < d->retired.count; ++i) {
        VulkanRetired retired = d->retired.data[i];
        // A slot that uploads still wait to write into is kept for the command buffer that will copy them.
        bool uploading = false;
        for (uint32_t k = 0; k < d->uploads.count && retired.kind == VulkanRetireKind_Slot && !uploading; ++k)
            uploading = d->uploads.data[k].target == retired.index;
        if (oldestOpen < retired.serial || uploading) {
            d->retired.data[kept++] = retired;
            continue;
        }
        if (retired.kind == VulkanRetireKind_Slot) {
            freeSlot(d, retired.index);
        } else {
            VulkanPipeline& pipeline = d->pipelines.data[retired.index - 1];
            d->vk.DestroyPipeline(d->device, pipeline.pipeline, nullptr);
            prMemZero(&pipeline, sizeof(pipeline));
            d->freePipelines.push(retired.index);
        }
    }
    d->retired.count = kept;
}

// Records the batch's uploads in order. Writes to one buffer are ordered by a barrier between them, and writes to
// different buffers run together. Each write to a texture is ordered by the barriers that take it in and out of the
// layout of copies.
void vulkanUploadsRecord(VulkanDevice* d, VulkanCommands* c, VkCommandBuffer buffer) {
    if (d->uploads.count == 0)
        return;
    VkMemoryBarrier2 before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    before.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    before.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &before;
    d->vk.CmdPipelineBarrier2(buffer, &dependency);

    RawArray<uint32_t> written;
    VkMemoryBarrier2 between{};
    between.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    between.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    between.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    between.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    between.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    for (uint32_t i = 0; i < d->uploads.count; ++i) {
        const VulkanUpload& upload = d->uploads.data[i];
        const VulkanResource& target = vulkanResourceAt(d, upload.target);
        if (target.kind == VulkanSlotKind_Buffer) {
            if (written.find(upload.target) != kArrayMissing) {
                dependency.pMemoryBarriers = &between;
                d->vk.CmdPipelineBarrier2(buffer, &dependency);
                written.clear();
            }
            written.push(upload.target);
            if (upload.kind == VulkanUploadKind_Fill) {
                d->vk.CmdFillBuffer(buffer, target.buffer, 0, VK_WHOLE_SIZE, 0);
            } else {
                VkBufferCopy copy{upload.offset, upload.targetOffset, upload.size};
                d->vk.CmdCopyBuffer(buffer, d->pages.data[upload.page].buffer, target.buffer, 1, &copy);
            }
            continue;
        }
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = target.image;
        VkDependencyInfo imageDependency{};
        imageDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        imageDependency.imageMemoryBarrierCount = 1;
        imageDependency.pImageMemoryBarriers = &barrier;
        if (upload.kind == VulkanUploadKind_Initialize) {
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
            barrier.subresourceRange = VkImageSubresourceRange{
                target.aspect, 0, VK_REMAINING_MIP_LEVELS, 0,
                VK_REMAINING_ARRAY_LAYERS
            };
            d->vk.CmdPipelineBarrier2(buffer, &imageDependency);
            continue;
        }
        const PrGpuTextureRegion& region = upload.region;
        bool is3d = target.textureDesc.kind == PrGpuTextureKind_3d;
        VkImageSubresourceRange range{};
        range.aspectMask = target.aspect;
        range.baseMipLevel = target.firstLevel + region.level;
        range.levelCount = 1;
        range.baseArrayLayer = target.firstLayer + (is3d ? 0 : region.firstLayer);
        range.layerCount = is3d ? 1 : region.extent.depth;
        // The texture keeps its texels outside the region, so it leaves the layout of shader reads with them.
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.subresourceRange = range;
        d->vk.CmdPipelineBarrier2(buffer, &imageDependency);

        VkBufferImageCopy copy{};
        copy.bufferOffset = upload.offset;
        copy.bufferRowLength = upload.rowTexels;
        copy.bufferImageHeight = upload.imageRows;
        if ((target.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0)
            copy.imageSubresource.aspectMask = VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT);
        else
            copy.imageSubresource.aspectMask = target.aspect;
        copy.imageSubresource.mipLevel = range.baseMipLevel;
        copy.imageSubresource.baseArrayLayer = range.baseArrayLayer;
        copy.imageSubresource.layerCount = range.layerCount;
        copy.imageOffset = VkOffset3D{int32_t(region.x), int32_t(region.y), int32_t(is3d ? region.z : 0)};
        copy.imageExtent = VkExtent3D{region.extent.width, region.extent.height, is3d ? region.extent.depth : 1};
        d->vk.CmdCopyBufferToImage(buffer, d->pages.data[upload.page].buffer, target.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        d->vk.CmdPipelineBarrier2(buffer, &imageDependency);
    }
    written.reset();

    VkMemoryBarrier2 after{};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    after.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    after.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    dependency.pMemoryBarriers = &after;
    d->vk.CmdPipelineBarrier2(buffer, &dependency);

    // A target that was destroyed since its upload waits for the command buffer that copies into it.
    for (uint32_t i = 0; i < d->retired.count; ++i) {
        VulkanRetired& retired = d->retired.data[i];
        for (uint32_t k = 0; k < d->uploads.count && retired.kind == VulkanRetireKind_Slot; ++k) {
            if (d->uploads.data[k].target == retired.index && retired.serial <= c->serial)
                retired.serial = c->serial + 1;
        }
    }
    // Every page that the uploads used belongs to the command buffer now, and frees once it is done.
    for (uint32_t i = 0; i < d->uploads.count; ++i) {
        if (d->uploads.data[i].kind != VulkanUploadKind_Copy)
            continue;
        uint32_t page = d->uploads.data[i].page;
        if (c->pages.find(page) == kArrayMissing)
            c->pages.push(page);
    }
    d->uploads.count = 0;
    d->openPage = UINT32_MAX;
}

VkImageView vulkanAttachmentView(VulkanDevice* d, uint32_t slot, uint8_t level, uint16_t layer, uint8_t layers) {
    prMutexLock(&d->mutex);
    VulkanResource& r = vulkanResourceAt(d, slot);
    for (uint32_t i = 0; i < r.attachments.count; ++i) {
        const VulkanAttachmentView& a = r.attachments.data[i];
        if (a.level == level && a.layer == layer && a.layers == layers) {
            VkImageView view = r.attachments.data[i].view;
            prMutexUnlock(&d->mutex);
            return view;
        }
    }
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = r.image;
    info.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    info.format = vulkanFormat(r.textureDesc.format);
    info.subresourceRange.aspectMask = r.aspect;
    info.subresourceRange.baseMipLevel = r.firstLevel + level;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.baseArrayLayer = r.firstLayer + layer;
    info.subresourceRange.layerCount = layers;
    VulkanAttachmentView view{level, layer, layers, VK_NULL_HANDLE};
    if (vulkanCheck(d, d->vk.CreateImageView(d->device, &info, nullptr, &view.view)))
        r.attachments.push(view);
    prMutexUnlock(&d->mutex);
    return view.view;
}

uint32_t vulkanTextureAdd(VulkanDevice* d, VkImage image, const PrGpuTextureDesc* desc) {
    VulkanImage* held = alloc<VulkanImage>();
    *held = VulkanImage{image, VulkanMemory{}, 1, false};
    uint32_t slot = vulkanSlotTake(d);
    if (slot == 0) {
        prMemoryFree(held);
        return 0;
    }
    VulkanResource& r = vulkanResourceAt(d, slot);
    r.kind = VulkanSlotKind_Texture;
    r.held = held;
    r.image = image;
    r.aspect = vulkanAspect(desc->format);
    r.textureDesc = *desc;
    r.textureDesc.name = PrStringView{};
    if (describeTexture(d, slot) != PrStatus_Ok) {
        freeSlot(d, slot);
        return 0;
    }
    return slot;
}

void vulkanSlotFree(VulkanDevice* d, uint32_t slot) {
    freeSlot(d, slot);
}

PrStatus vulkanTextureWrap(VulkanDevice* d, VkImage image, const PrGpuTextureDesc* desc, PrGpuTexture* out) {
    prMutexLock(&d->mutex);
    uint32_t slot = vulkanTextureAdd(d, image, desc);
    prMutexUnlock(&d->mutex);
    *out = PrGpuTexture{slot};
    return slot != 0 ? PrStatus_Ok : PrStatus_Limit;
}

PrStatus vulkanBufferWrap(VulkanDevice* d, VkBuffer buffer, const PrGpuBufferDesc* desc, PrGpuBuffer* out) {
    prMutexLock(&d->mutex);
    uint32_t slot = vulkanSlotTake(d);
    if (slot == 0) {
        prMutexUnlock(&d->mutex);
        *out = PrGpuBuffer{0};
        return PrStatus_Limit;
    }
    VulkanResource& r = vulkanResourceAt(d, slot);
    r.kind = VulkanSlotKind_Buffer;
    r.wrapped = true;
    r.buffer = buffer;
    r.bufferDesc = *desc;
    r.bufferDesc.name = PrStringView{};
    vulkanSlotWriteBuffer(d, slot, buffer);
    prMutexUnlock(&d->mutex);
    *out = PrGpuBuffer{slot};
    return PrStatus_Ok;
}

void vulkanResourcesReset(VulkanDevice* d) {
    d->openSerials.count = 0;
    vulkanRetireDone(d);
    for (uint32_t slot = 1; slot < d->resources.count; ++slot) {
        VulkanSlotKind kind = d->resources.data[slot].kind;
        if (kind == VulkanSlotKind_Buffer || kind == VulkanSlotKind_Texture)
            freeSlot(d, slot);
    }
    for (uint32_t i = 0; i < d->samplers.count; ++i)
        d->vk.DestroySampler(d->device, d->samplers.data[i].sampler, nullptr);
    for (uint32_t i = 0; i < d->pipelines.count; ++i) {
        if (d->pipelines.data[i].pipeline != VK_NULL_HANDLE)
            d->vk.DestroyPipeline(d->device, d->pipelines.data[i].pipeline, nullptr);
    }
    for (uint32_t i = 0; i < d->pages.count; ++i) {
        VulkanStagingPage& page = d->pages.data[i];
        if (page.buffer != VK_NULL_HANDLE) {
            d->vk.DestroyBuffer(d->device, page.buffer, nullptr);
            vulkanMemoryFree(d, page.memory);
        }
    }
    d->samplers.reset();
    d->pages.reset();
    d->uploads.reset();
    d->retired.reset();
    d->freeSlots.reset();
    d->freePipelines.reset();
    d->resources.reset();
    d->pipelines.reset();
}

void vulkanResourceFunctions(VulkanDevice* d) {
    d->supportsFormatFn = supportsFormat;
    d->bufferCreateFn = bufferCreate;
    d->bufferDestroyFn = bufferDestroy;
    d->bufferDescFn = bufferDesc;
    d->bufferStorageHandleFn = bufferStorageHandle;
    d->bufferMapFn = bufferMap;
    d->bufferUploadFn = bufferUpload;
    d->textureCreateFn = textureCreate;
    d->textureCreateViewFn = textureCreateView;
    d->textureDestroyFn = textureDestroy;
    d->textureDescFn = textureDesc;
    d->textureStorageHandleFn = textureStorageHandle;
    d->textureNativeFn = textureNative;
    d->textureUploadFn = textureUpload;
    d->samplerCreate = samplerCreate;
}

} // namespace pr_impl
