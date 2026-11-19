// The tests of what a 3D renderer's passes use, on every device through slots: passes of several samples and their
// resolve, draws of depth alone in a pass with colors, depth read by texel, indirect draws whose arguments a dispatch
// writes, and every Texture slot at once.

#include "pr/array.h"
#include "pr/gpu.h"
#include "pr/mem.h"

#include "gpu/tests/gpu_fixture.h"
#include "gpu/tests/kTestGpuPassShaders.h"
#include "tools/harness/harness_gpu.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

using namespace pr;

namespace {

// The push data of test_gpu_passes.slang.
struct PassPush {
    float color[4];
    float rect[4];
    float depth;
    uint32_t pattern;
    uint32_t unused[2];
};

// The fixture's device with the passes' shaders.
struct Passes {
    Fixture& f;
    PrGpuShaderLibrary* library = nullptr;

    explicit Passes(Fixture& fixture) : f(fixture) {
        REQUIRE(prGpuShaderLibraryCreate(f.device, kTestGpuPassShaders, sizeof(kTestGpuPassShaders), &library) ==
            PrStatus_Ok);
    }

    ~Passes() {
        prGpuShaderLibraryDestroy(library);
    }

    PrGpuGraphicsPipelineDesc description(const char* vertex, const char* PR_NULLABLE fragment, PrGpuFormat color,
            PrGpuFormat depth = PrGpuFormat_None, uint8_t samples = 1) {
        PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(library);
        desc.vertex = vertex;
        if (fragment != nullptr)
            desc.fragment = fragment;
        desc.topology = PrGpuTopology_TriangleStrip;
        desc.formats.colorCount = color != PrGpuFormat_None ? 1 : 0;
        desc.formats.color[0] = color;
        desc.formats.depthStencil = depth;
        desc.formats.samples = samples;
        return desc;
    }

