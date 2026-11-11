// The tests of the memory backend's device, which the Canvas 2D backend shares: its buffers and textures, its copies
// and clears, the order that its work runs in, and how the application reaches its memory. The interface's own tests
// draw with shaders, so they skip this device.

#include "pr/array.h"
#include "pr/float16.h"
#include "pr/gpu.h"
#include "pr/gpu_memory.h"
#include "pr/mem.h"
#include "pr/surface.h"
#include "pr/thread.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

struct Device {
    PrGpuDevice* device = nullptr;

    Device() { REQUIRE(prGpuMemoryCreate(&device) == PrStatus_Ok); }
    ~Device() { prGpuDeviceDestroy(device); }

    PrGpuTexture texture(PrUVec2 extent, PrGpuFormat format = PrGpuFormat_Rgba8) {
        PrGpuTextureDesc desc = prGpuTextureDesc2d(format, extent, PrGpuTextureUsageFlag_RenderTarget);
        PrGpuTexture texture{};
        REQUIRE(prGpuTextureCreate(device, &desc, &texture) == PrStatus_Ok);
        return texture;
    }

    Array<uint8_t> read(PrGpuTexture texture, uint32_t texelBytes = 4) {
        PrUVec2 extent = prGpuTextureExtent(device, texture);
        Array<uint8_t> texels(extent.x * extent.y * texelBytes);
        prGpuTextureRead(device, texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0},
            PrPixels{texels.data(), size_t(extent.x) * texelBytes, extent});
        return texels;
    }

    // Clears the texture, or one level and layer of it, to `color` in a pass.
    void clear(PrGpuTexture texture, PrColor color, uint8_t level = 0, uint16_t layer = 0) {
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        PrGpuPassDesc desc = prGpuPassDescColor(texture, PrGpuLoad_Clear, color);
        desc.color[0].level = level;
        desc.color[0].layer = layer;
        prGpuPassEnd(prGpuPassBegin(commands, &desc));
        prGpuDeviceWait(device, prGpuCommandsSubmit(commands));
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a memory device holds textures in memory and runs no shaders") {
    Device d;
    PrGpuDeviceInfo info = prGpuDeviceInfo(d.device);
    CHECK(info.backend == "memory");
    CHECK(prGpuDeviceHasNative(d.device, PrGpuNativeKind_Pixels));
    CHECK(!prGpuDeviceHasNative(d.device, PrGpuNativeKind_Canvas2d));
    CHECK(info.maxTextureSize >= 16384);
    CHECK(prGpuDeviceSupportsFormat(d.device, PrGpuFormat_Bgra8, PrGpuTextureUsageFlag_RenderTarget));
    CHECK(prGpuDeviceSupportsFormat(d.device, PrGpuFormat_Rgba16F, PrGpuTextureUsageFlag_Sampled));
    CHECK_FALSE(prGpuDeviceSupportsFormat(d.device, PrGpuFormat_D32F, PrGpuTextureUsageFlag_DepthStencil));
    CHECK_FALSE(prGpuDeviceSupportsFormat(d.device, PrGpuFormat_Rgba8, PrGpuTextureUsageFlag_Storage));
    CHECK_FALSE(prGpuDeviceSupportsFormat(d.device, PrGpuFormat_Bc7, PrGpuTextureUsageFlag_Sampled));
    // A library of any shaders has no code that the device runs.
    const uint8_t library[16] = {'P', 'R', 'S', 'L', 2};
    PrGpuShaderLibrary* shaders = nullptr;
    CHECK(prGpuShaderLibraryCreate(d.device, library, sizeof(library), &shaders) == PrStatus_NotSupported);
}

TEST_CASE("a memory texture's texels are written and read back, clipped to the texture") {
    Device d;
    PrGpuTexture texture = d.texture(PrUVec2{3, 2});
    Array<uint8_t> texels(3 * 2 * 4);
    for (uint32_t i = 0; i < texels.count(); ++i)
        texels[i] = uint8_t(i + 1);
    prGpuTextureWrite(d.device, texture, PrIVec2{0, 0}, PrPixelView{texels.data(), 12, {3, 2}});
    CHECK(d.read(texture) == texels);
    uint8_t white[4] = {255, 255, 255, 255};
    prGpuTextureWrite(d.device, texture, PrIVec2{2, 1}, PrPixelView{white, 4, {1, 1}});
    Array<uint8_t> after = d.read(texture);
    CHECK(prMemEquals(&after[20], white, 4));
    CHECK(after[16] == 17);
    prGpuTextureDestroy(d.device, texture);
}

