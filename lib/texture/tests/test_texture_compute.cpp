// A procedural texture source that a compute shader fills on the GPU, realized through the texture cache, and realized
// again on a new device after the first one is lost.

#include "pr/array.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/texture.h"
#include "pr/texture_cache.h"

#include "texture/tests/kTestTextureComputeShaders.h"
#include "tools/harness/harness_gpu.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// The push data of the test's shader (test_texture_compute.slang).
struct CheckerPush {
    uint32_t extent[2];
    uint32_t cell;
    uint32_t _pad;
};

// A checkerboard of `side` texels whose squares are `cell` texels wide. The source keeps two numbers and no pixels. A
// read into a texture dispatches a compute shader that writes the texture. The source has no path into memory, so a
// read into memory fails.
struct Checker {
    uint32_t side;
    uint32_t cell;
    uint32_t fills = 0;

    static void read(void* PR_NULLABLE context, PrTextureRead* read) {
        Checker* checker = static_cast<Checker*>(context);
        const PrTextureReadDesc& asked = *prTextureReadInfo(read);
        if (asked.device == nullptr) {
            prTextureReadComplete(read, PrStatus_NotSupported);
            return;
        }
        prTextureReadComplete(read, checker->fill(asked.device, asked.texture));
    }

    // Dispatches the shader over `texture`, which is in the accesses of shader reads before and after.
    PrStatus fill(PrGpuDevice* device, PrGpuTexture texture) {
        PrGpuShaderLibrary* library = nullptr;
        PrStatus status =
            prGpuShaderLibraryCreate(device, kTestTextureComputeShaders, sizeof(kTestTextureComputeShaders), &library);
        if (status != PrStatus_Ok)
            return status;
        PrGpuPipeline pipeline{};
        status = prGpuPipelineCreateCompute(device, library, "checker", "checker", &pipeline);
        PrGpuCommands* commands = nullptr;
        if (status == PrStatus_Ok && !prGpuCommandsBegin(device, nullptr, &commands))
            status = PrStatus_Lost;
        if (status == PrStatus_Ok) {
            CheckerPush push{.extent = {side, side}, .cell = cell};
            prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
                PrGpuAccessFlag_ComputeWrite);
            prGpuCommandsSetPush(commands, &push, sizeof(push));
            prGpuCommandsSetRwTexture(commands, 0, texture);
            prGpuCommandsSetPipeline(commands, pipeline);
            prGpuCommandsDispatch(commands, PrGpuExtent{(side + 7) / 8, (side + 7) / 8, 1});
            prGpuCommandsTextureBarrier(commands, texture, PrGpuSubresources{}, PrGpuAccessFlag_ComputeWrite,
                PrGpuAccessFlag_ShaderRead);
            prGpuCommandsSubmit(commands);
            ++fills;
        }
        prGpuPipelineRelease(device, pipeline);
        prGpuShaderLibraryDestroy(library);
        return status;
    }

    // Returns a source of the checkerboard. The texture that it fills must be one that a compute shader writes.
    const PrTextureSource* source() {
        PrTextureSourceDesc desc{
            .texture = prTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{side, side}, PrTextureAlpha_None),
            .flags = PrTextureSourceFlag_FillsTextures,
            .usage = PrGpuTextureUsageFlag_Storage,
            .read = read,
            .context = this,
        };
        return prTextureSourceCreate(&desc);
    }
};

// Returns the first byte of the texel at `at` of the region's texture on `device`.
uint8_t texelAt(PrGpuDevice* device, const PrTextureRegion& region, PrIVec2 at) {
    uint8_t texel[4] = {};
    PrIVec2 origin{int32_t(region.rect.left) + at.x, int32_t(region.rect.top) + at.y};
    gpuTextureRead(device, region.texture.texture, PrGpuAccessFlag_ShaderRead, origin, PrPixels{texel, 4, {1, 1}});
    return texel[0];
}