    PrGpuPipeline pipeline(const PrGpuGraphicsPipelineDesc& desc) {
        PrGpuPipeline p{};
        REQUIRE(prGpuPipelineCreateGraphics(f.device, &desc, &p) == PrStatus_Ok);
        return p;
    }
};

// Push data that draws `color` over the rectangle from (left, top) to (right, bottom) in clip space, at `depth`.
PassPush quad(float r, float g, float b, float left = -1, float right = 1, float depth = 0) {
    return PassPush{.color = {r, g, b, 1}, .rect = {left, 1, right, -1}, .depth = depth};
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a pass of each sample count that the device reports draws its samples and resolves them") {
    onEachBackend([](Fixture& f) {
        Passes p(f);
        PrGpuDeviceInfo info = prGpuDeviceInfo(f.device);
        CHECK((info.sampleCounts & 1) != 0);
        for (uint32_t samples = 2; samples <= 16; samples *= 2) {
            if ((info.sampleCounts & samples) == 0)
                continue;
            CAPTURE(samples);
            PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{8, 8},
                PrGpuTextureUsageFlag_RenderTarget);
            desc.samples = uint8_t(samples);
            PrGpuTexture multisampled{};
            REQUIRE(prGpuTextureCreate(f.device, &desc, &multisampled) == PrStatus_Ok);
            PrGpuTexture resolved = f.target(PrUVec2{8, 8});
            PrGpuPipeline triangle = p.pipeline(p.description("passTriangle", "passSolid", PrGpuFormat_Rgba8,
                PrGpuFormat_None, uint8_t(samples)));

            PrGpuCommands* commands = nullptr;
            REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
            prGpuCommandsTextureBarrier(commands, multisampled, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
                PrGpuAccessFlag_ColorAttachment);
            prGpuCommandsTextureBarrier(commands, resolved, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
                PrGpuAccessFlag_ColorAttachment);
            PrGpuPassDesc passDesc = prGpuPassDescColor(multisampled, PrGpuLoad_Clear, PrColor{0, 0, 0, 1});
            passDesc.color[0].resolve = resolved;
            passDesc.color[0].store = PrGpuStore_Resolve;
            PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
            CHECK(prGpuPassFormats(pass).samples == samples);
            PassPush push = quad(1, 1, 1);
            prGpuCommandsSetPipeline(commands, triangle);
            prGpuCommandsSetPush(commands, &push, sizeof(push));
            prGpuPassDraw(pass, 3, 1, 0, 0);
            prGpuPassEnd(pass);
            prGpuCommandsTextureBarrier(commands, resolved, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
                PrGpuAccessFlag_ShaderRead);
            gpuSubmitAndWait(commands);

            // The triangle covers the left of the target up to its point at the middle of the right edge. Its edges cut
            // pixels, whose samples resolve to gray.
            Array<uint8_t> texels = f.read(resolved);
            CHECK(near(texel(texels, 8, 0, 4), 255, 255, 255, 255));
            CHECK(near(texel(texels, 8, 7, 0), 0, 0, 0, 255));
            CHECK(near(texel(texels, 8, 7, 7), 0, 0, 0, 255));
            uint32_t gray = 0;
            for (uint32_t i = 0; i < 8 * 8; ++i) {
                uint8_t value = texels[i * 4];
                gray += value > 0 && value < 255 ? 1 : 0;
            }
            CHECK(gray >= 4);

            // A resolve that keeps the samples: a second pass draws nothing over them and resolves them again, into a
            // target that then holds what the first pass drew.
            PrGpuTexture again = f.target(PrUVec2{8, 8});
            REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
            prGpuCommandsTextureBarrier(commands, resolved, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
                PrGpuAccessFlag_ColorAttachment);
            prGpuCommandsTextureBarrier(commands, again, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
                PrGpuAccessFlag_ColorAttachment);
            passDesc.color[0].store = PrGpuStore_ResolveKeep;
            pass = prGpuPassBegin(commands, &passDesc);
            prGpuCommandsSetPipeline(commands, triangle);
            prGpuCommandsSetPush(commands, &push, sizeof(push));
            prGpuPassDraw(pass, 3, 1, 0, 0);
            prGpuPassEnd(pass);
            PrGpuPassDesc secondDesc = prGpuPassDescColor(multisampled, PrGpuLoad_Keep, PrColor{});
            secondDesc.color[0].resolve = again;
            secondDesc.color[0].store = PrGpuStore_Resolve;
            pass = prGpuPassBegin(commands, &secondDesc);
            prGpuPassEnd(pass);
            prGpuCommandsTextureBarrier(commands, resolved, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
                PrGpuAccessFlag_ShaderRead);
            prGpuCommandsTextureBarrier(commands, again, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
                PrGpuAccessFlag_ShaderRead);
            gpuSubmitAndWait(commands);
            Array<uint8_t> first = f.read(resolved);
            Array<uint8_t> second = f.read(again);
            CHECK(near(texel(first, 8, 0, 4), 255, 255, 255, 255));
            for (uint32_t i = 0; i < 8 * 8 * 4; ++i)
                CHECK(first[i] == second[i]);
            prGpuTextureRelease(f.device, again);
            prGpuPipelineRelease(f.device, triangle);
            prGpuTextureRelease(f.device, resolved);
            prGpuTextureRelease(f.device, multisampled);
        }
    });
}

