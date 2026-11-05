#include "pr/gpu.h"
#include "pr/mem.h"
#include "pr/utf8.h"

#include "gpu/tests/kTestGpuShaders.h"

#include "tools/harness/test.h"

#include <cstring>
#include <string>
#include <vector>

extern "C" int prTestGpuCApi(void);

namespace pr_impl {

namespace {

// The push data of the test shaders (test_gpu.slang).
struct Push {
    float color[4];
    uint32_t texture;
    uint32_t sampler;
    uint32_t buffer;
    uint32_t output;
    float rect[4];
};

// A device, the test shaders on it, and a target to draw into.
struct Fixture {
    PrGpuDevice* device = nullptr;
    PrGpuShaderLibrary* library = nullptr;

    // A device that runs no shaders, such as a memory device, has no fixture, because the tests draw and dispatch.
    explicit Fixture(PrGpuBackend backend) {
        PrGpuDeviceOptions options{.validation = true};
        if (prGpuDeviceCreate(backend, &options, &device) != PrStatus_Ok)
            return;
        PrStatus status = prGpuShaderLibraryCreate(device, kTestGpuShaders, sizeof(kTestGpuShaders), &library);
        if (status == PrStatus_NotSupported) {
            prGpuDeviceDestroy(device);
            device = nullptr;
            return;
        }
        REQUIRE(status == PrStatus_Ok);
    }

    ~Fixture() {
        prGpuShaderLibraryDestroy(library);
        prGpuDeviceDestroy(device);
    }

