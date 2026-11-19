// The tests of the Vulkan backend's device memory: resources placed in blocks, past the device's limit of allocations,
// and memory from the application's functions.

#include "pr/array.h"
#include "pr/gpu_vulkan.h"
#include "pr/mem.h"
#include "pr/random.h"

#include "tools/harness/harness_gpu.h"

#include "tools/harness/test.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

using namespace harness;
using namespace pr;

namespace pr_impl {

namespace {

// A device with validation, on which a test reports no validation error by its end. A test that finds no Vulkan device
// skips.
struct MemoryFixture {
    PrGpuDevice* device = nullptr;

    explicit MemoryFixture(PrGpuVulkanOptions options) {
        options.validation = true;
        device = prGpuVulkanCreate(&options);
        PrStatus status = prGpuDeviceLostStatus(device);
        if (status == PrStatus_NotSupported) {
            MESSAGE("no Vulkan device with the features that the backend needs");
            prGpuDeviceDestroy(device);
            device = nullptr;
            return;
        }
        REQUIRE(status == PrStatus_Ok);
    }

    ~MemoryFixture() {
        if (device == nullptr)
            return;
        uint32_t errors = prGpuVulkanValidationErrors(device);
        prGpuDeviceDestroy(device);
        CHECK(errors == 0);
    }

    // Returns a function of the device's VkDevice.
    template <typename Fn>
    Fn function(const char* name) const {
        PFN_vkGetInstanceProcAddr getInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(prGpuVulkanGetInstanceProcAddr(device));
        VkInstance instance = static_cast<VkInstance>(prGpuVulkanInstance(device));
        PFN_vkGetDeviceProcAddr get =
            reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
        return reinterpret_cast<Fn>(get(static_cast<VkDevice>(prGpuVulkanDevice(device)), name));
    }

    // Returns the number of VkDeviceMemory allocations that the device allows.
    uint32_t allocationLimit() const {
        PFN_vkGetInstanceProcAddr getInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(prGpuVulkanGetInstanceProcAddr(device));
        VkInstance instance = static_cast<VkInstance>(prGpuVulkanInstance(device));
        PFN_vkGetPhysicalDeviceProperties get = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"));
        VkPhysicalDeviceProperties properties;
        get(static_cast<VkPhysicalDevice>(prGpuVulkanPhysicalDevice(device)), &properties);
        return properties.limits.maxMemoryAllocationCount;
    }