TEST_CASE("a memory pass clears its target to the color as the target's format holds it") {
    Device d;
    PrColor color{1, 0.5f, 0, 1};
    PrGpuTexture rgba = d.texture(PrUVec2{2, 2});
    d.clear(rgba, color);
    Array<uint8_t> texels = d.read(rgba);
    CHECK(texels[0] == 255);
    CHECK(texels[1] == 128);
    CHECK(texels[2] == 0);
    CHECK(texels[15] == 255);
    // Bgra8 swaps red and blue, and a Srgb format holds sRGB's encoding of the linear color.
    PrGpuTexture bgra = d.texture(PrUVec2{1, 1}, PrGpuFormat_Bgra8Srgb);
    d.clear(bgra, color);
    Array<uint8_t> encoded = d.read(bgra);
    CHECK(encoded[0] == 0);
    CHECK(encoded[1] == 188);
    CHECK(encoded[2] == 255);
    PrGpuTexture half = d.texture(PrUVec2{1, 1}, PrGpuFormat_Rgba16F);
    d.clear(half, PrColor{2, -1, 0.25f, 1});
    Array<uint8_t> halves = d.read(half, 8);
    PrFloat16 red{};
    prMemCopy(&red, halves.data(), 2);
    CHECK(prFloat16ToFloat(red) == 2.0f);
    prGpuTextureDestroy(d.device, rgba);
    prGpuTextureDestroy(d.device, bgra);
    prGpuTextureDestroy(d.device, half);
}

TEST_CASE("a memory command buffer runs its work in order when it is submitted, after the uploads before it") {
    Device d;
    PrGpuBufferDesc desc{.size = 16, .memory = PrGpuMemory_Readback};
    PrGpuBuffer source{};
    PrGpuBuffer target{};
    REQUIRE(prGpuBufferCreate(d.device, &desc, &source) == PrStatus_Ok);
    REQUIRE(prGpuBufferCreate(d.device, &desc, &target) == PrStatus_Ok);
    const uint8_t* bytes = static_cast<const uint8_t*>(prGpuBufferMap(d.device, target));
    CHECK(bytes[0] == 0);
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(d.device, nullptr, &commands));
    prGpuCommandsCopyBuffer(commands, source, 0, target, 0, 16);
    bool done = false;
    prGpuCommandsOnDone(commands, [](void* context, double) { *static_cast<bool*>(context) = true; }, &done);
    // An upload made after the copy is recorded, and before it is submitted, is what the copy reads.
    const uint8_t sevens[16] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
    prGpuBufferUpload(d.device, source, 0, sevens, sizeof(sevens));
    CHECK(bytes[0] == 0);
    CHECK_FALSE(done);
    PrGpuFence fence = prGpuCommandsSubmit(commands);
    CHECK(done);
    CHECK(prGpuDeviceIsComplete(d.device, fence));
    CHECK(prMemEquals(bytes, sevens, 16));
    prGpuBufferDestroy(d.device, source);
    prGpuBufferDestroy(d.device, target);
}

TEST_CASE("a memory view covers its level and layer of the texture's memory") {
    Device d;
    PrGpuTextureDesc desc{
        .kind = PrGpuTextureKind_2dArray,
        .format = PrGpuFormat_Rgba8,
        .usage = PrGpuTextureUsageFlag_RenderTarget,
        .levels = 2,
        .samples = 1,
        .extent = {4, 4, 2},
    };
    PrGpuTexture array{};
    REQUIRE(prGpuTextureCreate(d.device, &desc, &array) == PrStatus_Ok);
    PrGpuTexture view{};
    REQUIRE(prGpuTextureCreateView(d.device, array, PrGpuTextureKind_2d,
        PrGpuSubresources{.firstLevel = 1, .levels = 1, .firstLayer = 1, .layers = 1}, &view) == PrStatus_Ok);
    PrGpuTextureDesc viewed = prGpuTextureDesc(d.device, view);
    CHECK(viewed.extent.width == 2);
    CHECK(viewed.extent.depth == 1);
    d.clear(view, PrColor{0, 0, 1, 1});
    // The view's texels are level 1 of layer 1, which a copy from the array reads.
    PrGpuBufferDesc readback{.size = 16, .memory = PrGpuMemory_Readback};
    PrGpuBuffer buffer{};
    REQUIRE(prGpuBufferCreate(d.device, &readback, &buffer) == PrStatus_Ok);
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(d.device, nullptr, &commands));
    PrGpuTextureRegion region{.level = 1, .firstLayer = 1, .extent = {2, 2, 1}};
    prGpuCommandsCopyTextureToBuffer(commands, array, &region, buffer, 0, 8, 0);
    prGpuDeviceWait(d.device, prGpuCommandsSubmit(commands));
    const uint8_t* texels = static_cast<const uint8_t*>(prGpuBufferMap(d.device, buffer));
    CHECK(texels[2] == 255);
    CHECK(texels[12 + 2] == 255);
    prGpuTextureDestroy(d.device, view);
    prGpuTextureDestroy(d.device, array);
    prGpuBufferDestroy(d.device, buffer);
}