// Returns a device of `backend` that runs compute shaders into Rgba8 textures, or null if the machine has none.
PrGpuDevice* PR_NULLABLE computeDevice(PrGpuBackend backend) {
    PrGpuDevice* device = nullptr;
    if (gpuDeviceCreate(backend, nullptr, &device) != PrStatus_Ok)
        return nullptr;
    PrGpuTextureUsageFlags usage = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_Storage;
    if (!prGpuDeviceSupportsFormat(device, PrGpuFormat_Rgba8, usage)) {
        prGpuDeviceDestroy(device);
        return nullptr;
    }
    return device;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("texture");

TEST_CASE("a source that a compute shader fills is realized on the GPU, and again on the next device after a loss" *
        doctest::skip(!gpuTests())) {
    // Each backend that the build has and that creates a device here.
    const PrGpuBackend backends[] =
        {PrGpuBackend_Metal, PrGpuBackend_Metal4, PrGpuBackend_Vulkan, PrGpuBackend_D3d12, PrGpuBackend_Webgpu};
    PrGpuBackend backend = PrGpuBackend_Count;
    SUBCASE("Metal") { backend = backends[0]; }
    SUBCASE("Metal 4") { backend = backends[1]; }
    SUBCASE("Vulkan") { backend = backends[2]; }
    SUBCASE("Direct3D 12") { backend = backends[3]; }
    SUBCASE("WebGPU") { backend = backends[4]; }
    PrGpuDevice* device = computeDevice(backend);
    if (device == nullptr)
        return;
    Checker checker{.side = 64, .cell = 16};
    const PrTextureSource* source = checker.source();
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    PrTextureCache* cache = prTextureCacheCreate(&desc);

    // lib/draw's ask, for encoded color, gets the texture that the shader wrote. The cache made it for compute writes.
    PrTextureRegion region{};
    REQUIRE(prTextureCacheWait(cache, source, prTextureCacheAskEncoded(0), &region) == PrTextureCacheState_Ready);
    CHECK(checker.fills == 1);
    CHECK(region.extent == PrUVec2{64, 64});
    PrGpuTextureDesc made = prGpuTextureDesc(device, region.texture.texture);
    CHECK((made.usage & PrGpuTextureUsageFlag_Storage) != 0);
    CHECK(texelAt(device, region, PrIVec2{0, 0}) == 255);
    CHECK(texelAt(device, region, PrIVec2{16, 0}) == 0);
    CHECK(texelAt(device, region, PrIVec2{16, 16}) == 255);
    CHECK(texelAt(device, region, PrIVec2{63, 47}) == 0);
    // A second ask is the same texture, and the shader does not run again.
    REQUIRE(prTextureCacheRealize(cache, source, prTextureCacheAskEncoded(0), &region) == PrTextureCacheState_Ready);
    CHECK(checker.fills == 1);

    // The device is lost. On the next device, the cache runs the shader again. Nothing of the texture was kept.
    prGpuDeviceSimulateLoss(device);
    PrGpuDevice* next = computeDevice(backend);
    REQUIRE(next != nullptr);
    prTextureCacheSetDevice(cache, next);
    REQUIRE(prTextureCacheWait(cache, source, prTextureCacheAskEncoded(0), &region) == PrTextureCacheState_Ready);
    CHECK(checker.fills == 2);
    CHECK(texelAt(next, region, PrIVec2{0, 0}) == 255);
    CHECK(texelAt(next, region, PrIVec2{16, 0}) == 0);

    // A consumer that reads into memory cannot use this source.
    PrTextureReadDesc memory = prTextureReadDescAll(prTextureSourceDesc(source));
    PrTextureRead* read = prTextureReadCreate(source, &memory);
    CHECK(prTextureReadStatus(read) == PrStatus_NotSupported);
    prTextureReadDestroy(read);

    prTextureCacheDestroy(cache);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(next);
    prGpuDeviceDestroy(device);
}

TEST_SUITE_END();