    PrGpuTexture target(PrUVec2 extent, PrGpuFormat format = PrGpuFormat_Rgba8) {
        PrGpuTextureDesc desc = prGpuTextureDesc2d(
            format, extent, PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
        PrGpuTexture texture{};
        REQUIRE(prGpuTextureCreate(device, &desc, &texture) == PrStatus_Ok);
        return texture;
    }

    PrGpuPipeline pipeline(const char* vertex, const char* fragment, PrGpuFormat format = PrGpuFormat_Rgba8,
            PrGpuTopology topology = PrGpuTopology_TriangleStrip, bool readsTarget = false) {
        PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(library);
        desc.vertex = vertex;
        desc.fragment = fragment;
        desc.topology = topology;
        desc.formats.colorCount = 1;
        desc.formats.color[0] = format;
        desc.readsTarget = readsTarget;
        PrGpuPipeline p{};
        REQUIRE(prGpuPipelineCreateGraphics(device, &desc, &p) == PrStatus_Ok);
        return p;
    }

    // Returns the texels of a texture of 8-bit RGBA that its passes left.
    std::vector<uint8_t> read(PrGpuTexture texture, uint32_t access = PrGpuAccessFlag_ShaderRead) {
        PrUVec2 extent = prGpuTextureExtent(device, texture);
        std::vector<uint8_t> texels(size_t(extent.x) * extent.y * 4);
        prGpuTextureRead(device, texture, access, PrIVec2{0, 0}, PrPixels{texels.data(), size_t(extent.x) * 4, extent});
        return texels;
    }

    // Returns the bytes of a buffer, copied through a readback buffer.
    std::vector<uint8_t> read(PrGpuBuffer buffer, uint64_t size) {
        PrGpuBufferDesc desc{.size = size, .memory = PrGpuMemory_Readback};
        PrGpuBuffer readback{};
        REQUIRE(prGpuBufferCreate(device, &desc, &readback) == PrStatus_Ok);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        prGpuCommandsBufferBarrier(commands, buffer, PrGpuAccessFlag_ShaderRead | PrGpuAccessFlag_ShaderWrite,
            PrGpuAccessFlag_CopyRead);
        prGpuCommandsCopyBuffer(commands, buffer, 0, readback, 0, size);
        prGpuCommandsSubmitAndWait(commands);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        prMemCopy(bytes.data(), prGpuBufferMap(device, readback), size_t(size));
        prGpuBufferDestroy(device, readback);
        return bytes;
    }

    // Draws a quad over the whole target with the pipeline and push data, into a pass that loads as `load` says and
    // clears to `clear`, and leaves the target to be sampled.
    void draw(PrGpuTexture target, PrGpuPipeline pipeline, Push push, PrGpuLoad load = PrGpuLoad_Clear,
            PrColor clear = PrColor{0, 0, 0, 0}, const float* uniforms = nullptr) {
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        PrGpuSubresources all{};
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc desc = prGpuPassDescColor(target, load, clear);
        PrGpuPass* pass = prGpuPassBegin(commands, &desc);
        prGpuCommandsSetPipeline(commands, pipeline);
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        if (uniforms != nullptr)
            prGpuCommandsSetUniforms(commands, uniforms, sizeof(float) * 4);
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
        prGpuCommandsSubmitAndWait(commands);
    }
};

// Push data that draws `color` over the whole target.
Push fill(float r, float g, float b, float a) {
    return Push{.color = {r, g, b, a}, .rect = {-1, 1, 1, -1}};
}

// Returns the texel at (x, y) of 8-bit RGBA texels `width` wide.
const uint8_t* texel(const std::vector<uint8_t>& texels, uint32_t width, uint32_t x, uint32_t y) {
    return &texels[(size_t(y) * width + x) * 4];
}

bool near(const uint8_t* t, uint8_t r, uint8_t g, uint8_t b, uint8_t a, int tolerance = 1) {
    auto close = [&](uint8_t x, uint8_t y) { return (x > y ? x - y : y - x) <= tolerance; };
    return close(t[0], r) && close(t[1], g) && close(t[2], b) && close(t[3], a);
}

// Runs `test` on a fixture for each backend that the build has and the system has a device for.
template <typename Test>
void onEachBackend(Test test) {
    for (PrGpuBackend backend = PrGpuBackend_Default + 1; backend < PrGpuBackend_Count; ++backend) {
        Fixture f(backend);
        if (f.device == nullptr)
            continue;
        PrStringView name = prGpuBackendName(backend);
        CAPTURE(std::string(name.data, name.size));
        test(f);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the gpu headers compile as C") {
    CHECK(prTestGpuCApi() == 0);
}

TEST_CASE("a backend's name names it, in any case") {
    for (PrGpuBackend backend = 0; backend < PrGpuBackend_Count; ++backend)
        CHECK(prGpuBackendFromName(prGpuBackendName(backend)) == backend);
    CHECK(prGpuBackendFromName("Metal") == PrGpuBackend_Metal);
    CHECK(prGpuBackendFromName("opengl") == PrGpuBackend_Count);
    CHECK(prGpuBackendIsBuilt(PrGpuBackend_Default) == (prGpuBackendIsBuilt(PrGpuBackend_Metal) ||
        prGpuBackendIsBuilt(PrGpuBackend_Metal4) || prGpuBackendIsBuilt(PrGpuBackend_Vulkan) ||
        prGpuBackendIsBuilt(PrGpuBackend_D3d12) || prGpuBackendIsBuilt(PrGpuBackend_Canvas2d)));
}

TEST_CASE("a device reports its backend and limits") {
    onEachBackend([](Fixture& f) {
        PrGpuDeviceInfo info = prGpuDeviceInfo(f.device);
        CHECK(info.backend.size != 0);
        CHECK(info.adapter.size != 0);
        CHECK(prUtf8Validate(info.adapter) == info.adapter.size);
        CHECK(info.maxTextureSize >= 16384);
        CHECK(info.maxResourceHandles > 1000);
        CHECK(prGpuDeviceSupportsFormat(f.device, PrGpuFormat_Rgba8, PrGpuTextureUsageFlag_RenderTarget));
        CHECK_FALSE(prGpuDeviceSupportsFormat(f.device, PrGpuFormat_D32F, PrGpuTextureUsageFlag_RenderTarget));
    });
}

TEST_CASE("a texture's texels are written and read back, clipped to the texture") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture texture = f.target(PrUVec2{4, 3});
        std::vector<uint8_t> texels(4 * 3 * 4);
        for (size_t i = 0; i < texels.size(); ++i)
            texels[i] = uint8_t(i);
        prGpuTextureWrite(f.device, texture, PrIVec2{0, 0}, PrPixelView{texels.data(), 16, {4, 3}});
        CHECK(f.read(texture) == texels);
        // A write that hangs off the top left lands where it overlaps.
        uint8_t white[2 * 2 * 4];
        prMemFill(white, 255, sizeof(white));
        prGpuTextureWrite(f.device, texture, PrIVec2{-1, -1}, PrPixelView{white, 8, {2, 2}});
        std::vector<uint8_t> after = f.read(texture);
        CHECK(near(texel(after, 4, 0, 0), 255, 255, 255, 255, 0));
        CHECK(near(texel(after, 4, 1, 0), 4, 5, 6, 7, 0));
        // A read that hangs off the bottom right reads zeros outside.
        uint8_t corner[2 * 2 * 4];
        prGpuTextureRead(f.device, texture, PrGpuAccessFlag_ShaderRead, PrIVec2{3, 2}, PrPixels{corner, 8, {2, 2}});
        CHECK(near(corner, 44, 45, 46, 47, 0));
        CHECK(near(corner + 4, 0, 0, 0, 0, 0));
        prGpuTextureDestroy(f.device, texture);
    });
}

TEST_CASE("a buffer is created zeroed, in device memory and in memory that the application maps") {
    onEachBackend([](Fixture& f) {
        PrGpuBufferDesc desc{.size = 4096};
        PrGpuBuffer device{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &device) == PrStatus_Ok);
        CHECK(f.read(device, 4096) == std::vector<uint8_t>(4096, 0));
        desc.memory = PrGpuMemory_Upload;
        PrGpuBuffer upload{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &upload) == PrStatus_Ok);
        const uint8_t* bytes = static_cast<const uint8_t*>(prGpuBufferMap(f.device, upload));
        CHECK(std::vector<uint8_t>(bytes, bytes + 4096) == std::vector<uint8_t>(4096, 0));
        prGpuBufferDestroy(f.device, upload);
        prGpuBufferDestroy(f.device, device);
    });
}