TEST_CASE("a draw of depth alone in a pass with a color sets what later draws test, and a shader reads it by texel") {
    onEachBackend([](Fixture& f) {
        Passes p(f);
        PrGpuTextureDesc depthDesc = prGpuTextureDesc2d(PrGpuFormat_D32F, PrUVec2{8, 8},
            PrGpuTextureUsageFlag_DepthStencil | PrGpuTextureUsageFlag_Sampled);
        REQUIRE(prGpuDeviceSupportsFormat(f.device, PrGpuFormat_D32F, depthDesc.usage));
        PrGpuTexture depth{};
        REQUIRE(prGpuTextureCreate(f.device, &depthDesc, &depth) == PrStatus_Ok);
        PrGpuTexture color = f.target(PrUVec2{8, 8});
        PrGpuGraphicsPipelineDesc depthOnly = p.description("passQuad", nullptr, PrGpuFormat_Rgba8, PrGpuFormat_D32F);
        depthOnly.depthTest = true;
        depthOnly.depthWrite = true;
        depthOnly.depthCompare = PrGpuCompare_Less;
        PrGpuPipeline writes = p.pipeline(depthOnly);
        PrGpuGraphicsPipelineDesc testedDesc = p.description("passQuad", "passSolid", PrGpuFormat_Rgba8,
            PrGpuFormat_D32F);
        testedDesc.depthTest = true;
        testedDesc.depthCompare = PrGpuCompare_Less;
        PrGpuPipeline tested = p.pipeline(testedDesc);

        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsTextureBarrier(commands, color, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        prGpuCommandsTextureBarrier(commands, depth, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_DepthStencilWrite);
        PrGpuPassDesc passDesc = prGpuPassDescColor(color, PrGpuLoad_Clear, PrColor{0, 0, 0, 1});
        passDesc.depthStencil = PrGpuAttachment{.texture = depth, .load = PrGpuLoad_Clear};
        passDesc.clearDepth = 1;
        PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
        // Depth alone over the left half, nearer than the color that follows over the whole target.
        PassPush nearer = quad(0, 0, 0, -1, 0, 0.25f);
        prGpuCommandsSetPipeline(commands, writes);
        prGpuCommandsSetPush(commands, &nearer, sizeof(nearer));
        prGpuPassDraw(pass, 4, 1, 0, 0);
        PassPush farther = quad(0, 1, 0, -1, 1, 0.5f);
        prGpuCommandsSetPipeline(commands, tested);
        prGpuCommandsSetPush(commands, &farther, sizeof(farther));
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, color, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        prGpuCommandsTextureBarrier(commands, depth, PrGpuSubresources{}, PrGpuAccessFlag_DepthStencilWrite,
            PrGpuAccessFlag_FragmentRead);
        gpuSubmitAndWait(commands);
        Array<uint8_t> drawn = f.read(color);
        CHECK(near(texel(drawn, 8, 1, 4), 0, 0, 0, 255));
        CHECK(near(texel(drawn, 8, 6, 4), 0, 255, 0, 255));

        // The depth, read by texel through a Texture slot without a sampler.
        PrGpuTexture read = f.target(PrUVec2{8, 8}, PrGpuFormat_R32F);
        PrGpuPipeline reads = p.pipeline(p.description("passQuad", "passDepthRead", PrGpuFormat_R32F));
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsTextureBarrier(commands, read, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        passDesc = prGpuPassDescColor(read, PrGpuLoad_Clear, PrColor{});
        pass = prGpuPassBegin(commands, &passDesc);
        PassPush all = quad(0, 0, 0);
        prGpuCommandsSetTexture(commands, 0, depth, PrGpuSampler{});
        prGpuCommandsSetPipeline(commands, reads);
        prGpuCommandsSetPush(commands, &all, sizeof(all));
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, read, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        prGpuCommandsTextureBarrier(commands, depth, PrGpuSubresources{}, PrGpuAccessFlag_FragmentRead,
            PrGpuAccessFlag_ShaderRead);
        gpuSubmitAndWait(commands);
        float depths[8 * 8];
        gpuTextureRead(f.device, read, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0},
            PrPixels{reinterpret_cast<uint8_t*>(depths), 8 * 4, PrUVec2{8, 8}});
        CHECK(depths[4 * 8 + 1] == 0.25f);
        CHECK(depths[4 * 8 + 6] == 1.0f);

        prGpuPipelineRelease(f.device, reads);
        prGpuPipelineRelease(f.device, tested);
        prGpuPipelineRelease(f.device, writes);
        prGpuTextureRelease(f.device, read);
        prGpuTextureRelease(f.device, color);
        prGpuTextureRelease(f.device, depth);
    });
}

TEST_CASE("indirect draws read their arguments at their stride, as a dispatch wrote them") {
    onEachBackend([](Fixture& f) {
        Passes p(f);
        PrGpuBufferDesc argumentsDesc{
            .size = 128,
            .usage = PrGpuBufferUsageFlag_Storage | PrGpuBufferUsageFlag_Indirect,
        };
        PrGpuBuffer arguments{};
        REQUIRE(prGpuBufferCreate(f.device, &argumentsDesc, &arguments) == PrStatus_Ok);
        const uint32_t indices[] = {0, 1, 2, 3};
        PrGpuBufferDesc indexDesc{.size = sizeof(indices), .usage = PrGpuBufferUsageFlag_Index};
        PrGpuBuffer indexBuffer{};
        REQUIRE(prGpuBufferCreate(f.device, &indexDesc, &indexBuffer) == PrStatus_Ok);
        prGpuBufferUpload(f.device, indexBuffer, 0, indices, sizeof(indices));
        PrGpuPipeline writes{};
        REQUIRE(prGpuPipelineCreateCompute(f.device, p.library, "passArguments", "passArguments", &writes) ==
            PrStatus_Ok);
        PrGpuPipeline solid = p.pipeline(p.description("passQuad", "passSolid", PrGpuFormat_Rgba8));

        const bool indexedModes[] = {false, true};
        for (bool indexed : indexedModes) {
            CAPTURE(indexed);
            PrGpuTexture target = f.target(PrUVec2{4, 4});
            PrGpuCommands* commands = nullptr;
            REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
            prGpuCommandsBufferBarrier(commands, arguments, PrGpuAccessFlag_IndirectRead, PrGpuAccessFlag_ComputeWrite);
            prGpuCommandsSetRwBuffer(commands, 0, arguments, 0);
            prGpuCommandsSetPipeline(commands, writes);
            prGpuCommandsDispatch(commands, PrGpuExtent{1, 1, 1});
            prGpuCommandsBufferBarrier(commands, arguments, PrGpuAccessFlag_ComputeWrite, PrGpuAccessFlag_IndirectRead);
            prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
                PrGpuAccessFlag_ColorAttachment);
            PrGpuPassDesc passDesc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{0, 0, 0, 1});
            PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
            PassPush push = quad(1, 0, 1);
            prGpuCommandsSetPipeline(commands, solid);
            prGpuCommandsSetPush(commands, &push, sizeof(push));
            // The first draw draws nothing, and the second covers the target, so only a read of the second at its
            // stride colors it.
            if (indexed) {
                prGpuPassSetIndexBuffer(pass, indexBuffer, 0, true);
                prGpuPassDrawIndexedIndirect(pass, arguments, 64, 2, 32);
            } else {
                prGpuPassDrawIndirect(pass, arguments, 0, 2, 32);
            }
            prGpuPassEnd(pass);
            prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
                PrGpuAccessFlag_ShaderRead);
            gpuSubmitAndWait(commands);
            Array<uint8_t> texels = f.read(target);
            CHECK(near(texel(texels, 4, 0, 0), 255, 0, 255, 255));
            CHECK(near(texel(texels, 4, 3, 3), 255, 0, 255, 255));
            prGpuTextureRelease(f.device, target);
        }
        prGpuPipelineRelease(f.device, solid);
        prGpuPipelineRelease(f.device, writes);
        prGpuBufferRelease(f.device, indexBuffer);
        prGpuBufferRelease(f.device, arguments);
    });
}

