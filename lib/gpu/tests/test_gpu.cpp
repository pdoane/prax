#include "pr/array.h"
#include "pr/float16.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/mem.h"
#include "pr/task.h"
#include "pr/task_backend.h"
#include "pr/utf8.h"

#include "gpu/tests/gpu_fixture.h"
#include "tools/harness/harness_gpu.h"

#include "tools/harness/test.h"

using namespace harness;

extern "C" int prTestGpuCApi(void);

namespace pr_impl {

using namespace pr;

namespace {

// The number of passes in a row that the ordering tests record, each drawing a column of its own 4 texels wide.
constexpr uint32_t kOrderedPasses = 64;

// Returns push data that draws column `i` of kOrderedPasses in the color (r, g, b).
Push orderedColumn(uint32_t i, float r, float g, float b) {
    Push push = fill(r, g, b, 1);
    push.rect[0] = -1 + 2 * float(i) / float(kOrderedPasses);
    push.rect[2] = push.rect[0] + 2 / float(kOrderedPasses);
    return push;
}

// Calls `test` with each backend that the build has and the tests run on, as onEachBackend chooses them.
template <typename Test>
void onEachBuilt(Test test) {
    for (PrGpuBackend backend = PrGpuBackend_Default + 1; backend < PrGpuBackend_Count; ++backend) {
        if (!prBackendIsBuilt(prGpuSelectInterface(), backend) || (!gpuTests() && backend != PrGpuBackend_Memory))
            continue;
        PrStringView name = prBackendName(prGpuSelectInterface(), backend);
        CAPTURE(name);
        test(backend);
    }
}

// A pool that holds the jobs posted to it until the test runs them, so that a test sees a device before and after its
// creation.
struct HeldPool : PrTaskPool {
    struct Job {
        PrTaskJobFn fn;
        void* PR_NULLABLE context;
    };
    Array<Job> jobs;

    HeldPool() : PrTaskPool{
        .widthFn = [](const PrTaskPool*) { return 1u; },
        .runFn = [](PrTaskPool*, uint32_t count, uint32_t, PrTaskFn fn, void* PR_NULLABLE context) {
            for (uint32_t i = 0; i < count; ++i)
                fn(context, i, 0);
        },
        .postFn = [](PrTaskPool* pool, PrTaskJobFn fn, void* PR_NULLABLE context) {
            static_cast<HeldPool*>(pool)->jobs.push(Job{fn, context});
        },
    } {}

    // Runs the jobs that were posted, in order, and returns how many ran.
    uint32_t runJobs() {
        Array<Job> run = static_cast<Array<Job>&&>(jobs);
        jobs = Array<Job>();
        for (const Job& job : run)
            job.fn(job.context);
        return run.count();
    }
};

// The colors of a test cube's faces, by layer.
constexpr uint8_t kFaceColors[6][4] = {
    {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}, {0, 255, 255, 255}, {255, 0, 255, 255},
};

// The direction along the axis of each face of a cube, by layer.
constexpr float kFaceAxes[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};

// The directions in which the columns and the rows of each face of a cube run, by layer.
constexpr float kFaceColumns[6][3] = {{0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
constexpr float kFaceRows[6][3] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};

// Returns the color that a shader samples from `cube` along `direction` at the level of detail `lod`, without
// filtering: by handle on a device with bindless, and through a slot on any other.
Array<uint8_t> sampleCube(Fixture& f, PrGpuTexture cube, const float* direction, float lod) {
    PrGpuSampler nearest{};
    PrGpuSamplerDesc samplerDesc = prGpuSamplerDescDefault(PrGpuFilter_Nearest, PrGpuAddress_Clamp);
    REQUIRE(prGpuSamplerIntern(f.device, &samplerDesc, &nearest) == PrStatus_Ok);
    PrGpuTexture target = f.target(PrUVec2{1, 1});
    PrGpuPipeline pipeline = f.pipeline("quadVertex", f.bindless ? "cubeSampled" : "slotCubeSampled");
    Push push = fill(direction[0], direction[1], direction[2], lod);
    push.texture = cube.id;
    push.sampler = nearest.id;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
    if (!f.bindless)
        prGpuCommandsSetTexture(commands, 5, cube, nearest);
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc desc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{});
    PrGpuPass* pass = prGpuPassBegin(commands, &desc);
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    prGpuPassDraw(pass, 4, 1, 0, 0);
    prGpuPassEnd(pass);
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_ShaderRead);
    gpuSubmitAndWait(commands);
    Array<uint8_t> out = f.read(target);
    prGpuPipelineRelease(f.device, pipeline);
    prGpuTextureRelease(f.device, target);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the gpu headers compile as C") {
    CHECK(prTestGpuCApi() == 0);
}

TEST_CASE("a library names each slot that its source declares, with its kind and its number") {
    PrByteView library{kTestGpuShaders, sizeof(kTestGpuShaders)};
    PrGpuShaderSlot slots[16];
    uint32_t count = prGpuShaderLibrarySlots(library, slots, 16);
    // A build without a backend compiles no target, which reflects nothing.
    if (count == 0)
        return;
    struct Declared {
        PrStringView name;
        PrGpuSlotKind kind;
        uint32_t slot;
    };
    const Declared declared[] = {
        {"slotCorners", PrGpuSlotKind_Buffer, 1},
        {"slotCube", PrGpuSlotKind_Texture, 5},
        {"slotImage", PrGpuSlotKind_Texture, 2},
        {"slotInput", PrGpuSlotKind_Buffer, 3},
        {"slotOutput", PrGpuSlotKind_RwBuffer, 1},
        {"slotWritten", PrGpuSlotKind_RwTexture, 2},
    };
    for (const Declared& d : declared) {
        bool found = false;
        for (uint32_t i = 0; i < count && i < 16; ++i) {
            if (slots[i].name != d.name)
                continue;
            found = true;
            CHECK(slots[i].kind == d.kind);
            CHECK(slots[i].slot == d.slot);
        }
        CHECK_MESSAGE(found, d.name);
    }
    // Bytes that are not a library declare nothing.
    const uint8_t junk[20] = {'P', 'R', 'S', 'L', 2};
    CHECK(prGpuShaderLibrarySlots(PrByteView{junk, sizeof(junk)}, slots, 16) == 0);
}

TEST_CASE("a backend's name names it, in any case") {
    for (PrGpuBackend backend = 0; backend < PrGpuBackend_Count; ++backend)
        CHECK(PrGpuBackend(prBackendFromName(prGpuSelectInterface(), prBackendName(prGpuSelectInterface(), backend))) == backend);
    CHECK(PrGpuBackend(prBackendFromName(prGpuSelectInterface(), "Metal")) == PrGpuBackend_Metal);
    CHECK(PrGpuBackend(prBackendFromName(prGpuSelectInterface(), "opengl")) == PrGpuBackend_Count);
    CHECK(prBackendIsBuilt(prGpuSelectInterface(), PrGpuBackend_Default) ==
        (prBackendIsBuilt(prGpuSelectInterface(), PrGpuBackend_Metal) ||
            prBackendIsBuilt(prGpuSelectInterface(), PrGpuBackend_Metal4) ||
            prBackendIsBuilt(prGpuSelectInterface(), PrGpuBackend_Vulkan) ||
            prBackendIsBuilt(prGpuSelectInterface(), PrGpuBackend_D3d12) ||
            prBackendIsBuilt(prGpuSelectInterface(), PrGpuBackend_Webgpu) ||
            prBackendIsBuilt(prGpuSelectInterface(), PrGpuBackend_Canvas2d)));
}

TEST_CASE("a capture's texels convert to 8-bit RGBA, half floats clamped, and other formats convert to nothing") {
    const uint8_t bgra[] = {10, 20, 30, 40, 50, 60, 70, 80};
    uint8_t rgba[8] = {};
    PrPixels out{rgba, 4, PrUVec2{1, 2}};
    REQUIRE(prGpuCaptureToRgba8(PrPixelView{bgra, 4, PrUVec2{1, 2}}, PrGpuFormat_Bgra8, out));
    CHECK(Array<uint8_t>(rgba) == Array<uint8_t>({30, 20, 10, 40, 70, 60, 50, 80}));
    const PrFloat16 half[] = {
        prFloat16FromFloat(-1), prFloat16FromFloat(0.5f), prFloat16FromFloat(2),
        prFloat16FromFloat(1),
    };
    uint8_t one[4] = {};
    REQUIRE(prGpuCaptureToRgba8(PrPixelView{reinterpret_cast<const uint8_t*>(half), 8, PrUVec2{1, 1}},
        PrGpuFormat_Rgba16F, PrPixels{one, 4, PrUVec2{1, 1}}));
    CHECK(Array<uint8_t>(one) == Array<uint8_t>({0, 128, 255, 255}));
    CHECK_FALSE(prGpuCaptureToRgba8(PrPixelView{bgra, 4, PrUVec2{1, 2}}, PrGpuFormat_Rgb10A2, out));
    CHECK_FALSE(prGpuCaptureToRgba8(PrPixelView{nullptr, 4, PrUVec2{1, 2}}, PrGpuFormat_Bgra8, out));
}

TEST_CASE("a device created on a pool refuses work until its creation runs, then calls back once") {
    onEachBuilt([](PrGpuBackend backend) {
        HeldPool pool;
        uint32_t calls = 0;
        PrGpuDeviceOptions options = prGpuDeviceOptionsDefault();
        options.pool = &pool;
        options.ready = [](void* PR_NULLABLE context, PrGpuDevice* device) {
            ++*static_cast<uint32_t*>(context);
            CHECK(prGpuDeviceIsReady(device));
        };
        options.readyContext = &calls;
        PrGpuDevice* device = prGpuDeviceCreate(backend, &options);
        REQUIRE(device != nullptr);
        CHECK_FALSE(prGpuDeviceIsReady(device));
        CHECK_FALSE(prGpuDeviceIsLost(device));
        CHECK(prGpuDeviceLostStatus(device) == PrStatus_Ok);
        CHECK(prGpuDeviceInfo(device).backend.size != 0);
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{2, 2}, PrGpuTextureUsageFlag_Sampled);
        PrGpuTexture texture{};
        CHECK(prGpuTextureCreate(device, &desc, &texture) == PrStatus_InvalidState);
        CHECK(texture.id == 0);
        PrGpuCommands* commands = nullptr;
        CHECK_FALSE(prGpuCommandsBegin(device, nullptr, &commands));
        CHECK(calls == 0);

        CHECK(pool.runJobs() == 1);
        CHECK(prGpuDeviceIsReady(device));
        CHECK(calls == 1);
        // On a system without what the backend needs, the device is lost, and it refuses work.
        if (prGpuDeviceLostStatus(device) == PrStatus_Ok) {
            REQUIRE(prGpuTextureCreate(device, &desc, &texture) == PrStatus_Ok);
            prGpuTextureRelease(device, texture);
        } else {
            CHECK(prGpuDeviceLostStatus(device) == PrStatus_NotSupported);
            CHECK(prGpuTextureCreate(device, &desc, &texture) == PrStatus_Lost);
        }
        prGpuDeviceDestroy(device);
    });
}