TEST_CASE("uploads to one resource before a submission land as they would copied in order") {
    onEachBackend([](Fixture& f) {
        PrGpuBufferDesc desc{.size = 4096, .usage = PrGpuBufferUsageFlag_Storage};
        PrGpuBuffer buffer{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &buffer) == PrStatus_Ok);
        std::vector<uint8_t> expected(4096, 0);
        // Overlapping writes of every shape: within, across the start, across the end, covering, and unaligned.
        struct Write {
            uint64_t offset;
            uint64_t size;
            uint8_t value;
        };
        const Write writes[] = {
            {0, 4096, 1}, {100, 200, 2}, {150, 50, 3}, {50, 100, 4}, {250, 100, 5},
            {0, 1000, 6}, {999, 3, 7},   {1001, 1, 8}, {4000, 96, 9}, {3, 1, 10},
        };
        for (const Write& w : writes) {
            std::vector<uint8_t> bytes(size_t(w.size), w.value);
            prGpuBufferUpload(f.device, buffer, w.offset, bytes.data(), w.size);
            prMemFill(&expected[size_t(w.offset)], w.value, size_t(w.size));
        }
        CHECK(f.read(buffer, 4096) == expected);
        // Writes to one texture land in order too.
        PrGpuTexture texture = f.target(PrUVec2{2, 2});
        for (uint8_t value = 1; value <= 3; ++value) {
            uint8_t texels[2 * 2 * 4];
            prMemFill(texels, value * 40, sizeof(texels));
            prGpuTextureWrite(f.device, texture, PrIVec2{0, 0}, PrPixelView{texels, 8, {2, 2}});
        }
        uint8_t last[4] = {200, 200, 200, 200};
        prGpuTextureWrite(f.device, texture, PrIVec2{1, 1}, PrPixelView{last, 4, {1, 1}});
        std::vector<uint8_t> texels = f.read(texture);
        CHECK(near(texel(texels, 2, 0, 0), 120, 120, 120, 120, 0));
        CHECK(near(texel(texels, 2, 1, 1), 200, 200, 200, 200, 0));
        prGpuTextureDestroy(f.device, texture);
        prGpuBufferDestroy(f.device, buffer);
    });
}