    // Submits the uploads that wait, and waits until the device has retired what was destroyed.
    void settle() {
        for (int i = 0; i < 2; ++i) {
            PrGpuCommands* commands = nullptr;
            REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
            prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
            prGpuDeviceWaitIdle(device);
        }
    }
};

// A buffer of a test, and the seed of its bytes.
struct TestBuffer {
    PrGpuBuffer buffer;
    uint32_t size;
    uint32_t seed;
    bool mapped;
};

// The sizes of the buffers, mixed so that small and large ones share blocks.
constexpr uint32_t kSizes[] = {24, 96, 256, 1000, 4096, 12000, 40000};

// Fills `bytes` with the bytes that `seed` gives.
void pattern(uint8_t* bytes, uint32_t size, uint32_t seed) {
    PrRandom random{.state = seed};
    for (uint32_t i = 0; i < size; ++i)
        bytes[i] = uint8_t(prRandomU32(&random));
}

// Creates the buffer `index` of a test, in device memory or, one in five, in memory that the test maps, and writes its
// bytes.
TestBuffer makeBuffer(PrGpuDevice* device, uint32_t index, uint32_t seed, Array<uint8_t>& scratch) {
    TestBuffer made{.size = kSizes[index % 7], .seed = seed, .mapped = index % 5 == 0};
    PrGpuBufferDesc desc{.size = made.size, .memory = PrGpuMemory_Device};
    if (made.mapped)
        desc.memory = PrGpuMemory_Upload;
    REQUIRE(prGpuBufferCreate(device, &desc, &made.buffer) == PrStatus_Ok);
    if (made.mapped) {
        pattern(static_cast<uint8_t*>(prGpuBufferMap(device, made.buffer)), made.size, seed);
    } else {
        pattern(scratch.data(), made.size, seed);
        prGpuBufferUpload(device, made.buffer, 0, scratch.data(), made.size);
    }
    return made;
}

// Returns the number of the buffers at every `step`th index from `first` whose bytes are not what their seeds give.
// Copies them through one readback buffer.
uint32_t wrongBuffers(PrGpuDevice* device, const Array<TestBuffer>& buffers, uint32_t first, uint32_t step) {
    uint64_t total = 0;
    for (uint32_t i = first; i < buffers.count(); i += step)
        total += (buffers[i].size + 15u) & ~15u;
    PrGpuBufferDesc desc{.size = total, .memory = PrGpuMemory_Readback};
    PrGpuBuffer readback{};
    REQUIRE(prGpuBufferCreate(device, &desc, &readback) == PrStatus_Ok);
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
    uint64_t at = 0;
    for (uint32_t i = first; i < buffers.count(); i += step) {
        prGpuCommandsBufferBarrier(commands, buffers[i].buffer, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_CopyRead);
        prGpuCommandsCopyBuffer(commands, buffers[i].buffer, 0, readback, at, buffers[i].size);
        at += (buffers[i].size + 15u) & ~15u;
    }
    prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
    const uint8_t* bytes = static_cast<const uint8_t*>(prGpuBufferMap(device, readback));
    Array<uint8_t> expected(40000);
    uint32_t wrong = 0;
    at = 0;
    for (uint32_t i = first; i < buffers.count(); i += step) {
        pattern(expected.data(), buffers[i].size, buffers[i].seed);
        if (!prMemEquals(bytes + at, expected.data(), buffers[i].size))
            ++wrong;
        at += (buffers[i].size + 15u) & ~15u;
    }
    prGpuBufferRelease(device, readback);
    return wrong;
}

// The application's memory of a test: one VkDeviceMemory for each call, and a count of the calls.
struct AppMemory {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    VkPhysicalDeviceMemoryProperties properties{};
    uint32_t allocated = 0;
    uint32_t freed = 0;
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vulkan places more buffers and textures than the device allows allocations, and keeps their contents") {
    const bool slotModes[] = {false, true};
    for (bool slotsOnly : slotModes) {
        CAPTURE(slotsOnly);
        PrGpuVulkanOptions options = prGpuVulkanOptionsDefault();
        options.slotsOnly = slotsOnly;
        MemoryFixture f(options);
        if (f.device == nullptr)
            return;
        uint32_t limit = f.allocationLimit();
        // Each count passes the limit on most systems and stays within the device's handles. Some drivers allow 2^32 -
        // 1 allocations, more than the test creates resources. On those, the test checks only that the resources fit in
        // few allocations.
        constexpr uint32_t kMaxBuffers = 24000;
        constexpr uint32_t kMaxTextures = 4000;
        uint32_t bufferCount = uint64_t(limit) + limit / 2 < kMaxBuffers ? limit + limit / 2 : kMaxBuffers;
        uint32_t textureCount = limit / 4 < kMaxTextures ? limit / 4 : kMaxTextures;
        Array<uint8_t> scratch(40000);
        Array<TestBuffer> buffers;
        for (uint32_t i = 0; i < bufferCount; ++i)
            buffers.push(makeBuffer(f.device, i, i + 1, scratch));
        Array<PrGpuTexture> textures;
        for (uint32_t i = 0; i < textureCount; ++i) {
            uint32_t side = 4u << (i % 4);
            PrGpuTextureDesc desc =
                prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{side, side}, PrGpuTextureUsageFlag_Sampled);
            PrGpuTexture texture{};
            REQUIRE(prGpuTextureCreate(f.device, &desc, &texture) == PrStatus_Ok);
            for (uint32_t t = 0; t < side * side; ++t) {
                uint32_t texel = i * 2654435761u + t;
                prMemCopy(scratch.data() + t * 4, &texel, 4);
            }
            prGpuTextureWrite(f.device, texture, PrIVec2{0, 0}, PrPixelView{scratch.data(), side * 4, {side, side}});
            textures.push(texture);
        }
        PrGpuVulkanMemoryInfo full = prGpuVulkanMemoryInfo(f.device);
        uint32_t resourceCount = bufferCount + textureCount;
        MESSAGE(resourceCount, " resources of ", full.usedBytes, " bytes in ", full.allocations,
            " allocations of ", full.allocatedBytes, " bytes, on a device that allows ", limit);
        if (limit < kMaxBuffers + kMaxTextures)
            CHECK(resourceCount > limit);
        CHECK(full.allocations < 256);
        CHECK(full.usedBytes <= full.allocatedBytes);
        CHECK(wrongBuffers(f.device, buffers, 0, 7) == 0);
        CHECK(wrongBuffers(f.device, buffers, 3, 11) == 0);