TEST_CASE("a device destroyed while its creation runs is freed once the creation is done") {
    PrTaskPool* pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
    REQUIRE(pool != nullptr);
    onEachBuilt([&](PrGpuBackend backend) {
        PrGpuDeviceOptions options = prGpuDeviceOptionsDefault();
        options.pool = pool;
        prGpuDeviceDestroy(prGpuDeviceCreate(backend, &options));
    });
    prTaskPoolDestroy(pool);
}

TEST_CASE("a device reports its backend and limits") {
    onEachBackend([](Fixture& f) {
        PrGpuDeviceInfo info = prGpuDeviceInfo(f.device);
        CHECK(info.backend.size != 0);
        CHECK(info.adapter.size != 0);
        CHECK(prUtf8Validate(info.adapter) == info.adapter.size);
        CHECK(info.maxTextureSize >= 16384);
        CHECK((info.sampleCounts & 1) != 0);
        // Each device holds what the default limits ask for, or less where its API allows less.
        const PrGpuDeviceLimits defaults = prGpuDeviceLimitsDefault();
        CHECK(info.limits.resources > 1000);
        CHECK(info.limits.resources <= defaults.resources);
        CHECK(info.limits.samplers > 100);
        CHECK(info.limits.samplers <= defaults.samplers);
        CHECK(info.limits.pipelines == defaults.pipelines);
        CHECK(info.maxBufferSlots >= 4);
        CHECK(info.maxRwBufferSlots >= 4);
        CHECK(info.maxTextureSlots >= 8);
        CHECK(info.maxRwTextureSlots >= 4);
        CHECK(prGpuDeviceSupportsFormat(f.device, PrGpuFormat_Rgba8, PrGpuTextureUsageFlag_RenderTarget));
        CHECK_FALSE(prGpuDeviceSupportsFormat(f.device, PrGpuFormat_D32F, PrGpuTextureUsageFlag_RenderTarget));
    });
}

TEST_CASE("Metal, Metal 4 and Vulkan create a device of slots only wherever they create a device") {
    if (!gpuTests())
        return;
    const PrGpuBackend backends[] = {PrGpuBackend_Metal, PrGpuBackend_Metal4, PrGpuBackend_Vulkan};
    for (PrGpuBackend backend : backends) {
        PrStringView name = prBackendName(prGpuSelectInterface(), backend);
        CAPTURE(name);
        Fixture bindless(backend, false);
        if (bindless.device == nullptr)
            continue;
        Fixture slots(backend, true);
        REQUIRE(slots.device != nullptr);
        CHECK_FALSE(slots.bindless);
    }
}