TEST_CASE("a memory pass draws through the application, in order with its clear, into the texels of its attachment") {
    Device d;
    PrGpuTexture texture = d.texture(PrUVec2{2, 2});
    struct Drawn {
        PrUVec2 extent;
        PrGpuFormat format;
        uint8_t cleared;
    } drawn{};
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(d.device, nullptr, &commands));
    PrGpuPassDesc desc = prGpuPassDescColor(texture, PrGpuLoad_Clear, PrColor{1, 0, 0, 1});
    PrGpuPass* pass = prGpuPassBegin(commands, &desc);
    prGpuPassNativeDraw(pass, PrGpuNativeKind_Pixels, [](void* context, const void* native) {
        const PrGpuNativePixels& target = *static_cast<const PrGpuNativePixels*>(native);
        Drawn& out = *static_cast<Drawn*>(context);
        out.extent = target.pixels.extent;
        out.format = target.format;
        out.cleared = target.pixels.data[0];
        target.pixels.data[target.pixels.stride + 4 + 1] = 255;
    }, &drawn);
    prGpuPassEnd(pass);
    CHECK(drawn.extent.x == 0);
    prGpuDeviceWait(d.device, prGpuCommandsSubmit(commands));
    CHECK(drawn.extent.x == 2);
    CHECK(drawn.extent.y == 2);
    CHECK(drawn.format == PrGpuFormat_Rgba8);
    CHECK(drawn.cleared == 255);
    Array<uint8_t> texels = d.read(texture);
    CHECK(texels[12 + 1] == 255);
    prGpuTextureDestroy(d.device, texture);
}

TEST_CASE("a memory texture over the application's pixels draws and reads them in place, with their stride") {
    Device d;
    Array<uint8_t> pixels(3 * 16);
    prMemFill(pixels.data(), 7, pixels.count());
    PrGpuTexture texture{};
    REQUIRE(prGpuTextureCreateOverPixels(d.device, PrGpuFormat_Rgba8, PrPixels{pixels.data(), 16, {3, 3}},
        PrGpuTextureUsageFlag_RenderTarget, &texture) == PrStatus_Ok);
    PrGpuNativePixels native{};
    REQUIRE(prGpuTextureNative(d.device, texture, PrGpuNativeKind_Pixels, &native));
    PrPixels held = native.pixels;
    CHECK(held.data == pixels.data());
    CHECK(held.stride == 16);
    d.clear(texture, PrColor{0, 0, 1, 1});
    CHECK(pixels[2] == 255);
    CHECK(pixels[16 * 2 + 8 + 2] == 255);
    // The four bytes past each row are the application's, which the texture leaves alone.
    CHECK(pixels[12] == 7);
    PrGpuTexture unmatched{};
    CHECK(prGpuTextureCreateOverPixels(d.device, PrGpuFormat_Rg8, PrPixels{pixels.data(), 16, {3, 3}},
        PrGpuTextureUsageFlag_RenderTarget, &unmatched) == PrStatus_NotSupported);
    prGpuTextureDestroy(d.device, texture);
}

TEST_CASE("a memory device presents to no surface") {
    Device d;
    PrSurface window{.kind = PrSurfaceKind_Hwnd};
    PrGpuSwapChainDesc desc{};
    PrGpuSwapChain* swapChain = nullptr;
    CHECK(prGpuSwapChainCreate(d.device, &window, &desc, &swapChain) == PrStatus_NotSupported);
}

TEST_CASE("a memory texture destroyed after work that uses it is recorded lasts until that work has run") {
    Device d;
    PrGpuTexture texture = d.texture(PrUVec2{2, 2});
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(d.device, nullptr, &commands));
    PrGpuPassDesc desc = prGpuPassDescColor(texture, PrGpuLoad_Clear, PrColor{1, 0, 0, 1});
    PrGpuPass* pass = prGpuPassBegin(commands, &desc);
    uint8_t drawn = 0;
    prGpuPassNativeDraw(pass, PrGpuNativeKind_Pixels, [](void* context, const void* native) {
        *static_cast<uint8_t*>(context) = static_cast<const PrGpuNativePixels*>(native)->pixels.data[0];
    }, &drawn);
    prGpuPassEnd(pass);
    // The texture's slot stays taken, so a texture made now does not reuse it.
    prGpuTextureDestroy(d.device, texture);
    PrGpuTexture made = d.texture(PrUVec2{2, 2});
    CHECK(made.id != texture.id);
    prGpuDeviceWait(d.device, prGpuCommandsSubmit(commands));
    CHECK(drawn == 255);
    prGpuTextureDestroy(d.device, made);
}

