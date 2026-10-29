// Studio Kit's images.

#include "pr/draw_image.h"
#include "pr/kit_box.h"
#include "pr/kit_image.h"

#include "tools/harness/kit_fixture.h"

#include "doctest/doctest.h"

#include <vector>

using namespace harness;

namespace pr_impl {

namespace {

// Returns an image of `width` by `height` opaque white pixels, which the caller releases.
const PrDrawImage* whiteImage(uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(size_t(width) * height * 4, 255);
    PrDrawImageDesc desc{PrPixelView{pixels.data(), width * 4, {width, height}}, PrPixelFormat_Rgba8,
                         PrDrawAlpha_Straight, PrColorSpace_Srgb};
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    return image;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an image takes its own size unless it is placed, and keeps its records from round to round") {
    KitFixture f;
    const PrDrawImage* image = whiteImage(64, 32);
    PrLayoutNode natural = kPrLayoutNodeNone;
    PrLayoutNode framed = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode ground) {
        // The row aligns its items at their start, so that the framed image's height does not stretch the other.
        PrKitBoxStyle line{.direction = PrKitDirection_Row, .align = PrKitAlign_Start};
        PrLayoutNode row = prKitBox(f.kit, ground, {}, line);
        natural = prKitImage(f.kit, row, {.image = image}, {});
        PrKitImageStyle cover{.fit = PrKitImageFit_Cover,
                              .alignX = PrKitAlign_Start,
                              .place = {.width = prKitPx(40), .height = prKitPx(40), .flex = PrKitFlex_Fixed}};
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
    prDrawImageRelease(image);
    CHECK(kitVerified(f.tree));
}