TEST_CASE("a texture's texels are written and read back, clipped to the texture") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture texture = f.target(PrUVec2{4, 3});
        Array<uint8_t> texels(4 * 3 * 4);
        for (uint32_t i = 0; i < texels.count(); ++i)
            texels[i] = uint8_t(i);
        prGpuTextureWrite(f.device, texture, PrIVec2{0, 0}, PrPixelView{texels.data(), 16, {4, 3}});
        CHECK(f.read(texture) == texels);
        // A write that hangs off the top left lands where it overlaps.
        uint8_t white[2 * 2 * 4];
        prMemFill(white, 255, sizeof(white));
        prGpuTextureWrite(f.device, texture, PrIVec2{-1, -1}, PrPixelView{white, 8, {2, 2}});
        Array<uint8_t> after = f.read(texture);
        CHECK(near(texel(after, 4, 0, 0), 255, 255, 255, 255, 0));
        CHECK(near(texel(after, 4, 1, 0), 4, 5, 6, 7, 0));
        // A read that hangs off the bottom right reads zeros outside.
        uint8_t corner[2 * 2 * 4];
        gpuTextureRead(f.device, texture, PrGpuAccessFlag_ShaderRead, PrIVec2{3, 2}, PrPixels{corner, 8, {2, 2}});
        CHECK(near(corner, 44, 45, 46, 47, 0));
        CHECK(near(corner + 4, 0, 0, 0, 0, 0));
        prGpuTextureRelease(f.device, texture);
    });
}

TEST_CASE("an upload reads rows that lie any distance apart, whether or not it is a multiple of a texel's bytes") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture texture = f.target(PrUVec2{2, 3});
        // Rows of two texels, 11 bytes apart.
        uint8_t rows[11 * 2 + 8];
        prMemFill(rows, 0xee, sizeof(rows));
        Array<uint8_t> expected(2 * 3 * 4);
        for (uint32_t y = 0; y < 3; ++y) {
            for (uint32_t i = 0; i < 8; ++i) {
                rows[y * 11 + i] = uint8_t(1 + y * 8 + i);
                expected[y * 8 + i] = uint8_t(1 + y * 8 + i);
            }
        }
        PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, PrUVec2{2, 3});
        prGpuTextureUpload(f.device, texture, &region, rows, 11, 0);
        CHECK(f.read(texture) == expected);
        prGpuTextureRelease(f.device, texture);
    });
}

TEST_CASE("a buffer is created zeroed, in device memory and in memory that the application maps") {
    onEachBackend([](Fixture& f) {
        PrGpuBufferDesc desc{.size = 4096};
        PrGpuBuffer device{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &device) == PrStatus_Ok);
        CHECK(f.read(device, 4096) == Array<uint8_t>(4096));
        desc.memory = PrGpuMemory_Upload;
        PrGpuBuffer upload{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &upload) == PrStatus_Ok);
        const uint8_t* bytes = static_cast<const uint8_t*>(prGpuBufferMap(f.device, upload));
        Array<uint8_t> mapped;
        mapped.append(bytes, 4096);
        CHECK(mapped == Array<uint8_t>(4096));
        prGpuBufferRelease(f.device, upload);
        prGpuBufferRelease(f.device, device);
    });
}

TEST_CASE("uploads to one resource before a submission land as they would copied in order") {
    onEachBackend([](Fixture& f) {
        PrGpuBufferDesc desc{.size = 4096, .usage = PrGpuBufferUsageFlag_Storage};
        PrGpuBuffer buffer{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &buffer) == PrStatus_Ok);
        Array<uint8_t> expected(4096);
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
            Array<uint8_t> bytes(w.size);
            prMemFill(bytes.data(), w.value, bytes.count());
            prGpuBufferUpload(f.device, buffer, w.offset, bytes.data(), w.size);
            prMemFill(&expected[uint32_t(w.offset)], w.value, size_t(w.size));
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
        Array<uint8_t> texels = f.read(texture);
        CHECK(near(texel(texels, 2, 0, 0), 120, 120, 120, 120, 0));
        CHECK(near(texel(texels, 2, 1, 1), 200, 200, 200, 200, 0));
        prGpuTextureRelease(f.device, texture);
        prGpuBufferRelease(f.device, buffer);
    });
}

TEST_CASE("a large upload is staged in pieces and lands whole") {
    onEachBackend([](Fixture& f) {
        const uint64_t size = uint64_t(40) << 20;
        PrGpuBufferDesc desc{.size = size};
        PrGpuBuffer buffer{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &buffer) == PrStatus_Ok);
        Array<uint8_t> bytes(size);
        for (uint64_t i = 0; i < size; ++i)
            bytes[uint32_t(i)] = uint8_t(i * 7 + (i >> 20));
        prGpuBufferUpload(f.device, buffer, 0, bytes.data(), size);
        CHECK(f.read(buffer, size) == bytes);
        prGpuBufferRelease(f.device, buffer);
    });
}

TEST_CASE("uploads land in order across the batches that fill their staging") {
    onEachBackend([](Fixture& f) {
        PrGpuBufferDesc desc{.size = 256};
        PrGpuBuffer small{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &small) == PrStatus_Ok);
        uint8_t first[256];
        prMemFill(first, 1, sizeof(first));
        prGpuBufferUpload(f.device, small, 0, first, sizeof(first));
        // A texture larger than a staging block fills batches of its own between the writes to `small`.
        const PrUVec2 extent{1200, 1200};
        PrGpuTexture large = f.target(extent);
        Array<uint8_t> texels(extent.x * extent.y * 4);
        for (uint32_t i = 0; i < texels.count(); ++i)
            texels[i] = uint8_t(i * 13 + (i >> 16));
        prGpuTextureWrite(f.device, large, PrIVec2{0, 0}, PrPixelView{texels.data(), extent.x * 4, extent});
        uint8_t second[256];
        prMemFill(second, 2, sizeof(second));
        prGpuBufferUpload(f.device, small, 0, second, sizeof(second));
        Array<uint8_t> expected;
        expected.append(second, sizeof(second));
        CHECK(f.read(small, 256) == expected);
        CHECK(f.read(large) == texels);
        prGpuTextureRelease(f.device, large);
        prGpuBufferRelease(f.device, small);
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
        Array<uint8_t> texels = f.read(target);
        CHECK(near(texel(texels, 8, 0, 0), 255, 0, 0, 255));
        CHECK(near(texel(texels, 8, 7, 3), 255, 0, 0, 255));
        CHECK(near(texel(texels, 8, 0, 4), 0, 0, 255, 255));
        CHECK(near(texel(texels, 8, 7, 7), 0, 0, 255, 255));
        prGpuPipelineRelease(f.device, solid);
        prGpuTextureRelease(f.device, target);
    });
}

TEST_CASE("a pass of other attachments draws with the pipeline set in it, not the one that an earlier pass set") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture layer = f.target(PrUVec2{8, 8}, PrGpuFormat_Rgba16F);
        PrGpuTexture target = f.target(PrUVec2{8, 8});
        PrGpuPipeline layerSolid = f.pipeline("quadVertex", "solid", PrGpuFormat_Rgba16F);
        PrGpuPipeline solid = f.pipeline("quadVertex", "solid");
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        PrGpuSubresources all{};
        Push push = fill(0, 1, 0, 1);
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        // The first pass leaves its pipeline set, which the second pass's attachments do not match.
        prGpuCommandsTextureBarrier(commands, layer, all, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc first = prGpuPassDescColor(layer, PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
        PrGpuPass* pass = prGpuPassBegin(commands, &first);
        prGpuCommandsSetPipeline(commands, layerSolid);
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc second = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{0, 0, 1, 1});
        pass = prGpuPassBegin(commands, &second);
        prGpuCommandsSetPipeline(commands, solid);
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
        gpuSubmitAndWait(commands);
        Array<uint8_t> texels = f.read(target);
        CHECK(near(texel(texels, 8, 4, 4), 0, 255, 0, 255));
        prGpuPipelineRelease(f.device, solid);
        prGpuPipelineRelease(f.device, layerSolid);
        prGpuTextureRelease(f.device, target);
        prGpuTextureRelease(f.device, layer);
    });
}

