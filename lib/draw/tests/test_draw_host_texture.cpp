// Host textures: an import, the lists that hold a texture, and an import into a texture that nothing else holds.

#include "pr/draw_host_texture.h"
#include "pr/draw_list.h"
#include "pr/gpu_memory.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Imports a picture of 2 by 1 texels whose first byte is `value` into `*texture`.
PrStatus importValue(PrGpuDevice* device, uint8_t value, PrDrawHostTexture** texture) {
    const uint8_t texels[8] = {value, 0, 0, 255, value, 0, 0, 255};
    PrGpuImportPixels pixels{.pixels = PrPixelView{texels, 8, {2, 1}}, .format = PrGpuFormat_Rgba8};
    PrGpuImport image{.kind = PrGpuImportKind_Pixels, .handle = &pixels};
    return prDrawHostTextureImport(device, &image, PrTextureAlpha_Premultiplied, texture);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("draw host texture");

TEST_CASE("an import goes into a host texture that only the host holds, and makes another while a list draws it") {
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    PrDrawHostTexture* texture = nullptr;
    REQUIRE(importValue(device, 10, &texture) == PrStatus_Ok);
    REQUIRE(texture != nullptr);
    CHECK(prDrawHostTextureExtent(texture) == PrUVec2{2, 1});
    CHECK(prDrawHostTextureDevice(texture) == device);

    // Nothing else holds the texture, so the next picture replaces what it shows.
    PrDrawHostTexture* first = texture;
    REQUIRE(importValue(device, 20, &texture) == PrStatus_Ok);
    CHECK(texture == first);

    // A list that draws the texture holds it. The next picture is another texture, and the list keeps the first.
    PrDrawList* list = prDrawListCreate();
    PrDrawSource source = prDrawSourceHostTexture(list, texture);
    CHECK(source.kind == PrDrawSourceKind_Texture);
    CHECK(prRectWidth(source.rect) == 2);
    REQUIRE(importValue(device, 30, &texture) == PrStatus_Ok);
    CHECK(texture != first);

    // Once the list is emptied, only the host holds the second texture, and an import goes into it again.
    PrDrawHostTexture* second = texture;
    prDrawSourceHostTexture(list, texture);
    prDrawListClear(list);
    REQUIRE(importValue(device, 40, &texture) == PrStatus_Ok);
    CHECK(texture == second);

    prDrawListRelease(list);
    prDrawHostTextureRelease(texture);
    prGpuDeviceDestroy(device);
}

TEST_SUITE_END();