TEST_CASE("a large upload is staged in pieces and lands whole") {
    onEachBackend([](Fixture& f) {
        const uint64_t size = uint64_t(40) << 20;
        PrGpuBufferDesc desc{.size = size};
        PrGpuBuffer buffer{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &buffer) == PrStatus_Ok);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        for (uint64_t i = 0; i < size; ++i)
            bytes[size_t(i)] = uint8_t(i * 7 + (i >> 20));
        prGpuBufferUpload(f.device, buffer, 0, bytes.data(), size);
        CHECK(f.read(buffer, size) == bytes);
        prGpuBufferDestroy(f.device, buffer);
    });
}

TEST_CASE("a pass clears its target, and a draw covers what its vertices cover, with y down in the target") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(PrUVec2{8, 8});
        PrGpuPipeline solid = f.pipeline("quadVertex", "solid");
        // The top half, in clip space with y up.
        Push push = fill(1, 0, 0, 1);
        push.rect[3] = 0;
        f.draw(target, solid, push, PrGpuLoad_Clear, PrColor{0, 0, 1, 1});
        std::vector<uint8_t> texels = f.read(target);
        CHECK(near(texel(texels, 8, 0, 0), 255, 0, 0, 255));
        CHECK(near(texel(texels, 8, 7, 3), 255, 0, 0, 255));
        CHECK(near(texel(texels, 8, 0, 4), 0, 0, 255, 255));
        CHECK(near(texel(texels, 8, 7, 7), 0, 0, 255, 255));
        prGpuPipelineDestroy(f.device, solid);
        prGpuTextureDestroy(f.device, target);
    });
}

TEST_CASE("a draw reads its push data and uniforms") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(PrUVec2{4, 4});
        PrGpuPipeline tinted = f.pipeline("quadVertex", "tinted");
        const float tint[4] = {0.5f, 1, 0, 1};
        f.draw(target, tinted, fill(1, 1, 1, 1), PrGpuLoad_Clear, PrColor{}, tint);
        CHECK(near(texel(f.read(target), 4, 2, 2), 128, 255, 0, 255));
        prGpuPipelineDestroy(f.device, tinted);
        prGpuTextureDestroy(f.device, target);
    });
}

TEST_CASE("a shader samples a texture and pulls vertices from a buffer, both by handle") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture source = f.target(PrUVec2{2, 2});
        const uint8_t texels[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        prGpuTextureWrite(f.device, source, PrIVec2{0, 0}, PrPixelView{texels, 8, {2, 2}});
        PrGpuSampler nearest{};
        PrGpuSamplerDesc samplerDesc = prGpuSamplerDescDefault(PrGpuFilter_Nearest, PrGpuAddress_Clamp);
        REQUIRE(prGpuSamplerIntern(f.device, &samplerDesc, &nearest) == PrStatus_Ok);
        PrGpuSampler again{};
        REQUIRE(prGpuSamplerIntern(f.device, &samplerDesc, &again) == PrStatus_Ok);
        CHECK(again.id == nearest.id);
        PrGpuTexture target = f.target(PrUVec2{4, 4});
        PrGpuPipeline sampled = f.pipeline("quadVertex", "sampled");
        Push push = fill(0, 0, 0, 0);
        push.texture = source.id;
        push.sampler = nearest.id;
        f.draw(target, sampled, push);
        std::vector<uint8_t> out = f.read(target);
        CHECK(near(texel(out, 4, 0, 0), 255, 0, 0, 255));
        CHECK(near(texel(out, 4, 3, 0), 0, 255, 0, 255));
        CHECK(near(texel(out, 4, 0, 3), 0, 0, 255, 255));
        CHECK(near(texel(out, 4, 3, 3), 255, 255, 255, 255));

        // A triangle over the left half, from corners in a buffer.
        const float corners[] = {-1, 1, 0, 1, -1, -1, 0, 1, 0, -1, -1, -1};
        PrGpuBufferDesc desc{.size = sizeof(corners)};
        PrGpuBuffer buffer{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &buffer) == PrStatus_Ok);
        prGpuBufferUpload(f.device, buffer, 0, corners, sizeof(corners));
        PrGpuPipeline pulled = f.pipeline("pulled", "solid", PrGpuFormat_Rgba8, PrGpuTopology_TriangleList);
        Push color = fill(0, 1, 0, 1);
        color.buffer = buffer.id;
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc passDesc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{});
        PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
        prGpuCommandsSetPipeline(commands, pulled);
        prGpuCommandsSetPush(commands, &color, sizeof(color));
        prGpuPassDraw(pass, 6, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        prGpuCommandsSubmitAndWait(commands);
        out = f.read(target);
        CHECK(near(texel(out, 4, 0, 0), 0, 255, 0, 255));
        CHECK(near(texel(out, 4, 1, 3), 0, 255, 0, 255));
        CHECK(near(texel(out, 4, 2, 0), 0, 0, 0, 0));
        prGpuPipelineDestroy(f.device, pulled);
        prGpuPipelineDestroy(f.device, sampled);
        prGpuBufferDestroy(f.device, buffer);
        prGpuTextureDestroy(f.device, target);
        prGpuTextureDestroy(f.device, source);
    });
}