TEST_CASE("a draw reads its push data and uniforms") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(PrUVec2{4, 4});
        PrGpuPipeline tinted = f.pipeline("quadVertex", "tinted");
        const float tint[4] = {0.5f, 1, 0, 1};
        f.draw(target, tinted, fill(1, 1, 1, 1), PrGpuLoad_Clear, PrColor{}, tint);
        CHECK(near(texel(f.read(target), 4, 2, 2), 128, 255, 0, 255));
        prGpuPipelineRelease(f.device, tinted);
        prGpuTextureRelease(f.device, target);
    });
}

TEST_CASE("a shader samples a texture and pulls vertices from a buffer, both by handle") {
    onEachBindless([](Fixture& f) {
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
        Array<uint8_t> out = f.read(target);
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
        gpuSubmitAndWait(commands);
        out = f.read(target);
        CHECK(near(texel(out, 4, 0, 0), 0, 255, 0, 255));
        CHECK(near(texel(out, 4, 1, 3), 0, 255, 0, 255));
        CHECK(near(texel(out, 4, 2, 0), 0, 0, 0, 0));
        prGpuPipelineRelease(f.device, pulled);
        prGpuPipelineRelease(f.device, sampled);
        prGpuBufferRelease(f.device, buffer);
        prGpuTextureRelease(f.device, target);
        prGpuTextureRelease(f.device, source);
    });
}

TEST_CASE("a shader follows a handle that it reads from a buffer, to a buffer in the stream") {
    onEachBindless([](Fixture& f) {
        PrGpuTexture target = f.target(PrUVec2{2, 2});
        PrGpuPipeline followed = f.pipeline("quadVertex", "followed");
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        // The color, in the stream, and the buffer that names it.
        PrGpuBuffer colorBuffer{};
        uint64_t colorOffset = 0;
        float* color = static_cast<float*>(prGpuCommandsStream(commands, 16, 0, &colorBuffer, &colorOffset));
        color[0] = 0;
        color[1] = 0;
        color[2] = 1;
        color[3] = 1;
        CHECK(colorOffset % 16 == 0);
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
        gpuSubmitAndWait(commands);
        CHECK(near(texel(f.read(target), 2, 1, 1), 0, 0, 255, 255));
        prGpuBufferRelease(f.device, names);
        prGpuPipelineRelease(f.device, followed);
        prGpuTextureRelease(f.device, target);
    });
}

TEST_CASE("a dispatch writes a buffer that a copy reads after a barrier") {
    onEachBindless([](Fixture& f) {
        PrGpuPipeline doubled{};
        REQUIRE(prGpuPipelineCreateCompute(f.device, f.library, "doubled", "doubled", &doubled) == PrStatus_Ok);
        PrGpuExtent group = prGpuPipelineGroupSize(f.device, doubled);
        CHECK(group.width == 64);
        CHECK(group.height == 1);
        const uint32_t count = 256;
        Array<uint32_t> input(count);
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
        gpuSubmitAndWait(commands);
        Array<uint8_t> bytes = f.read(out, count * 4);
        const uint32_t* words = reinterpret_cast<const uint32_t*>(bytes.data());
        CHECK(words[0] == 0);
        CHECK(words[1] == 2);
        CHECK(words[255] == 510);
        prGpuBufferRelease(f.device, in);
        prGpuBufferRelease(f.device, out);
        prGpuPipelineRelease(f.device, doubled);
    });
}

TEST_CASE("a device without bindless refuses pipelines that reach resources by handle") {
    onEachBackend([](Fixture& f) {
        if (f.bindless)
            return;
        PrGpuPipeline p{};
        PrGpuGraphicsPipelineDesc sampled = f.description("quadVertex", "sampled");
        CHECK(prGpuPipelineCreateGraphics(f.device, &sampled, &p) == PrStatus_NotSupported);
        PrGpuGraphicsPipelineDesc pulled = f.description("pulled", "solid");
        CHECK(prGpuPipelineCreateGraphics(f.device, &pulled, &p) == PrStatus_NotSupported);
        CHECK(prGpuPipelineCreateCompute(f.device, f.library, "doubled", "doubled", &p) == PrStatus_NotSupported);
        CHECK(p.id == 0);
    });
}

TEST_CASE("a draw fetches its vertices and instances through the pipeline's vertex layout") {
    onEachBackend([](Fixture& f) {
        // A triangle over the target's top left, red, from a buffer of the device's own.
        struct Vertex {
            float position[2];
            uint8_t color[4];
        };
        const Vertex vertices[] = {{{-1, 1}, {255, 0, 0, 255}}, {{0, 1}, {255, 0, 0, 255}}, {{-1, -1}, {255, 0, 0, 255}}};
        PrGpuBufferDesc desc{.size = sizeof(vertices), .usage = PrGpuBufferUsageFlag_Vertex};
        PrGpuBuffer buffer{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &buffer) == PrStatus_Ok);
        prGpuBufferUpload(f.device, buffer, 0, vertices, sizeof(vertices));

        PrGpuGraphicsPipelineDesc pipelineDesc = f.description("fetched", "colored", PrGpuFormat_Rgba8,
            PrGpuTopology_TriangleList);
        pipelineDesc.vertexLayout = PrGpuVertexLayout{
            .bufferCount = 2,
            .attributeCount = 3,
            .buffers = {{.stride = sizeof(Vertex)}, {.stride = 8, .step = PrGpuVertexStep_Instance}},
            .attributes = {
                {.format = PrGpuVertexFormat_Float2, .buffer = 0, .offset = 0},
                {.format = PrGpuVertexFormat_Unorm8x4, .buffer = 0, .offset = 8},
                {.format = PrGpuVertexFormat_Float2, .buffer = 1, .offset = 0},
            },
        };
        PrGpuPipeline pipeline{};
        REQUIRE(prGpuPipelineCreateGraphics(f.device, &pipelineDesc, &pipeline) == PrStatus_Ok);

        PrGpuTexture target = f.target(PrUVec2{4, 4});
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        // Two instances, the second a half of the target to the right, from the stream.
        const float offsets[] = {0, 0, 1, 0};
        PrGpuBuffer stream{};
        uint64_t at = 0;
        prMemCopy(prGpuCommandsStream(commands, sizeof(offsets), PrGpuBufferUsageFlag_Vertex, &stream, &at), offsets,
            sizeof(offsets));
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc passDesc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{});
        PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
        prGpuCommandsSetPipeline(commands, pipeline);
        prGpuPassSetVertexBuffer(pass, 0, buffer, 0);
        prGpuPassSetVertexBuffer(pass, 1, stream, at);
        prGpuPassDraw(pass, 3, 2, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        gpuSubmitAndWait(commands);

        Array<uint8_t> out = f.read(target);
        CHECK(near(texel(out, 4, 0, 0), 255, 0, 0, 255));
        CHECK(near(texel(out, 4, 2, 0), 255, 0, 0, 255));
        CHECK(near(texel(out, 4, 1, 3), 0, 0, 0, 0));
        CHECK(near(texel(out, 4, 3, 3), 0, 0, 0, 0));
        prGpuPipelineRelease(f.device, pipeline);
        prGpuBufferRelease(f.device, buffer);
        prGpuTextureRelease(f.device, target);
    });
}

