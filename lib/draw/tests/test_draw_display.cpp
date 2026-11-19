// Tests of the display shader on the platform's GPU.

#include "pr/draw_display.h"
#include "pr/float.h"
#include "pr/gpu.h"

#include "tools/harness/harness_gpu.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace harness;

namespace {

constexpr uint32_t kTexels = 4;

struct Texels {
    PrColor at[kTexels];
};

// Writes `frame`, premultiplied texels of lib/draw, mapped to a target of `space` on an SDR display of sRGB's gamut, to
// `out`.
void mapped(const Texels& frame, PrColorSpace space, Texels* out) {
    PrGpuDevice* device = nullptr;
    REQUIRE(gpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) == PrStatus_Ok);
    PrDrawDisplay* display = nullptr;
    REQUIRE(prDrawDisplayCreate(device, &display) == PrStatus_Ok);
    PrUVec2 extent{kTexels, 1};
    PrGpuTextureDesc frameDesc = prGpuTextureDesc2d(PrGpuFormat_Rgba32F, extent, PrGpuTextureUsageFlag_Sampled);
    PrGpuTextureDesc targetDesc = prGpuTextureDesc2d(PrGpuFormat_Rgba32F, extent,
        PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    PrGpuTexture source{};
    PrGpuTexture target{};
    REQUIRE(prGpuTextureCreate(device, &frameDesc, &source) == PrStatus_Ok);
    REQUIRE(prGpuTextureCreate(device, &targetDesc, &target) == PrStatus_Ok);
    Texels texels = frame;
    PrPixelView upload{reinterpret_cast<const uint8_t*>(texels.at), sizeof texels, extent};
    prGpuTextureWrite(device, source, PrIVec2{0, 0}, upload);

    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
        PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc passDesc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
    PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
    PrDrawDisplayDesc desc{.target = space, .headroom = 1, .gamut = PrColorSpace_Srgb};
    prDrawDisplayDraw(display, pass, source, &desc);
    prGpuPassEnd(pass);
    prGpuCommandsTextureBarrier(commands, target, PrGpuSubresources{}, PrGpuAccessFlag_ColorAttachment,
        PrGpuAccessFlag_ShaderRead);
    gpuSubmitAndWait(commands);

    PrPixels read{reinterpret_cast<uint8_t*>(out->at), sizeof *out, extent};
    gpuTextureRead(device, target, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, read);
    prGpuTextureRelease(device, source);
    prGpuTextureRelease(device, target);
    prDrawDisplayDestroy(display);
    prGpuDeviceDestroy(device);
}

void checkColor(PrColor actual, PrColor expected) {
    CAPTURE(actual.r);
    CAPTURE(actual.g);
    CAPTURE(actual.b);
    CAPTURE(actual.a);
    CHECK(prF32Abs(actual.r - expected.r) < 1e-3f);
    CHECK(prF32Abs(actual.g - expected.g) < 1e-3f);
    CHECK(prF32Abs(actual.b - expected.b) < 1e-3f);
    CHECK(prF32Abs(actual.a - expected.a) < 1e-3f);
}

// sRGB's transfer function to linear light, for a channel in [0, 1].
float linearOf(float c) {
    return c <= 0.04045f ? c / 12.92f : prF32Pow((c + 0.055f) / 1.055f, 2.4f);
}

// Opaque, half covered in two colors, and uncovered.
const Texels kFrame{
    {
        PrColor{0.5f, 0.25f, 1, 1},
        PrColor{0.25f, 0.125f, 0.5f, 0.5f},
        PrColor{0.4f, 0.2f, 0.1f, 0.5f},
        PrColor{0, 0, 0, 0},
    },
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// The tests that draw on the platform's GPU, which a run without one skips (harness::gpuTests).
TEST_SUITE_BEGIN("gpu" * doctest::skip(!gpuTests()));

TEST_CASE("the display shader keeps the frame's alpha, with colors premultiplied") {
    SUBCASE("an encoded target takes the frame as it is") {
        Texels out{};
        mapped(kFrame, PrColorSpace_Srgb, &out);
        for (uint32_t i = 0; i < kTexels; ++i) {
            CAPTURE(i);
            checkColor(out.at[i], kFrame.at[i]);
        }
    }

    SUBCASE("a linear target premultiplies the linear color") {
        Texels out{};
        mapped(kFrame, PrColorSpace_SrgbLinear, &out);
        for (uint32_t i = 0; i < kTexels; ++i) {
            CAPTURE(i);
            PrColor f = kFrame.at[i];
            PrColor expected{0, 0, 0, 0};
            if (f.a > 0)
                expected = {linearOf(f.r / f.a) * f.a, linearOf(f.g / f.a) * f.a, linearOf(f.b / f.a) * f.a, f.a};
            checkColor(out.at[i], expected);
        }
    }

    SUBCASE("a PQ target is opaque, showing the frame over black") {
        Texels out{};
        mapped(kFrame, PrColorSpace_Rec2100Pq, &out);
        for (uint32_t i = 0; i < kTexels; ++i) {
            CAPTURE(i);
            CHECK(out.at[i].a == 1);
        }
        CHECK(out.at[3].r == doctest::Approx(out.at[3].g));
        CHECK(out.at[1].r < out.at[0].r);
    }
}

TEST_SUITE_END();