TEST_CASE("a shader follows a handle that it reads from a buffer, to a buffer in the stream") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(PrUVec2{2, 2});
        PrGpuPipeline followed = f.pipeline("quadVertex", "followed");
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        // The color, in the stream, and the buffer that names it.
        PrGpuBuffer colorBuffer{};
        uint64_t colorOffset = 0;
        float* color = static_cast<float*>(prGpuCommandsStream(commands, 16, &colorBuffer, &colorOffset));
        color[0] = 0;
        color[1] = 0;
        color[2] = 1;
        color[3] = 1;
        CHECK(colorOffset % 256 == 0);
        PrGpuBufferDesc desc{.size = 16};
        PrGpuBuffer names{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &names) == PrStatus_Ok);
        uint32_t at[2] = {colorBuffer.id, uint32_t(colorOffset)};
        prGpuBufferUpload(f.device, names, 0, at, sizeof(at));
        Push push = fill(0, 0, 0, 0);
        push.buffer = names.id;
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc passDesc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{});
        PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
        prGpuCommandsSetPipeline(commands, followed);
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        prGpuCommandsSubmitAndWait(commands);
        CHECK(near(texel(f.read(target), 2, 1, 1), 0, 0, 255, 255));
        prGpuBufferDestroy(f.device, names);
        prGpuPipelineDestroy(f.device, followed);
        prGpuTextureDestroy(f.device, target);
    });
}

TEST_CASE("a dispatch writes a buffer that a copy reads after a barrier") {
    onEachBackend([](Fixture& f) {
        PrGpuPipeline doubled{};
        REQUIRE(prGpuPipelineCreateCompute(f.device, f.library, "doubled", "doubled", &doubled) == PrStatus_Ok);
        PrGpuExtent group = prGpuPipelineGroupSize(f.device, doubled);
        CHECK(group.width == 64);
        CHECK(group.height == 1);
        const uint32_t count = 256;
        std::vector<uint32_t> input(count);
        for (uint32_t i = 0; i < count; ++i)
            input[i] = i;
        PrGpuBufferDesc desc{.size = count * 4, .usage = PrGpuBufferUsageFlag_Storage};
        PrGpuBuffer in{};
        PrGpuBuffer out{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &in) == PrStatus_Ok);
        REQUIRE(prGpuBufferCreate(f.device, &desc, &out) == PrStatus_Ok);
        prGpuBufferUpload(f.device, in, 0, input.data(), count * 4);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        Push push{};
        push.buffer = in.id;
        push.output = prGpuBufferStorageHandle(f.device, out);
        prGpuCommandsSetPipeline(commands, doubled);
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuCommandsDispatch(commands, PrGpuExtent{count / 64, 1, 1});
        prGpuCommandsBufferBarrier(commands, out, PrGpuAccessFlag_ComputeWrite, PrGpuAccessFlag_ShaderRead);
        prGpuCommandsSubmitAndWait(commands);
        std::vector<uint8_t> bytes = f.read(out, count * 4);
        const uint32_t* words = reinterpret_cast<const uint32_t*>(bytes.data());
        CHECK(words[0] == 0);
        CHECK(words[1] == 2);
        CHECK(words[255] == 510);
        prGpuBufferDestroy(f.device, in);
        prGpuBufferDestroy(f.device, out);
        prGpuPipelineDestroy(f.device, doubled);
    });
}