TEST_CASE("a shader samples a texture and pulls vertices from a buffer, both bound to slots") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture source = f.target(PrUVec2{2, 2});
        const uint8_t texels[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        prGpuTextureWrite(f.device, source, PrIVec2{0, 0}, PrPixelView{texels, 8, {2, 2}});
        PrGpuSampler nearest{};
        PrGpuSamplerDesc samplerDesc = prGpuSamplerDescDefault(PrGpuFilter_Nearest, PrGpuAddress_Clamp);
        REQUIRE(prGpuSamplerIntern(f.device, &samplerDesc, &nearest) == PrStatus_Ok);

        // The corners of a triangle over the left half, at a slot's offset into the buffer.
        const float corners[] = {-1, 1, 0, 1, -1, -1, 0, 1, 0, -1, -1, -1};
        uint32_t at = prGpuDeviceInfo(f.device).storageOffsetAlignment;
        PrGpuBufferDesc desc{.size = at + sizeof(corners), .usage = PrGpuBufferUsageFlag_Storage};
        PrGpuBuffer buffer{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &buffer) == PrStatus_Ok);
        prGpuBufferUpload(f.device, buffer, at, corners, sizeof(corners));

        PrGpuTexture target = f.target(PrUVec2{4, 4});
        PrGpuPipeline sampled = f.pipeline("quadVertex", "slotSampled");
        PrGpuPipeline pulled = f.pipeline("slotPulled", "solid", PrGpuFormat_Rgba8, PrGpuTopology_TriangleList);
        PrGpuTexture half = f.target(PrUVec2{4, 4});
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        // Bound before the pass begins, so that its encoder takes them, and the buffer once the pass is open.
        prGpuCommandsSetTexture(commands, 2, source, nearest);
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        prGpuCommandsTextureBarrier(commands, half, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc passDesc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{});
        PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
        Push push = fill(0, 0, 0, 0);
        prGpuCommandsSetPipeline(commands, sampled);
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        passDesc = prGpuPassDescColor(half, PrGpuLoad_Clear, PrColor{});
        pass = prGpuPassBegin(commands, &passDesc);
        Push color = fill(0, 1, 0, 1);
        prGpuCommandsSetPipeline(commands, pulled);
        prGpuCommandsSetPush(commands, &color, sizeof(color));
        prGpuCommandsSetBuffer(commands, 1, buffer, at);
        prGpuPassDraw(pass, 6, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        prGpuCommandsTextureBarrier(commands, half, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        gpuSubmitAndWait(commands);

        Array<uint8_t> out = f.read(target);
        CHECK(near(texel(out, 4, 0, 0), 255, 0, 0, 255));
        CHECK(near(texel(out, 4, 3, 0), 0, 255, 0, 255));
        CHECK(near(texel(out, 4, 0, 3), 0, 0, 255, 255));
        CHECK(near(texel(out, 4, 3, 3), 255, 255, 255, 255));
        out = f.read(half);
        CHECK(near(texel(out, 4, 0, 0), 0, 255, 0, 255));
        CHECK(near(texel(out, 4, 1, 3), 0, 255, 0, 255));
        CHECK(near(texel(out, 4, 2, 0), 0, 0, 0, 0));
        prGpuPipelineRelease(f.device, pulled);
        prGpuPipelineRelease(f.device, sampled);
        prGpuBufferRelease(f.device, buffer);
        prGpuTextureRelease(f.device, half);
        prGpuTextureRelease(f.device, target);
        prGpuTextureRelease(f.device, source);
    });
}

TEST_CASE("a dispatch reads a Buffer slot and writes RwBuffer and RwTexture slots") {
    onEachBackend([](Fixture& f) {
        PrGpuPipeline doubled{};
        REQUIRE(prGpuPipelineCreateCompute(f.device, f.library, "slotDoubled", "slotDoubled", &doubled) == PrStatus_Ok);
        PrGpuPipeline filled{};
        REQUIRE(prGpuPipelineCreateCompute(f.device, f.library, "slotFilled", "slotFilled", &filled) == PrStatus_Ok);
        const uint32_t count = 256;
        Array<uint32_t> input(count);
        for (uint32_t i = 0; i < count; ++i)
            input[i] = i;
        PrGpuBufferDesc desc{.size = count * 4, .usage = PrGpuBufferUsageFlag_Storage};
        PrGpuBuffer in{};
        PrGpuBuffer out{};
        REQUIRE(prGpuBufferCreate(f.device, &desc, &in) == PrStatus_Ok);
        REQUIRE(prGpuBufferCreate(f.device, &desc, &out) == PrStatus_Ok);
        prGpuBufferUpload(f.device, in, 0, input.data(), count * 4);
        PrGpuTextureDesc textureDesc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{8, 8},
            PrGpuTextureUsageFlag_Storage | PrGpuTextureUsageFlag_Sampled);
        PrGpuTexture written{};
        REQUIRE(prGpuTextureCreate(f.device, &textureDesc, &written) == PrStatus_Ok);

        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsSetBuffer(commands, 3, in, 0);
        prGpuCommandsSetRwBuffer(commands, 1, out, 0);
        prGpuCommandsSetPipeline(commands, doubled);
        prGpuCommandsDispatch(commands, PrGpuExtent{count / 64, 1, 1});
        prGpuCommandsTextureBarrier(commands, written, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ComputeWrite);
        Push push = fill(0, 0, 1, 1);
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuCommandsSetRwTexture(commands, 2, written);
        prGpuCommandsSetPipeline(commands, filled);
        prGpuCommandsDispatch(commands, PrGpuExtent{1, 1, 1});
        prGpuCommandsBufferBarrier(commands, out, PrGpuAccessFlag_ComputeWrite, PrGpuAccessFlag_ShaderRead);
        prGpuCommandsTextureBarrier(commands, written, PrGpuSubresources{}, PrGpuAccessFlag_ComputeWrite,
            PrGpuAccessFlag_ShaderRead);
        gpuSubmitAndWait(commands);

        Array<uint8_t> bytes = f.read(out, count * 4);
        const uint32_t* words = reinterpret_cast<const uint32_t*>(bytes.data());
        CHECK(words[0] == 0);
        CHECK(words[1] == 2);
        CHECK(words[255] == 510);
        Array<uint8_t> texels = f.read(written);
        CHECK(near(texel(texels, 8, 0, 0), 0, 0, 255, 255));
        CHECK(near(texel(texels, 8, 7, 7), 0, 0, 255, 255));
        prGpuTextureRelease(f.device, written);
        prGpuBufferRelease(f.device, in);
        prGpuBufferRelease(f.device, out);
        prGpuPipelineRelease(f.device, filled);
        prGpuPipelineRelease(f.device, doubled);
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
        if ((prGpuDeviceInfo(f.device).features & PrGpuFeatureFlag_TargetReads) == 0) {
            CHECK(prGpuPipelineCreateGraphics(f.device, &desc, &inverted) == PrStatus_NotSupported);
            prGpuPipelineRelease(f.device, solid);
            prGpuTextureRelease(f.device, target);
            return;
        }
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
        gpuSubmitAndWait(commands);
        Array<uint8_t> texels = f.read(target);
        CHECK(near(texel(texels, 4, 0, 0), 0, 255, 255, 255));
        CHECK(near(texel(texels, 4, 3, 3), 255, 255, 255, 255));
        prGpuPipelineRelease(f.device, inverted);
        prGpuPipelineRelease(f.device, solid);
        prGpuTextureRelease(f.device, target);
    });
}