        // Every third buffer and every other texture go, and buffers of other sizes take their places.
        for (uint32_t i = 1; i < bufferCount; i += 3)
            prGpuBufferRelease(f.device, buffers[i].buffer);
        for (uint32_t i = 0; i < textureCount; i += 2)
            prGpuTextureRelease(f.device, textures[i]);
        f.settle();
        for (uint32_t i = 1; i < bufferCount; i += 3)
            buffers[i] = makeBuffer(f.device, i + 3, bufferCount + i, scratch);
        CHECK(wrongBuffers(f.device, buffers, 0, 5) == 0);
        CHECK(wrongBuffers(f.device, buffers, 1, 3) == 0);
        uint32_t wrongTextures = 0;
        for (uint32_t i = 1; i < textureCount; i += 2 * (textureCount / 64 + 1)) {
            uint32_t side = 4u << (i % 4);
            Array<uint8_t> texels(side * side * 4);
            gpuTextureRead(f.device, textures[i], PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0},
                PrPixels{texels.data(), side * 4, {side, side}});
            for (uint32_t t = 0; t < side * side; ++t) {
                uint32_t texel = i * 2654435761u + t;
                if (!prMemEquals(texels.data() + t * 4, &texel, 4))
                    ++wrongTextures;
            }
        }
        CHECK(wrongTextures == 0);
        CHECK(prGpuVulkanMemoryInfo(f.device).allocations < 256);

        // With everything destroyed, the device gives the blocks back, but for one small block of each of its three
        // pools. The bytes still in use are its staging and its stream.
        for (const TestBuffer& buffer : buffers)
            prGpuBufferRelease(f.device, buffer.buffer);
        for (uint32_t i = 1; i < textureCount; i += 2)
            prGpuTextureRelease(f.device, textures[i]);
        f.settle();
        PrGpuVulkanMemoryInfo empty = prGpuVulkanMemoryInfo(f.device);
        MESSAGE("after: ", empty.usedBytes, " bytes in ", empty.allocations, " allocations of ", empty.allocatedBytes);
        CHECK(empty.allocatedBytes - empty.usedBytes <= 3 * (uint64_t(16) << 20));
        CHECK(prGpuVulkanValidationErrors(f.device) == 0);
    }
}

TEST_CASE("vulkan's memory stays bounded as buffers and textures of mixed sizes come and go at a steady total") {
    MemoryFixture f(prGpuVulkanOptionsDefault());
    if (f.device == nullptr)
        return;
    struct Resource {
        PrGpuBuffer buffer;
        PrGpuTexture texture;
    };
    PrRandom random{.state = 5};
    // One resource in four is a texture, with its levels, and the rest are buffers of 256 bytes to 512 KiB.
    auto make = [&]() {
        Resource made{};
        if (prRandomU32Below(&random, 4) == 0) {
            uint32_t side = 16u << prRandomU32Below(&random, 5);
            PrGpuTextureDesc desc =
                prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{side, side}, PrGpuTextureUsageFlag_Sampled);
            desc.levels = prGpuMipLevels(PrUVec2{side, side});
            REQUIRE(prGpuTextureCreate(f.device, &desc, &made.texture) == PrStatus_Ok);
        } else {
            uint32_t bits = 8 + prRandomU32Below(&random, 11);
            PrGpuBufferDesc desc{.size = (uint64_t(1) << bits) + prRandomU32Below(&random, 1u << bits)};
            REQUIRE(prGpuBufferCreate(f.device, &desc, &made.buffer) == PrStatus_Ok);
        }
        return made;
    };
    constexpr uint32_t kResources = 1500;
    constexpr uint32_t kRounds = 40;
    Array<Resource> resources;
    for (uint32_t i = 0; i < kResources; ++i)
        resources.push(make());
    f.settle();
    PrGpuVulkanMemoryInfo filled = prGpuVulkanMemoryInfo(f.device);
    // The bytes that the device holds for each byte in use, at worst in each half of the rounds. A round replaces a
    // tenth of the resources.
    double worst[2] = {0, 0};
    uint32_t allocations = 0;
    for (uint32_t round = 0; round < kRounds; ++round) {
        for (uint32_t i = 0; i < kResources / 10; ++i) {
            Resource& resource = resources[prRandomU32Below(&random, kResources)];
            prGpuBufferRelease(f.device, resource.buffer);
            prGpuTextureRelease(f.device, resource.texture);
            resource = make();
        }
        f.settle();
        PrGpuVulkanMemoryInfo info = prGpuVulkanMemoryInfo(f.device);
        double overhead = double(info.allocatedBytes) / double(info.usedBytes);
        if (overhead > worst[round * 2 / kRounds])
            worst[round * 2 / kRounds] = overhead;
        if (info.allocations > allocations)
            allocations = info.allocations;
    }
    MESSAGE("bytes held per byte in use: ", double(filled.allocatedBytes) / double(filled.usedBytes),
        " once filled, then at worst ", worst[0], " and ", worst[1], " in each half of the churn, in at most ",
        allocations, " allocations");
    CHECK(worst[1] < 2);
    CHECK(worst[1] < worst[0] * 1.1);
    CHECK(allocations < 128);
    for (const Resource& resource : resources) {
        prGpuBufferRelease(f.device, resource.buffer);
        prGpuTextureRelease(f.device, resource.texture);
    }
    f.settle();
}