TEST_CASE("a shader reads the target as the draws before it left it") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(PrUVec2{4, 4});
        PrGpuPipeline solid = f.pipeline("quadVertex", "solid");
        PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(f.library);
        desc.vertex = "quadVertex";
        desc.fragment = "inverted";
        desc.topology = PrGpuTopology_TriangleStrip;
        desc.formats.colorCount = 1;
        desc.formats.color[0] = PrGpuFormat_Rgba8;
        desc.readsTarget = true;
        PrGpuPipeline inverted{};
        REQUIRE(prGpuPipelineCreateGraphics(f.device, &desc, &inverted) == PrStatus_Ok);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc passDesc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{0, 0, 0, 1});
        PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
        // The left half red, then the whole target inverted.
        Push left = fill(1, 0, 0, 1);
        left.rect[2] = 0;
        prGpuCommandsSetPipeline(commands, solid);
        prGpuCommandsSetPush(commands, &left, sizeof(left));
        prGpuPassDraw(pass, 4, 1, 0, 0);
        Push all = fill(0, 0, 0, 0);
        prGpuCommandsSetPipeline(commands, inverted);
        prGpuCommandsSetPush(commands, &all, sizeof(all));
        prGpuPassSetScissor(pass, PrIRect{0, 0, 4, 4});
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        prGpuCommandsSubmitAndWait(commands);
        std::vector<uint8_t> texels = f.read(target);
        CHECK(near(texel(texels, 4, 0, 0), 0, 255, 255, 255));
        CHECK(near(texel(texels, 4, 3, 3), 255, 255, 255, 255));
        prGpuPipelineDestroy(f.device, inverted);
        prGpuPipelineDestroy(f.device, solid);
        prGpuTextureDestroy(f.device, target);
    });
}

TEST_CASE("objects destroyed after their work is recorded live until it is done") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(PrUVec2{4, 4});
        PrGpuTexture source = f.target(PrUVec2{1, 1});
        const uint8_t green[] = {0, 255, 0, 255};
        prGpuTextureWrite(f.device, source, PrIVec2{0, 0}, PrPixelView{green, 4, {1, 1}});
        PrGpuPipeline sampled = f.pipeline("quadVertex", "sampled");
        PrGpuSampler sampler{};
        PrGpuSamplerDesc samplerDesc = prGpuSamplerDescDefault(PrGpuFilter_Nearest, PrGpuAddress_Clamp);
        REQUIRE(prGpuSamplerIntern(f.device, &samplerDesc, &sampler) == PrStatus_Ok);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc passDesc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{});
        PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
        Push push = fill(0, 0, 0, 0);
        push.texture = source.id;
        push.sampler = sampler.id;
        prGpuCommandsSetPipeline(commands, sampled);
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        // Destroyed before the submission, and new objects take no slot that the work still reads.
        prGpuTextureDestroy(f.device, source);
        prGpuPipelineDestroy(f.device, sampled);
        PrGpuTexture other = f.target(PrUVec2{1, 1});
        prGpuCommandsSubmitAndWait(commands);
        CHECK(near(texel(f.read(target), 4, 2, 2), 0, 255, 0, 255));
        prGpuTextureDestroy(f.device, other);
        prGpuTextureDestroy(f.device, target);
    });
}