TEST_CASE("passes that keep a color attachment are in order on it without a barrier") {
    onEachBackend([](Fixture& f) {
        constexpr PrUVec2 kExtent{kOrderedPasses * 4, 64};
        PrGpuTexture target = f.target(kExtent);
        PrGpuPipeline solid = f.pipeline("quadVertex", "solid");

        // Each pass keeps what the passes before it drew and draws its own column, in a value of its own.
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        for (uint32_t i = 0; i < kOrderedPasses; ++i) {
            PrGpuPassDesc desc = prGpuPassDescColor(target, i == 0 ? PrGpuLoad_Clear : PrGpuLoad_Keep, PrColor{0, 0, 0, 1});
            PrGpuPass* pass = prGpuPassBegin(commands, &desc);
            Push push = orderedColumn(i, float(i + 1) / 255, 1, 0);
            prGpuCommandsSetPipeline(commands, solid);
            prGpuCommandsSetPush(commands, &push, sizeof(push));
            prGpuPassDraw(pass, 4, 1, 0, 0);
            prGpuPassEnd(pass);
        }
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        gpuSubmitAndWait(commands);

        Array<uint8_t> texels = f.read(target);
        uint32_t wrong = 0;
        for (uint32_t y = 0; y < kExtent.y; ++y) {
            for (uint32_t x = 0; x < kExtent.x; ++x)
                wrong += near(texel(texels, kExtent.x, x, y), uint8_t(x / 4 + 1), 255, 0, 255, 0) ? 0 : 1;
        }
        CHECK(wrong == 0);
        prGpuPipelineRelease(f.device, solid);
        prGpuTextureRelease(f.device, target);
    });
}

TEST_CASE("passes of depth alone that keep it are in order on it without a barrier") {
    onEachBackend([](Fixture& f) {
        constexpr PrUVec2 kExtent{kOrderedPasses * 4, 64};
        PrGpuTextureDesc depthDesc = prGpuTextureDesc2d(PrGpuFormat_D32F, kExtent, PrGpuTextureUsageFlag_DepthStencil);
        PrGpuTexture depth{};
        REQUIRE(prGpuTextureCreate(f.device, &depthDesc, &depth) == PrStatus_Ok);
        PrGpuTexture target = f.target(kExtent);
        // Every draw is at depth 0. The passes of depth alone write it, and the last pass draws a color where the depth
        // that they left equals it.
        PrGpuGraphicsPipelineDesc writesDesc = f.description("quadVertex", "solid");
        writesDesc.fragment = nullptr;
        writesDesc.formats.colorCount = 0;
        writesDesc.formats.color[0] = PrGpuFormat_None;
        writesDesc.formats.depthStencil = PrGpuFormat_D32F;
        writesDesc.depthTest = true;
        writesDesc.depthWrite = true;
        writesDesc.depthCompare = PrGpuCompare_Always;
        PrGpuPipeline writes{};
        REQUIRE(prGpuPipelineCreateGraphics(f.device, &writesDesc, &writes) == PrStatus_Ok);
        PrGpuGraphicsPipelineDesc testedDesc = f.description("quadVertex", "solid");
        testedDesc.formats.depthStencil = PrGpuFormat_D32F;
        testedDesc.depthTest = true;
        testedDesc.depthCompare = PrGpuCompare_Equal;
        PrGpuPipeline tested{};
        REQUIRE(prGpuPipelineCreateGraphics(f.device, &testedDesc, &tested) == PrStatus_Ok);

        // Each pass keeps the depth that the passes before it wrote and writes its own column.
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsTextureBarrier(commands, depth, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_DepthStencilWrite);
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        for (uint32_t i = 0; i < kOrderedPasses; ++i) {
            PrGpuPassDesc desc{
                .viewCount = 1,
                .depthStencil = {.texture = depth, .load = PrGpuLoad(i == 0 ? PrGpuLoad_Clear : PrGpuLoad_Keep)},
                .clearDepth = 1,
            };
            PrGpuPass* pass = prGpuPassBegin(commands, &desc);
            Push push = orderedColumn(i, 0, 0, 0);
            prGpuCommandsSetPipeline(commands, writes);
            prGpuCommandsSetPush(commands, &push, sizeof(push));
            prGpuPassDraw(pass, 4, 1, 0, 0);
            prGpuPassEnd(pass);
        }
        PrGpuPassDesc desc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{0, 0, 0, 1});
        desc.depthStencil = PrGpuAttachment{.texture = depth, .load = PrGpuLoad_Keep};
        PrGpuPass* pass = prGpuPassBegin(commands, &desc);
        Push push = fill(1, 1, 1, 1);
        prGpuCommandsSetPipeline(commands, tested);
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        gpuSubmitAndWait(commands);

        Array<uint8_t> texels = f.read(target);
        uint32_t wrong = 0;
        for (uint32_t i = 0; i < kExtent.x * kExtent.y; ++i)
            wrong += near(&texels[i * 4], 255, 255, 255, 255) ? 0 : 1;
        CHECK(wrong == 0);
        prGpuPipelineRelease(f.device, tested);
        prGpuPipelineRelease(f.device, writes);
        prGpuTextureRelease(f.device, target);
        prGpuTextureRelease(f.device, depth);
    });
}

