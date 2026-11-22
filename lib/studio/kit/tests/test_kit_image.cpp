// Studio Kit's images.

#include "pr/array.h"
#include "pr/draw_cpu.h"
#include "pr/draw_list.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/kit_box.h"
#include "pr/kit_image.h"
#include "pr/layout_scene.h"
#include "pr/mem.h"
#include "pr/texture.h"

#include "tools/harness/harness_gpu.h"
#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// Returns an image of `width` by `height` opaque white pixels, which the caller releases.
const PrTextureSource* whiteImage(uint32_t width, uint32_t height) {
    Array<uint8_t> pixels(width * height * 4);
    prMemFill(pixels.data(), 255, pixels.count());
    const PrTextureSource* image =
        prTextureSourceCreateCopy(PrPixelView{pixels.data(), width * 4, {width, height}}, PrPixelFormat_Rgba8,
            PrTextureAlpha_Straight);
    return image;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an image takes its own size unless it is placed, and keeps its records from round to round") {
    KitFixture f;
    const PrTextureSource* image = whiteImage(64, 32);
    PrLayoutNode natural = kPrLayoutNodeNone;
    PrLayoutNode framed = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        // The row aligns its items at their start, so that the framed image's height does not stretch the other.
        PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_Start};
        PrLayoutNode row = prKitBox(f.kit, root, {}, line);
        natural = prKitImage(f.kit, row, {.image = image}, {});
        PrKitImageStyle cover{
            .fit = PrKitImageFit_Cover,
            .alignX = PrKitAlign_Start,
            .place = {.width = prKitPx(40), .height = prKitPx(40), .flex = PrKitFlex_Fixed},
        };
        framed = prKitImage(f.kit, row, {.image = image}, cover);
    };
    f.frame();
    PrRect own = f.bounds(natural);
    CHECK(prRectWidth(own) == 64.0f);
    CHECK(prRectHeight(own) == 32.0f);
    PrRect box = f.bounds(framed);
    CHECK(prRectWidth(box) == 40.0f);
    CHECK(prRectHeight(box) == 40.0f);
    CHECK(f.paintedCommands() >= 2);
    CHECK(prLayoutNodeContent(f.tree, framed).image == image);
    const PrLayoutScene* first = prLayoutTreeScene(f.tree);
    f.frame();
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesMade == 0);
    CHECK(stats.blocksLaidOut == 0);
    const PrLayoutScene* second = prLayoutTreeScene(f.tree);
    CHECK(second == first);
    prLayoutSceneRelease(first);
    prLayoutSceneRelease(second);
    prTextureSourceRelease(image);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a texture on the device draws fitted to its box and clipped by the parent box, and shows its texels as of "
        "each frame") {
    // A green texture of 4 by 4 texels on the device that the CPU renderer renders on.
    PrGpuDevice* device = nullptr;
    REQUIRE(gpuDeviceCreate(PrGpuBackend_Memory, nullptr, &device) == PrStatus_Ok);
    Array<uint8_t> texels(4 * 4 * 4);
    for (uint32_t i = 0; i < 16; ++i) {
        texels[i * 4 + 1] = 255;
        texels[i * 4 + 3] = 255;
    }
    PrPixels held{texels.data(), 16, {4, 4}};
    PrGpuTexture texture{};
    REQUIRE(prGpuTextureCreateOverPixels(device, PrGpuFormat_Rgba8, held, PrGpuTextureUsageFlag_Sampled, &texture) ==
        PrStatus_Ok);

    // A texture stretched over 200 by 100 px, in a box that clips it to 120 px.
    KitFixture f;
    uint64_t revision = 1;
    PrLayoutNode node = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitBoxStyle clip{
            .overflow = PrKitOverflow_Clip,
            .place = {.width = prKitPx(120), .height = prKitPx(100), .flex = PrKitFlex_Fixed},
        };
        PrLayoutNode box = prKitBox(f.kit, root, {}, clip);
        PrKitTexture content{
            .texture = PrTexture{.texture = texture, .alpha = PrTextureAlpha_None},
            .extent = {4, 4},
            .revision = revision,
        };
        PrKitImageStyle fill{
            .fit = PrKitImageFit_Fill,
            .place = {.width = prKitPx(200), .height = prKitPx(100), .flex = PrKitFlex_Fixed},
        };
        node = prKitTexture(f.kit, box, content, fill);
    };
    f.frame();
    CHECK(prRectWidth(f.bounds(node)) == 200.0f);

    PrDrawCpuOptions options = prDrawCpuOptionsDefault();
    options.device = device;
    PrDrawRenderer* renderer = prDrawCpuCreate(&options);
    REQUIRE(renderer != nullptr);
    auto render = [&](uint32_t x, uint32_t y) {
        PrDrawList* list = prDrawListCreate();
        prLayoutTreePaint(f.tree, list);
        Array<uint8_t> rgba(KitFixture::kRenderWidth * KitFixture::kRenderHeight * 4);
        PrPixels pixels{
            rgba.data(), size_t(KitFixture::kRenderWidth) * 4,
            {KitFixture::kRenderWidth, KitFixture::kRenderHeight},
        };
        REQUIRE(prDrawRendererRenderPixels(renderer, list, pixels, PrPixelFormat_Rgba8) == PrStatus_Ok);
        prDrawListRelease(list);
        const uint8_t* at = &rgba[(y * KitFixture::kRenderWidth + x) * 4];
        return PrColor{float(at[0]) / 255, float(at[1]) / 255, float(at[2]) / 255, float(at[3]) / 255};
    };
    PrRect box = f.bounds(node);
    PrColor inside = render(uint32_t(box.left + 60), uint32_t(box.top + 50));
    CHECK(inside.g == doctest::Approx(1.0));
    CHECK(inside.r == doctest::Approx(0.0));
    // Past the holder's 120 px, the texture is clipped away.
    PrColor clipped = render(uint32_t(box.left + 160), uint32_t(box.top + 50));
    CHECK(clipped.g < 0.2f);

    // The application writes red into the texture. The next render shows the red texels through the same list.
    for (uint32_t i = 0; i < 16; ++i) {
        texels[i * 4] = 255;
        texels[i * 4 + 1] = 0;
    }
    revision = 2;
    f.frame();
    PrColor red = render(uint32_t(box.left + 60), uint32_t(box.top + 50));
    CHECK(red.r == doctest::Approx(1.0));
    CHECK(red.g == doctest::Approx(0.0));
    CHECK(kitVerified(f.tree));
    prDrawRendererDestroy(renderer);
    prGpuTextureRelease(device, texture);
    prGpuDeviceDestroy(device);
}