TEST_CASE("a draw samples a texture in each of eight Texture slots") {
    onEachBackend([](Fixture& f) {
        Passes p(f);
        REQUIRE(prGpuDeviceInfo(f.device).maxTextureSlots >= 8);
        PrGpuSampler sampler{};
        PrGpuSamplerDesc samplerDesc = prGpuSamplerDescDefault(PrGpuFilter_Nearest, PrGpuAddress_Clamp);
        REQUIRE(prGpuSamplerIntern(f.device, &samplerDesc, &sampler) == PrStatus_Ok);
        PrGpuTexture textures[8];
        const uint32_t pattern = 0xb2;
        for (uint32_t i = 0; i < 8; ++i) {
            textures[i] = f.target(PrUVec2{1, 1});
            uint8_t value = (pattern >> i) & 1 ? 255 : 0;
            const uint8_t texel[] = {value, 0, 0, 255};
            prGpuTextureWrite(f.device, textures[i], PrIVec2{0, 0}, PrPixelView{texel, 4, {1, 1}});
        }
        PrGpuPipeline eight = p.pipeline(p.description("passQuad", "passEightSlots", PrGpuFormat_Rgba8));
        PrGpuTexture target = f.target(PrUVec2{2, 2});
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc passDesc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{});
        PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
        for (uint32_t i = 0; i < 8; ++i)
            prGpuCommandsSetTexture(commands, i, textures[i], sampler);
        PassPush push = quad(0, 0, 0);
        prGpuCommandsSetPipeline(commands, eight);
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        gpuSubmitAndWait(commands);
        CHECK(near(texel(f.read(target), 2, 1, 1), uint8_t(pattern), 0, 0, 255));
        prGpuTextureRelease(f.device, target);
        prGpuPipelineRelease(f.device, eight);
        for (PrGpuTexture texture : textures)
            prGpuTextureRelease(f.device, texture);
    });
}