TEST_CASE("a pass of several views draws each draw, direct or indirect, into each view's layer") {
    onEachBackend([](Fixture& f) {
        uint8_t maxViews = prGpuDeviceInfo(f.device).maxViews;
        bool indirect = (prGpuDeviceInfo(f.device).features & PrGpuFeatureFlag_Indirect) != 0;
        CHECK(maxViews >= 1);
        PrGpuGraphicsPipelineDesc desc = f.description("viewQuad", "viewColor");
        desc.formats.viewCount = uint8_t(maxViews + 1);
        PrGpuPipeline refused{};
        CHECK(prGpuPipelineCreateGraphics(f.device, &desc, &refused) == PrStatus_NotSupported);
        // A shader of views draws view zero in a pass of one view.
        PrGpuTexture flat = f.target(PrUVec2{4, 4});
        PrGpuPipeline single = f.pipeline("viewQuad", "viewColor");
        f.draw(flat, single, fill(1, 0.5f, 0, 1));
        CHECK(near(texel(f.read(flat), 4, 1, 1), 0, 128, 0, 255));
        prGpuPipelineRelease(f.device, single);
        prGpuTextureRelease(f.device, flat);
        PrGpuDrawIndirect arguments{.vertexCount = 4, .instanceCount = 1};
        PrGpuBufferDesc argumentsDesc{.size = sizeof(arguments), .usage = PrGpuBufferUsageFlag_Indirect};
        PrGpuBuffer argumentsBuffer{};
        REQUIRE(prGpuBufferCreate(f.device, &argumentsDesc, &argumentsBuffer) == PrStatus_Ok);
        prGpuBufferUpload(f.device, argumentsBuffer, 0, &arguments, sizeof(arguments));
        for (uint8_t views = 2; views <= maxViews && views <= 3; ++views) {
            CAPTURE(int(views));
            PrGpuTextureDesc layered = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{4, 4},
                PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
            layered.kind = PrGpuTextureKind_2dArray;
            layered.extent.depth = views;
            PrGpuTexture target{};
            REQUIRE(prGpuTextureCreate(f.device, &layered, &target) == PrStatus_Ok);
            desc.formats.viewCount = views;
            PrGpuPipeline pipeline{};
            REQUIRE(prGpuPipelineCreateGraphics(f.device, &desc, &pipeline) == PrStatus_Ok);
            PrGpuCommands* commands = nullptr;
            REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
            PrGpuSubresources all{};
            prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ShaderRead,
                PrGpuAccessFlag_ColorAttachment);
            prGpuCommandsBufferBarrier(commands, argumentsBuffer, PrGpuAccessFlag_ShaderRead,
                PrGpuAccessFlag_IndirectRead);
            PrGpuPassDesc passDesc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{0, 0, 1, 1});
            passDesc.viewCount = views;
            PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
            CHECK(prGpuPassFormats(pass).viewCount == views);
            prGpuCommandsSetPipeline(commands, pipeline);
            // The top half directly, and the bottom half's left indirectly where the device can.
            Push top = fill(0, 1, 0, 1);
            top.rect[3] = 0;
            prGpuCommandsSetPush(commands, &top, sizeof(top));
            prGpuPassDraw(pass, 4, 1, 0, 0);
            Push bottom = fill(0, 0.5f, 0, 1);
            bottom.rect[1] = 0;
            bottom.rect[2] = 0;
            prGpuCommandsSetPush(commands, &bottom, sizeof(bottom));
            if (indirect)
                prGpuPassDrawIndirect(pass, argumentsBuffer, 0, 1, sizeof(arguments));
            else
                prGpuPassDraw(pass, 4, 1, 0, 0);
            prGpuPassEnd(pass);
            prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment,
                PrGpuAccessFlag_ShaderRead);
            gpuSubmitAndWait(commands);
            for (uint16_t layer = 0; layer < views; ++layer) {
                CAPTURE(layer);
                PrGpuTexture view{};
                REQUIRE(prGpuTextureCreateView(f.device, target, PrGpuTextureViewDesc{
                    .kind = PrGpuTextureKind_2d,
                    .subresources = PrGpuSubresources{.levels = 1, .firstLayer = layer, .layers = 1},
                }, &view) == PrStatus_Ok);
                Array<uint8_t> texels = f.read(view);
                uint8_t red = uint8_t(layer * 85);
                CHECK(near(texel(texels, 4, 3, 0), red, 255, 0, 255));
                CHECK(near(texel(texels, 4, 0, 3), red, 128, 0, 255));
                CHECK(near(texel(texels, 4, 3, 3), 0, 0, 255, 255));
                prGpuTextureRelease(f.device, view);
            }
            prGpuPipelineRelease(f.device, pipeline);
            prGpuTextureRelease(f.device, target);
        }
        prGpuBufferRelease(f.device, argumentsBuffer);
    });
}

TEST_CASE("objects destroyed after their work is recorded live until it is done") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(PrUVec2{4, 4});
        PrGpuTexture source = f.target(PrUVec2{1, 1});
        const uint8_t green[] = {0, 255, 0, 255};
        prGpuTextureWrite(f.device, source, PrIVec2{0, 0}, PrPixelView{green, 4, {1, 1}});
        PrGpuPipeline sampled = f.pipeline("quadVertex", "slotSampled");
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
        prGpuCommandsSetTexture(commands, 2, source, sampler);
        prGpuCommandsSetPipeline(commands, sampled);
        prGpuCommandsSetPush(commands, &push, sizeof(push));
        prGpuPassDraw(pass, 4, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        // Destroyed before the submission, and new objects take no slot that the work still reads.
        prGpuTextureRelease(f.device, source);
        prGpuPipelineRelease(f.device, sampled);
        PrGpuTexture other = f.target(PrUVec2{1, 1});
        gpuSubmitAndWait(commands);
        CHECK(near(texel(f.read(target), 4, 2, 2), 0, 255, 0, 255));
        prGpuTextureRelease(f.device, other);
        prGpuTextureRelease(f.device, target);
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
        gpuSubmitAndWait(commands);
        PrGpuTexture last{};
        REQUIRE(prGpuTextureCreateView(f.device, texture, PrGpuTextureViewDesc{
            .kind = PrGpuTextureKind_2d,
            .subresources = PrGpuSubresources{.firstLevel = 2, .levels = 1, .layers = 1},
        }, &last) == PrStatus_Ok);
        CHECK(prGpuTextureExtent(f.device, last).x == 1);
        Array<uint8_t> top = f.read(last);
        CHECK(near(top.data(), 128, 128, 128, 128, 2));
        prGpuTextureRelease(f.device, last);
        prGpuTextureRelease(f.device, texture);
    });
}

