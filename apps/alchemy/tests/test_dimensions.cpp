#include "apps/alchemy/dimensions.h"

#include "tools/harness/test.h"

using namespace alchemy;

namespace {

// A recorded portrait run, and the square that a ladder starts at.
constexpr DimensionSize kPortrait = {832, 1152};
constexpr DimensionSize kSquare = {1024, 1024};

bool same(DimensionSize a, DimensionSize b) {
    return a.width == b.width && a.height == b.height;
}

} // namespace

TEST_CASE("a ladder starts square, and an aspect fits onto the nearest entry of the family") {
    Dimensions dims;
    CHECK(same(dims.size, kSquare));
    CHECK(same(applyAspectRatio(dims, kPortrait), kPortrait));
    setDimensionFamily(dims, DimensionFamily_Sd15);
    CHECK(same(dims.size, DimensionSize{512, 704}));
    setDimensionFamily(dims, DimensionFamily_Flexible);
    CHECK(dims.size.width % 64 == 0);
    CHECK(sizeAspect(dims.size) == doctest::Approx(sizeAspect(kPortrait)).epsilon(0.05));
}

TEST_CASE("a flexible budget keeps its pixels whatever the aspect, in steps of 64") {
    CHECK(same(flexibleSize(1.0, sizeAspect(kPortrait)), kPortrait));
    DimensionSize small = flexibleSize(0.5, sizeAspect(kPortrait));
    DimensionSize large = flexibleSize(2.0, sizeAspect(kPortrait));
    CHECK(sizeMegapixels(small) < 0.7);
    CHECK(sizeMegapixels(large) > 1.7);
    CHECK(sizeAspect(large) == doctest::Approx(sizeAspect(kPortrait)).epsilon(0.05));
    // The size override keeps its own area and takes the new aspect.
    Dimensions dims;
    setCustomSize(dims, true);
    CHECK(same(applyAspectRatio(dims, kPortrait), DimensionSize{896, 1216}));
}

TEST_CASE("a recorded size lands on the ladder that states it exactly, or on none") {
    Dimensions dims;
    REQUIRE(adoptExactSize(dims, {512, 704}));
    CHECK(dims.family == DimensionFamily_Sd15);
    CHECK(same(dims.size, DimensionSize{512, 704}));
    REQUIRE(adoptExactSize(dims, kPortrait));
    CHECK(dims.family == DimensionFamily_Sdxl);
    REQUIRE(adoptExactSize(dims, flexibleSize(1.5, 1.5)));
    CHECK(dims.family == DimensionFamily_Flexible);
    CHECK_FALSE(adoptExactSize(dims, {1024, 1088}));
    CHECK_FALSE(adoptExactSize(dims, {833, 1152}));
}

TEST_CASE("the checkpoint's architecture chooses the family, ahead of what its name says") {
    Dimensions dims;
    seedDimensions(dims, "dreamshaper_8.safetensors", ModelArchitecture_Unknown);
    CHECK(dims.family == DimensionFamily_Sdxl);
    seedDimensions(dims, "v1-5-pruned.safetensors", ModelArchitecture_Unknown);
    CHECK(dims.family == DimensionFamily_Sd15);
    CHECK_FALSE(dims.detected);
    // A family that a caller chose stands while the checkpoint stays selected.
    setDimensionFamily(dims, DimensionFamily_Flexible);
    seedDimensions(dims, "v1-5-pruned.safetensors", ModelArchitecture_Unknown);
    CHECK(dims.family == DimensionFamily_Flexible);
    // The file's own answer replaces the name's.
    seedDimensions(dims, "v1-5-pruned.safetensors", ModelArchitecture_Sdxl);
    CHECK(dims.family == DimensionFamily_Sdxl);
    CHECK(dims.detected);
    CHECK(familyForCheckpoint("flux1-dev.safetensors", DimensionFamily_Sdxl) == DimensionFamily_Flexible);
    CHECK(familyForArchitecture(ModelArchitecture_SdxlRefiner) == DimensionFamily_Sdxl);
}

TEST_CASE("a swap flips the orientation on the ladder, and labels name the size and its ratio") {
    Dimensions dims;
    applyAspectRatio(dims, kPortrait);
    swapDimensions(dims);
    CHECK(same(dims.size, DimensionSize{1152, 832}));
    CHECK(dimensionLabel(dims.size) == "1152 x 832 (18:13)");
    CHECK(dimensionRatioLabel(kSquare) == "1:1");
    CHECK(parseFamily("SD15", DimensionFamily_Sdxl) == DimensionFamily_Sd15);
    CHECK(parseFamily("other", DimensionFamily_Flexible) == DimensionFamily_Flexible);
}