TEST_CASE("vulkan takes device memory from the application's functions, and maps memory of its own") {
    AppMemory app;
    PrGpuVulkanOptions options = prGpuVulkanOptionsDefault();
    options.memoryContext = &app;
    options.allocate = [](void* context, uint64_t size, uint64_t, uint32_t memoryTypeBits, uint64_t* memory, uint64_t* offset) {
        AppMemory& a = *static_cast<AppMemory*>(context);
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < a.properties.memoryTypeCount && type == UINT32_MAX; ++i) {
            bool local = (a.properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
            if ((memoryTypeBits & (1u << i)) != 0 && local)
                type = i;
        }
        VkMemoryAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        info.allocationSize = size;
        info.memoryTypeIndex = type;
        VkDeviceMemory made = VK_NULL_HANDLE;
        if (type == UINT32_MAX || a.allocateMemory(a.device, &info, nullptr, &made) != VK_SUCCESS)
            return false;
        ++a.allocated;
        *memory = reinterpret_cast<uint64_t>(made);
        *offset = 0;
        return true;
    };
    options.free = [](void* context, uint64_t memory, uint64_t) {
        AppMemory& a = *static_cast<AppMemory*>(context);
        a.freeMemory(a.device, reinterpret_cast<VkDeviceMemory>(memory), nullptr);
        ++a.freed;
    };
    {
        MemoryFixture f(options);
        if (f.device == nullptr)
            return;
        app.device = static_cast<VkDevice>(prGpuVulkanDevice(f.device));
        app.allocateMemory = f.function<PFN_vkAllocateMemory>("vkAllocateMemory");
        app.freeMemory = f.function<PFN_vkFreeMemory>("vkFreeMemory");
        PFN_vkGetInstanceProcAddr getInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(prGpuVulkanGetInstanceProcAddr(f.device));
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(getInstanceProcAddr(
            static_cast<VkInstance>(prGpuVulkanInstance(f.device)), "vkGetPhysicalDeviceMemoryProperties"))(
                static_cast<VkPhysicalDevice>(prGpuVulkanPhysicalDevice(f.device)), &app.properties);

        Array<uint8_t> scratch(40000);
        Array<TestBuffer> buffers;
        // Indices 1 to 4 are buffers in device memory, and index 5 is one that the test maps.
        for (uint32_t i = 1; i <= 5; ++i)
            buffers.push(makeBuffer(f.device, i, i, scratch));
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{8, 8}, PrGpuTextureUsageFlag_Sampled);
        PrGpuTexture texture{};
        REQUIRE(prGpuTextureCreate(f.device, &desc, &texture) == PrStatus_Ok);
        // The application gave the memory of the four buffers and the texture. The mapped buffer and the staging of the
        // uploads are the device's own.
        CHECK(app.allocated == 5);
        CHECK(prGpuVulkanMemoryInfo(f.device).usedBytes != 0);
        CHECK(wrongBuffers(f.device, buffers, 0, 1) == 0);
        for (const TestBuffer& buffer : buffers)
            prGpuBufferRelease(f.device, buffer.buffer);
        prGpuTextureRelease(f.device, texture);
        f.settle();
        CHECK(app.freed == 5);
    }
    CHECK(app.freed == app.allocated);
}