TEST_CASE("a cube's faces and levels are uploaded, drawn to, viewed, copied and sampled along a direction") {
    onEachBackend([](Fixture& f) {
        PrGpuTextureDesc desc{
            .kind = PrGpuTextureKind_Cube,
            .format = PrGpuFormat_Rgba8,
            .usage = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget,
            .levels = 3,
            .samples = 1,
            .extent = {4, 4, 6},
        };
        PrGpuTexture cube{};
        REQUIRE(prGpuTextureCreate(f.device, &desc, &cube) == PrStatus_Ok);
        CHECK(prGpuTextureDesc(f.device, cube).kind == PrGpuTextureKind_Cube);
        CHECK(prGpuTextureDesc(f.device, cube).extent.depth == 6);

        // The first level takes one face at a time, each of its face's color. The first texel of each face is white,
        // which tells where the face's rows and columns lie.
        for (uint16_t face = 0; face < 6; ++face) {
            uint8_t texels[4 * 4 * 4];
            for (uint32_t i = 0; i < 16; ++i)
                prMemCopy(texels + i * 4, kFaceColors[face], 4);
            prMemFill(texels, 255, 4);
            PrGpuTextureRegion region{.firstLayer = face, .extent = {4, 4, 1}};
            prGpuTextureUpload(f.device, cube, &region, texels, 16, 64);
        }
        // The second level takes all six faces in one upload, each half as bright as in the first.
        uint8_t second[6 * 2 * 2 * 4];
        for (uint32_t face = 0; face < 6; ++face) {
            for (uint32_t i = 0; i < 4; ++i) {
                for (uint32_t channel = 0; channel < 4; ++channel)
                    second[(face * 4 + i) * 4 + channel] = kFaceColors[face][channel] / 2;
            }
        }
        PrGpuTextureRegion all{.level = 1, .extent = {2, 2, 6}};
        prGpuTextureUpload(f.device, cube, &all, second, 8, 16);

        // The third level is drawn: a pass clears one face of it at a time.
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        PrGpuSubresources third{.firstLevel = 2, .levels = 1};
        prGpuCommandsTextureBarrier(commands, cube, third, PrGpuAccessFlag_ShaderRead | PrGpuAccessFlag_Discard,
            PrGpuAccessFlag_ColorAttachment);
        for (uint16_t face = 0; face < 6; ++face) {
            const uint8_t* color = kFaceColors[face];
            PrColor clear{float(color[0]) / 1020, float(color[1]) / 1020, float(color[2]) / 1020, 1};
            PrGpuPassDesc passDesc = prGpuPassDescColor(cube, PrGpuLoad_Clear, clear);
            passDesc.color[0].level = 2;
            passDesc.color[0].layer = face;
            PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
            CHECK(prGpuPassExtent(pass) == PrUVec2{1, 1});
            prGpuPassEnd(pass);
        }
        prGpuCommandsTextureBarrier(commands, cube, third, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
        gpuSubmitAndWait(commands);

        // A direction along a face's axis samples that face, at each level.
        for (uint32_t face = 0; face < 6; ++face) {
            CAPTURE(face);
            const uint8_t* color = kFaceColors[face];
            // The first texel lies three quarters of the way from the face's center against its columns and its rows,
            // and the last as far along them.
            float firstTexel[3];
            float lastTexel[3];
            for (uint32_t axis = 0; axis < 3; ++axis) {
                float along = 0.75f * (kFaceColumns[face][axis] + kFaceRows[face][axis]);
                firstTexel[axis] = kFaceAxes[face][axis] - along;
                lastTexel[axis] = kFaceAxes[face][axis] + along;
            }
            CHECK(near(sampleCube(f, cube, firstTexel, 0).data(), 255, 255, 255, 255));
            CHECK(near(sampleCube(f, cube, lastTexel, 0).data(), color[0], color[1], color[2], 255));
            Array<uint8_t> half = sampleCube(f, cube, kFaceAxes[face], 1);
            CHECK(near(half.data(), color[0] / 2, color[1] / 2, color[2] / 2, 127));
            Array<uint8_t> quarter = sampleCube(f, cube, kFaceAxes[face], 2);
            CHECK(near(quarter.data(), color[0] / 4, color[1] / 4, color[2] / 4, 255, 2));
        }

        // A view of one level of one face is a 2D texture, and a view of one level of every face is a cube.
        PrGpuTexture face{};
        REQUIRE(prGpuTextureCreateView(f.device, cube, PrGpuTextureViewDesc{
            .kind = PrGpuTextureKind_2d,
            .subresources = PrGpuSubresources{.firstLevel = 1, .levels = 1, .firstLayer = 3, .layers = 1},
        }, &face) == PrStatus_Ok);
        CHECK(prGpuTextureDesc(f.device, face).kind == PrGpuTextureKind_2d);
        CHECK(prGpuTextureExtent(f.device, face) == PrUVec2{2, 2});
        Array<uint8_t> viewed = f.read(face);
        REQUIRE(viewed.count() == 2 * 2 * 4);
        CHECK(near(texel(viewed, 2, 1, 1), 127, 127, 0, 127));
        PrGpuTexture level{};
        REQUIRE(prGpuTextureCreateView(f.device, cube, PrGpuTextureViewDesc{
            .kind = PrGpuTextureKind_Cube,
            .subresources = PrGpuSubresources{.firstLevel = 1, .levels = 1},
        }, &level) == PrStatus_Ok);
        CHECK(prGpuTextureDesc(f.device, level).kind == PrGpuTextureKind_Cube);
        CHECK(prGpuTextureDesc(f.device, level).levels == 1);
        CHECK(near(sampleCube(f, level, kFaceAxes[1], 0).data(), 0, 127, 0, 127));

        // A copy reads one level of a range of faces into a buffer.
        PrGpuBufferDesc bufferDesc{.size = 2 * 2 * 2 * 4, .memory = PrGpuMemory_Readback};
        PrGpuBuffer readback{};
        REQUIRE(prGpuBufferCreate(f.device, &bufferDesc, &readback) == PrStatus_Ok);
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        PrGpuSubresources secondLevel{.firstLevel = 1, .levels = 1};
        prGpuCommandsTextureBarrier(commands, cube, secondLevel, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_CopyRead);
        PrGpuTextureRegion faces{.level = 1, .firstLayer = 4, .extent = {2, 2, 2}};
        prGpuCommandsCopyTextureToBuffer(commands, cube, &faces, readback, 0, 8, 16);
        prGpuCommandsTextureBarrier(commands, cube, secondLevel, PrGpuAccessFlag_CopyRead, PrGpuAccessFlag_ShaderRead);
        gpuSubmitAndWait(commands);
        const uint8_t* copied = static_cast<const uint8_t*>(prGpuBufferMap(f.device, readback));
        CHECK(near(copied, 0, 127, 127, 127));
        CHECK(near(copied + 16, 127, 0, 127, 127));

        prGpuBufferRelease(f.device, readback);
        prGpuTextureRelease(f.device, level);
        prGpuTextureRelease(f.device, face);
        prGpuTextureRelease(f.device, cube);
    });
}

TEST_CASE("a command buffer reports its time and the texels that it read when it is done") {
    onEachBackend([](Fixture& f) {
        PrGpuTexture target = f.target(PrUVec2{3, 2});
        PrGpuPipeline solid = f.pipeline("quadVertex", "solid");
        f.draw(target, solid, fill(0, 0, 1, 1));
        struct Read {
            Array<uint8_t> texels;
            double seconds = -1;
        } read;
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        prGpuCommandsReadTexture(commands, target, PrGpuAccessFlag_ShaderRead, [](void* context, PrPixelView texels) {
            Array<uint8_t>& into = static_cast<Read*>(context)->texels;
            for (uint32_t y = 0; y < texels.extent.y; ++y)
                into.append(texels.data + y * texels.stride, texels.extent.x * 4);
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
        REQUIRE(read.texels.count() == 3 * 2 * 4);
        CHECK(near(texel(read.texels, 3, 2, 1), 0, 0, 255, 255));
        prGpuPipelineRelease(f.device, solid);
        prGpuTextureRelease(f.device, target);
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
        prGpuTextureRelease(f.device, texture);
    });
}
