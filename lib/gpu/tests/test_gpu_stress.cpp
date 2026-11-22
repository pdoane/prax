// The tests of a device that several threads use at once: creating, uploading and submitting from each.

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/gpu.h"
#include "pr/mem.h"
#include "pr/thread.h"
#include "pr/time.h"

#include "gpu/tests/gpu_fixture.h"
#include "tools/harness/harness_gpu.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

using namespace pr;

namespace {

constexpr uint32_t kLoaders = 4;
constexpr uint32_t kTexturesPerLoader = 8;
constexpr uint32_t kBufferBytes = 64 * 1024;

// The time that the work of a test may take before the test gives up on it.
constexpr double kPatienceSeconds = 120;

// A thread that loads content as an application's loader does: it creates textures and buffers, uploads them, and
// submits a command buffer that fills each texture's mip levels, without waiting for any of it.
struct Loader {
    PrGpuDevice* device;
    uint32_t index;
    PrThread thread;
    PrGpuTexture textures[kTexturesPerLoader];
    PrGpuBuffer buffers[kTexturesPerLoader];
    PrGpuFence fences[kTexturesPerLoader];
    uint32_t failures;          // creations and command buffers that the device refused
};

// The side of a loader's texture. The first of each loader is larger than a block of staging holds, and the rest are
// small.
uint32_t sideOf(uint32_t texture) {
    return texture == 0 ? 1100 : 64u << (texture % 3);
}

// The color of every texel of a loader's texture, so every level of it holds the same color.
void colorOf(uint32_t loader, uint32_t texture, uint8_t* out) {
    out[0] = uint8_t(20 + 50 * loader);
    out[1] = uint8_t(10 + 30 * texture);
    out[2] = 99;
    out[3] = 255;
}

void load(void* PR_NULLABLE context) {
    Loader& loader = *static_cast<Loader*>(context);
    Array<uint8_t> texels(1100 * 1100 * 4);
    Array<uint32_t> words(kBufferBytes / 4);
    for (uint32_t t = 0; t < kTexturesPerLoader; ++t) {
        uint32_t side = sideOf(t);
        uint8_t color[4];
        colorOf(loader.index, t, color);
        for (uint32_t i = 0; i < side * side; ++i)
            prMemCopy(texels.data() + i * 4, color, 4);
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{side, side},
            PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        desc.levels = prGpuMipLevels(PrUVec2{side, side});
        if (prGpuTextureCreate(loader.device, &desc, &loader.textures[t]) != PrStatus_Ok) {
            ++loader.failures;
            continue;
        }
        prGpuTextureWrite(loader.device, loader.textures[t], PrIVec2{0, 0}, PrPixelView{texels.data(), side * 4, {side, side}});
        for (uint32_t i = 0; i < words.count(); ++i)
            words[i] = (loader.index << 24) | (t << 16) | (i & 0xffff);
        PrGpuBufferDesc bufferDesc{.size = kBufferBytes};
        if (prGpuBufferCreate(loader.device, &bufferDesc, &loader.buffers[t]) != PrStatus_Ok)
            ++loader.failures;
        else
            prGpuBufferUpload(loader.device, loader.buffers[t], 0, words.data(), kBufferBytes);
        PrGpuCommands* commands = nullptr;
        if (!prGpuCommandsBegin(loader.device, nullptr, &commands)) {
            ++loader.failures;
            continue;
        }
        prGpuCommandsGenerateMipmaps(commands, loader.textures[t], PrGpuAccessFlag_ShaderRead);
        loader.fences[t] = prGpuCommandsSubmit(commands);
    }
}

// Waits for a fence, and stops the tests if the device does not reach it in kPatienceSeconds: a device that never
// completes its work would otherwise hold the tests forever, here and when it is destroyed.
void waitPatiently(PrGpuDevice* device, PrGpuFence fence) {
    PrTimePerf start = prTimePerfNow();
    while (!prGpuDeviceIsComplete(device, fence)) {
        if (prTimePerfSeconds(start, prTimePerfNow()) > kPatienceSeconds)
            PR_FATAL("a device did not complete the work that was submitted to it");
        prThreadSleep(1);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("threads create, upload and fill mip levels while another draws frames, and all of the work completes") {
    if (!prThreadSupported())
        return;
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(PrUVec2{64, 64});
        PrGpuPipeline solid = f.pipeline("quadVertex", "solid");
        Loader loaders[kLoaders] = {};
        for (uint32_t i = 0; i < kLoaders; ++i) {
            loaders[i].device = f.device;
            loaders[i].index = i;
            prThreadStart(&loaders[i].thread, load, &loaders[i]);
        }
        // Frames, each submitted while the one before may still run, as a swap chain's are.
        constexpr uint32_t kFrames = 60;
        PrGpuFence frames[kFrames] = {};
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            if (frame >= 2)
                waitPatiently(f.device, frames[frame - 2]);
            PrGpuCommands* commands = nullptr;
            REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
            PrGpuSubresources all{};
            prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ShaderRead,
                PrGpuAccessFlag_ColorAttachment);
            PrGpuPassDesc desc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{});
            PrGpuPass* pass = prGpuPassBegin(commands, &desc);
            Push push = fill(float(frame) / kFrames, 0, 1, 1);
            float uniforms[4] = {0, 0, 0, 0};
            prGpuCommandsSetPipeline(commands, solid);
            prGpuCommandsSetPush(commands, &push, sizeof(push));
            prGpuCommandsSetUniforms(commands, uniforms, sizeof(uniforms));
            prGpuPassDraw(pass, 4, 1, 0, 0);
            prGpuPassEnd(pass);
            prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment,
                PrGpuAccessFlag_ShaderRead);
            frames[frame] = prGpuCommandsSubmit(commands);
        }
        for (Loader& loader : loaders)
            prThreadJoin(&loader.thread);
        waitPatiently(f.device, frames[kFrames - 1]);
        for (const Loader& loader : loaders) {
            CHECK(loader.failures == 0);
            for (PrGpuFence fence : loader.fences)
                waitPatiently(f.device, fence);
        }
        CHECK(near(texel(f.read(target), 64, 32, 32), uint8_t(255 * (kFrames - 1) / kFrames), 0, 255, 255, 2));

        // Every texture's smallest level holds its color, so its upload landed and each of its levels was filled, and
        // every buffer holds its words.
        uint32_t wrongTextures = 0;
        uint32_t wrongBuffers = 0;
        for (const Loader& loader : loaders) {
            for (uint32_t t = 0; t < kTexturesPerLoader; ++t) {
                if (loader.textures[t].id == 0 || loader.buffers[t].id == 0)
                    continue;
                uint8_t color[4];
                colorOf(loader.index, t, color);
                uint8_t last = uint8_t(prGpuMipLevels(PrUVec2{sideOf(t), sideOf(t)}) - 1);
                PrGpuTexture level{};
                REQUIRE(prGpuTextureCreateView(f.device, loader.textures[t], PrGpuTextureViewDesc{
                    .kind = PrGpuTextureKind_2d,
                    .subresources = PrGpuSubresources{.firstLevel = last, .levels = 1, .layers = 1},
                }, &level) == PrStatus_Ok);
                if (!near(f.read(level).data(), color[0], color[1], color[2], color[3], 2))
                    ++wrongTextures;
                prGpuTextureRelease(f.device, level);
                Array<uint8_t> bytes = f.read(loader.buffers[t], kBufferBytes);
                for (uint32_t i = 0; i < kBufferBytes / 4; i += 997) {
                    uint32_t word = (loader.index << 24) | (t << 16) | (i & 0xffff);
                    if (!prMemEquals(bytes.data() + i * 4, &word, 4))
                        ++wrongBuffers;
                }
                prGpuTextureRelease(f.device, loader.textures[t]);
                prGpuBufferRelease(f.device, loader.buffers[t]);
            }
        }
        CHECK(wrongTextures == 0);
        CHECK(wrongBuffers == 0);
        prGpuPipelineRelease(f.device, solid);
        prGpuTextureRelease(f.device, target);
    });
}