TEST_CASE("a memory device imports pixels by copying them, and is done with them at once") {
    Device d;
    CHECK(!prGpuDeviceImportsInPlace(d.device, PrGpuImportKind_Pixels));
    const uint8_t texels[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    PrGpuImportPixels pixels{.pixels = PrPixelView{texels, 8, PrUVec2{2, 1}}, .format = PrGpuFormat_Rgba8};
    PrGpuImport image{.kind = PrGpuImportKind_Pixels, .handle = &pixels};
    bool released = false;
    PrGpuTexture texture{};
    REQUIRE(prGpuTextureImport(d.device, &image, [](void* context) { *static_cast<bool*>(context) = true; }, &released,
        &texture) == PrStatus_Ok);
    CHECK(released);
    CHECK(prGpuTextureDesc(d.device, texture).usage == PrGpuTextureUsageFlag_Sampled);
    Array<uint8_t> read = d.read(texture);
    CHECK(prMemEquals(read.data(), texels, 8));
    prGpuTextureDestroy(d.device, texture);
}

TEST_CASE("a memory device runs work while another thread makes and destroys textures") {
    if (!prThreadSupported())
        return;
    Device d;
    PrGpuTexture source = d.texture(PrUVec2{4, 4});
    PrGpuTexture target = d.texture(PrUVec2{4, 4});
    d.clear(source, PrColor{1, 0, 0, 1});
    // The other thread grows the device's table of textures while this one copies between two of them.
    struct Maker {
        PrGpuDevice* device;
        bool failed;
    } maker{d.device, false};
    PrThread thread{};
    prThreadStart(&thread, [](void* context) {
        Maker& m = *static_cast<Maker*>(context);
        Array<PrGpuTexture> made;
        for (int i = 0; i < 2000; ++i) {
            PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{2, 2}, PrGpuTextureUsageFlag_Sampled);
            PrGpuTexture texture{};
            m.failed |= prGpuTextureCreate(m.device, &desc, &texture) != PrStatus_Ok;
            made.push(texture);
        }
        for (PrGpuTexture texture : made)
            prGpuTextureDestroy(m.device, texture);
    }, &maker);
    for (int i = 0; i < 2000; ++i) {
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(d.device, nullptr, &commands));
        PrGpuTextureRegion region{.extent = {4, 4, 1}};
        prGpuCommandsCopyTexture(commands, source, &region, target, &region);
        prGpuCommandsSubmit(commands);
    }
    prThreadJoin(&thread);
    CHECK(!maker.failed);
    CHECK(d.read(target)[0] == 255);
    prGpuTextureDestroy(d.device, source);
    prGpuTextureDestroy(d.device, target);
}

TEST_CASE("an import that copies writes into the texture that it replaces, if the texture fits the image") {
    Device d;
    uint8_t texels[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    PrGpuImportPixels pixels{.pixels = PrPixelView{texels, 8, PrUVec2{2, 1}}, .format = PrGpuFormat_Rgba8};
    PrGpuImport image{.kind = PrGpuImportKind_Pixels, .handle = &pixels};
    PrGpuTexture texture{};
    REQUIRE(prGpuTextureImport(d.device, &image, nullptr, nullptr, &texture) == PrStatus_Ok);
    PrGpuTexture first = texture;

    // The next frame of the same extent and format lands in the same texture.
    texels[0] = 9;
    REQUIRE(prGpuTextureImport(d.device, &image, nullptr, nullptr, &texture) == PrStatus_Ok);
    CHECK(texture.id == first.id);
    CHECK(d.read(texture)[0] == 9);

    // A frame of another extent replaces the texture.
    pixels.pixels = PrPixelView{texels, 4, PrUVec2{1, 2}};
    REQUIRE(prGpuTextureImport(d.device, &image, nullptr, nullptr, &texture) == PrStatus_Ok);
    CHECK(prGpuTextureExtent(d.device, texture) == PrUVec2{1, 2});

    // A failed import leaves the texture as it was, and never releases the image.
    PrGpuImport unknown{.kind = PrGpuImportKind_WebImage, .handle = &pixels};
    PrGpuTexture held = texture;
    bool released = false;
    CHECK(prGpuTextureImport(d.device, &unknown, [](void* context) { *static_cast<bool*>(context) = true; }, &released,
        &texture) == PrStatus_NotSupported);
    CHECK(texture.id == held.id);
    CHECK(!released);
    prGpuTextureDestroy(d.device, texture);
}