TEST_CASE("mip levels are filled with the average of the level before") {
    onEachBackend([](Fixture& f) {
        PrGpuTextureDesc desc = prGpuTextureDesc2d(
            PrGpuFormat_Rgba8, PrUVec2{4, 4}, PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        desc.levels = prGpuMipLevels(PrUVec2{4, 4});
        CHECK(desc.levels == 3);
        PrGpuTexture texture{};
        REQUIRE(prGpuTextureCreate(f.device, &desc, &texture) == PrStatus_Ok);
        // Columns alternating black and white, so every texel of the levels after is gray.
        uint8_t texels[4 * 4 * 4];
        for (int i = 0; i < 16; ++i)
            prMemFill(texels + i * 4, (i % 2) != 0 ? 255 : 0, 4);
        prGpuTextureWrite(f.device, texture, PrIVec2{0, 0}, PrPixelView{texels, 16, {4, 4}});
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsGenerateMipmaps(commands, texture, PrGpuAccessFlag_ShaderRead);
        prGpuCommandsSubmitAndWait(commands);
        PrGpuTexture last{};
        REQUIRE(prGpuTextureCreateView(f.device, texture, PrGpuTextureKind_2d,
            PrGpuSubresources{.firstLevel = 2, .levels = 1, .layers = 1}, &last) == PrStatus_Ok);
        CHECK(prGpuTextureExtent(f.device, last).x == 1);
        std::vector<uint8_t> top = f.read(last);
        CHECK(near(top.data(), 128, 128, 128, 128, 2));
        prGpuTextureDestroy(f.device, last);
        prGpuTextureDestroy(f.device, texture);
    });
}

TEST_CASE("a command buffer reports its time and the texels that it read when it is done") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(PrUVec2{3, 2});
        PrGpuPipeline solid = f.pipeline("quadVertex", "solid");
        f.draw(target, solid, fill(0, 0, 1, 1));
        struct Read {
            std::vector<uint8_t> texels;
            double seconds = -1;
        } read;
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsReadTexture(commands, target, PrGpuAccessFlag_ShaderRead, [](void* context, PrPixelView texels) {
            std::vector<uint8_t>& into = static_cast<Read*>(context)->texels;
            for (uint32_t y = 0; y < texels.extent.y; ++y)
                into.insert(into.end(), texels.data + y * texels.stride,
                    texels.data + y * texels.stride + texels.extent.x * 4);
        }, &read);
        prGpuCommandsOnDone(
            commands, [](void* context, double seconds) { static_cast<Read*>(context)->seconds = seconds; }, &read);
        PrGpuFence fence = prGpuCommandsSubmit(commands);
        prGpuDeviceWait(f.device, fence);
        CHECK(prGpuDeviceIsComplete(f.device, fence));
        // The functions run on the device's thread, after the fence, so the test waits for them.
        prGpuDeviceWaitIdle(f.device);
        for (int i = 0; i < 1000 && read.seconds < 0; ++i)
            prGpuDeviceWaitIdle(f.device);
        CHECK(read.seconds >= 0);
        REQUIRE(read.texels.size() == 3 * 2 * 4);
        CHECK(near(texel(read.texels, 3, 2, 1), 0, 0, 255, 255));
        prGpuPipelineDestroy(f.device, solid);
        prGpuTextureDestroy(f.device, target);
    });
}

TEST_CASE("a lost device refuses new work, and stays safe to call") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture texture = f.target(PrUVec2{2, 2});
        prGpuDeviceSimulateLoss(f.device);
        CHECK(prGpuDeviceIsLost(f.device));
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{2, 2}, PrGpuTextureUsageFlag_Sampled);
        PrGpuTexture more{};
        CHECK(prGpuTextureCreate(f.device, &desc, &more) == PrStatus_Lost);
        CHECK(more.id == 0);
        PrGpuCommands* commands = nullptr;
        CHECK_FALSE(prGpuCommandsBegin(f.device, nullptr, &commands));
        CHECK(prGpuDeviceIsComplete(f.device, 1000));
        prGpuTextureDestroy(f.device, texture);
    });
}
