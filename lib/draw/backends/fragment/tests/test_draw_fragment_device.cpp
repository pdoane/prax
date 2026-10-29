#include "pr/color_space.h"
#include "pr/draw_cpu.h"
#include "pr/draw_filter.h"
#include "pr/draw_fragment.h"
#include "pr/draw_list.h"
#include "pr/gpu.h"
#include "pr/vec.h"

#include "doctest/doctest.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

extern "C" int prTestDrawFragmentCApi(void);

namespace pr_impl {

namespace {

// Creates a device on the platform's GPU.
PrGpuDevice* createDevice() {
    PrGpuDevice* device = nullptr;
    REQUIRE(prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) == PrStatus_Ok);
    return device;
}

// Creates the renderer on the device.
PrDrawRenderer* createRenderer(PrGpuDevice* device, const PrDrawFragmentOptions* PR_NULLABLE options = nullptr) {
    PrDrawRenderer* renderer = nullptr;
    REQUIRE(prDrawFragmentCreate(device, options, &renderer) == PrStatus_Ok);
    return renderer;
}

// Begins a pass over the color attachment of `desc`, which rests in the accesses of shader reads, as the renderer's
// textures do.
PrGpuPass* beginPass(PrGpuCommands* commands, const PrGpuPassDesc* desc) {
    prGpuCommandsTextureBarrier(commands, desc->color[0].texture, PrGpuSubresources{}, PrGpuAccessFlag_ShaderRead,
                                PrGpuAccessFlag_ColorAttachment);
    return prGpuPassBegin(commands, desc);
}

// Ends the pass, and returns its color attachment to the accesses of shader reads.
void endPass(PrGpuPass* pass, const PrGpuPassDesc* desc) {
    PrGpuCommands* commands = prGpuPassCommands(pass);
    prGpuPassEnd(pass);
    prGpuCommandsTextureBarrier(commands, desc->color[0].texture, PrGpuSubresources{},
                                PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
}

// Copies all of `from` into `to` at its origin. Both rest in the accesses of shader reads.
void copyTexture(PrGpuCommands* commands, PrGpuTexture from, PrGpuTexture to) {
    PrGpuSubresources all{};
    PrUVec2 extent = prGpuTextureExtent(prGpuCommandsDevice(commands), from);
    PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, extent);
    prGpuCommandsTextureBarrier(commands, from, all, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_CopyRead);
    prGpuCommandsTextureBarrier(commands, to, all, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_CopyWrite);
    prGpuCommandsCopyTexture(commands, from, &region, to, &region);
    prGpuCommandsTextureBarrier(commands, from, all, PrGpuAccessFlag_CopyRead, PrGpuAccessFlag_ShaderRead);
    prGpuCommandsTextureBarrier(commands, to, all, PrGpuAccessFlag_CopyWrite, PrGpuAccessFlag_ShaderRead);
}

struct Device {
    PrGpuDevice* gpuDevice = createDevice();
    PrDrawRenderer* renderer = nullptr;
    PrGpuTexture texture{};
    PrDrawFrame* frame = nullptr;
    uint32_t width;
    uint32_t height;

    Device(uint32_t w, uint32_t h, PrDrawFragmentPaths paths = PrDrawFragmentPaths_Tessellate,
           PrDrawFragmentEmbeds embeds = PrDrawFragmentEmbeds_Keep)
        : width(w), height(h) {
        PrDrawFragmentOptions options = prDrawFragmentOptionsDefault();
        options.paths = paths;
        options.embeds = embeds;
        renderer = createRenderer(gpuDevice, &options);
        PrGpuTextureDesc desc = prGpuTextureDesc2d(prGpuFormatFromPixelFormat(PrPixelFormat_Rgba8), PrUVec2{w, h},
                                                   PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        REQUIRE(prGpuTextureCreate(gpuDevice, &desc, &texture) == PrStatus_Ok);
    }

    ~Device() {
        prDrawFrameDestroy(frame);
        prDrawRendererDestroy(renderer);
        prGpuTextureDestroy(gpuDevice, texture);
        prGpuDeviceDestroy(gpuDevice);
    }

    // Prepares and encodes the list in a pass over the texture, then reads the texture back.
    std::vector<uint8_t> render(const PrDrawList* list) {
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(gpuDevice, nullptr, &commands));
        PrDrawTarget prepared{.extent = prGpuTextureExtent(gpuDevice, texture),
                              .format = PrPixelFormat_Rgba8,
                              .commands = commands};
        if (frame == nullptr)
            frame = prDrawFrameCreate(renderer);
        REQUIRE(prDrawFramePrepare(frame, list, &prepared) == PrStatus_Ok);
        PrGpuPassDesc desc = prGpuPassDescColor(texture, PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
        PrGpuPass* pass = beginPass(commands, &desc);
        prDrawFrameRender(frame, pass, PrIVec2{0, 0});
        endPass(pass, &desc);
        prGpuCommandsSubmitAndWait(commands);
        std::vector<uint8_t> pixels(size_t(width) * height * 4);
        PrPixels out{pixels.data(), size_t(width) * 4, {width, height}};
        prGpuTextureRead(gpuDevice, texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, out);
        return pixels;
    }

    // Returns the counts of the work that the last render's prepare produced.
    PrDrawStats stats() const {
        return prDrawFrameStats(frame);
    }
};

// Returns the list rendered by the cpu backend onto transparent pixels, which serves as the reference.
std::vector<uint8_t> reference(const PrDrawList* list, uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(size_t(width) * height * 4, 0);
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrPixels into{pixels.data(), size_t(width) * 4, {width, height}};
    REQUIRE(prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(renderer);
    return pixels;
}

double inked(const std::vector<uint8_t>& pixels) {
    double sum = 0;
    for (size_t i = 3; i < pixels.size(); i += 4)
        sum += pixels[i] / 255.0;
    return sum;
}

// Returns the fraction of pixels whose channels differ by more than `tolerance`.
double fractionDiffering(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int tolerance) {
    uint32_t differing = 0;
    for (size_t i = 0; i < a.size(); i += 4) {
        for (size_t k = 0; k < 4; ++k) {
            if (std::abs(int(a[i + k]) - int(b[i + k])) > tolerance) {
                ++differing;
                break;
            }
        }
    }
    return double(differing) / double(a.size() / 4);
}

const PrFont* load(const char* name) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + name;
    std::vector<uint8_t> bytes;
    FILE* file = std::fopen(path.c_str(), "rb");
    REQUIRE(file != nullptr);
    uint8_t buffer[65536];
    size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        bytes.insert(bytes.end(), buffer, buffer + read);
    std::fclose(file);
    const PrFont* font = nullptr;
    REQUIRE(prFontCreate(bytes, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// Creates a filter of the nodes, in order.
const PrDrawFilter* filterOf(std::initializer_list<PrDrawFilterNode> nodes) {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    for (const PrDrawFilterNode& node : nodes)
        prDrawFilterBuilderAdd(builder, &node);
    const PrDrawFilter* filter = nullptr;
    filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    return filter;
}

PrDrawFilterNode nodeOf(PrDrawFilterKind kind, PrColorSpace space = PrColorSpace_Srgb) {
    PrDrawFilterNode node{};
    node.kind = kind;
    node.space = space;
    node.inputs[0] = PrDrawFilterInput_Source;
    node.inputs[1] = PrDrawFilterInput_Source;
    return node;
}

// Records shapes in several colors within `cell`, on transparency.
void subject(PrDrawList* list, PrRect cell) {
    float x = cell.left;
    float y = cell.top;
    prDrawListEllipse(list, PrVec2{x + 22, y + 24}, PrVec2{14, 14}, 0, {.color = PrColor{0.9f, 0.1f, 0.1f, 1}});
    prDrawListRect(list, PrRect{x + 26, y + 26, x + 50, y + 48}, {.color = PrColor{0.1f, 0.2f, 0.9f, 1}});
}

// Pushes a group bounded to `cell`, with the filter acting on its contents or its backdrop, and releases the filter.
void pushFiltered(PrDrawList* list, PrRect cell, const PrDrawFilter* filter, bool backdrop) {
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = cell;
    if (backdrop)
        group.backdrop = filter;
    else
        group.filter = filter;
    prDrawListPushGroup(list, &group);
    prDrawFilterRelease(filter);
}

// Redraws what lies beneath a render by rendering a frame prepared with the background at the render's position.
void renderBeneath(void* PR_NULLABLE context, PrGpuPass* pass, PrIRect area) {
    prDrawFrameRender(static_cast<PrDrawFrame*>(context), pass, PrIVec2{-area.left, -area.top});
}

// A plane tilted back about its horizontal center line by `radians`, seen from `distance` in front, and placed at
// `center`.
PrMatrix4 tiltedPlane(float radians, float distance, PrVec2 center) {
    PrMatrix4 tilt = prMatrix4Rotate(PrVec3{1, 0, 0}, radians);
    PrMatrix4 eye = prMatrix4Perspective(distance);
    PrMatrix4 place = prMatrix4Translate(PrVec3{center.x, center.y, 0});
    PrMatrix4 seen = prMatrix4Concat(&tilt, &eye);
    return prMatrix4Concat(&seen, &place);
}

// Records a gradient under a checkerboard of eight columns over `r`. It shows both where the projection places the
// plane and how the plane is sampled.
void planeContent(PrDrawList* list, PrRect r) {
    PrDrawColorStop stops[2] = {{.offset = 0, .color = PrColor{1, 0.2f, 0, 1}},
                                {.offset = 1, .color = PrColor{0, 0.3f, 1, 1}}};
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{r.left, r.top}, PrVec2{r.right, r.bottom}, stops, 2);
    prDrawListRect(list, r, {.brush = &brush});
    float cell = prRectWidth(r) / 8;
    uint32_t rows = uint32_t(prRectHeight(r) / cell);
    for (uint32_t y = 0; y < rows; ++y) {
        for (uint32_t x = (y & 1); x < 8; x += 2) {
            PrRect square{r.left + cell * float(x), r.top + cell * float(y), r.left + cell * float(x + 1),
                          r.top + cell * float(y + 1)};
            prDrawListRect(list, square, {.color = PrColor{0, 0, 0, 0.5f}});
        }
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the fragment renderer evaluates superellipse corners as the cpu backend does, in fills and clips") {
    Device device(96, 64);
    PrDrawList* list = prDrawListCreate();
    PrCorner squircle{{14, 14}, 2};
    PrCorner bevel{{10, 6}, 0};
    PrCorner scoop{{12, 12}, -1};
    PrCorner notch{{8, 8}, -INFINITY};
    prDrawListCornerRect(list, PrCornerRect{PrRect{4, 4, 44, 60}, {squircle, bevel, scoop, notch}},
                         {.color = PrColor{0.2f, 0.4f, 0.8f, 1}});
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.3f), prAffine2Translate(PrVec2{60, 6})));
    prDrawListPushClipCornerRect(list, PrCornerRect{PrRect{0, 0, 30, 40}, {scoop, squircle, squircle, bevel}},
                                 PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{-10, -10, 50, 60}, {.color = PrColor{0.8f, 0.2f, 0.2f, 1}});
    prDrawListPopClip(list);
    prDrawListPopTransform(list);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 96, 64);
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.005));
    CHECK(fractionDiffering(drawn, expected, 24) <= 0.01);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer draws the host's texture as the cpu backend draws an image of its texels") {
    Device device(64, 32);
    // Straight texels: a translucent red over one half, and an opaque blue over the other.
    std::vector<uint8_t> texels(8 * 8 * 4);
    for (uint32_t i = 0; i < 64; ++i) {
        uint8_t* t = texels.data() + i * 4;
        bool left = i % 8 < 4;
        t[0] = left ? 255 : 0;
        t[1] = 0;
        t[2] = left ? 0 : 255;
        t[3] = left ? 128 : 255;
    }
    PrGpuTextureDesc desc = prGpuTextureDesc2d(prGpuFormatFromPixelFormat(PrPixelFormat_Rgba8), PrUVec2{8, 8},
                                               PrGpuTextureUsageFlag_Sampled);
    PrGpuTexture texture{};
    REQUIRE(prGpuTextureCreate(device.gpuDevice, &desc, &texture) == PrStatus_Ok);
    PrPixels upload{texels.data(), 32, {8, 8}};
    prGpuTextureWrite(device.gpuDevice, texture, PrIVec2{0, 0}, prPixelsView(upload));
    for (PrDrawAlpha alpha : {PrDrawAlpha_Straight, PrDrawAlpha_Opaque}) {
        CAPTURE(int(alpha));
        PrDrawImageDesc imageDesc{PrPixelView{texels.data(), 32, {8, 8}}, PrPixelFormat_Rgba8, alpha,
                                        PrColorSpace_Srgb};
        const PrDrawImage* image = nullptr;
        REQUIRE(prDrawImageCreate(&imageDesc, &image) == PrStatus_Ok);
        auto draw = [&](PrDrawSource source) {
            PrDrawList* list = prDrawListCreate();
            PrDrawBrush brush = prDrawBrushImage(source, PrRect{4, 4, 60, 28}, PrDrawSampling_Nearest);
            prDrawListRect(list, PrRect{4, 4, 60, 28}, {.brush = &brush});
            return list;
        };
        PrDrawList* fromTexture =
            draw(prDrawSourceTexture(prDrawGpuTexture(texture), prGpuTextureExtent(device.gpuDevice, texture),
                                     PrRect{0, 0, 8, 8}, alpha));
        PrDrawList* fromImage = draw(prDrawSourceImage(image, PrRect{0, 0, 8, 8}));
        std::vector<uint8_t> drawn = device.render(fromTexture);
        std::vector<uint8_t> expected = reference(fromImage, 64, 32);
        CHECK(fractionDiffering(drawn, expected, 2) == 0);
        prDrawListRelease(fromTexture);
        prDrawListRelease(fromImage);
        prDrawImageRelease(image);
    }
    prDrawFrameDestroy(device.frame);
    device.frame = nullptr;
    prGpuTextureDestroy(device.gpuDevice, texture);
}

TEST_CASE("the fragment renderer fills shapes as the cpu backend does") {
    Device device(96, 64);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{4.25f, 6.5f, 40.75f, 30}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListEllipse(list, PrVec2{64, 32}, PrVec2{20, 14}, 0.3f, {.color = PrColor{0, 0, 1, 0.8f}});
    PrCornerRect shape{PrRect{6, 36, 44, 60}, {{{8, 8}, 1}, {{8, 4}, 1}, {{0, 0}, 1}, {{12, 12}, 1}}};
    prDrawListCornerRect(list, shape, {.color = PrColor{0, 0.5f, 0, 1}});
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{50, 50});
    prPathLineTo(path, PrVec2{90, 54});
    prPathLineTo(path, PrVec2{60, 62});
    prPathClose(path);
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = PrColor{0.5f, 0.5f, 0, 1}});
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 96, 64);
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.005));
    CHECK(fractionDiffering(drawn, expected, 24) <= 0.01);
    prPathDestroy(path);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer paints each kind of gradient per pixel") {
    PrDrawColorStop stops[3] = {{.offset = 0, .color = PrColor{1, 0, 0, 1}},
                                {.offset = 0.5f, .color = PrColor{0, 1, 0, 1}},
                                {.offset = 1, .color = PrColor{0, 0, 1, 1}}};
    for (PrDrawBrushKind kind : {PrDrawBrushKind_Linear, PrDrawBrushKind_Radial, PrDrawBrushKind_Conic,
                                 PrDrawBrushKind_Diamond}) {
        CAPTURE(int(kind));
        Device device(96, 96);
        PrDrawList* list = prDrawListCreate();
        PrDrawBrush brush = prDrawBrushDefault(kind);
        brush.extend = PrDrawExtend_Reflect;
        brush.transform = prAffine2Rotate(0.3f);
        brush.stops = stops;
        brush.stopCount = 3;
        brush.linear.start = PrVec2{10, 10};
        brush.linear.end = PrVec2{40, 20};
        if (kind == PrDrawBrushKind_Radial)
            brush.radial = {PrVec2{40, 25}, 0, PrVec2{50, 20}, 30};
        if (kind == PrDrawBrushKind_Conic)
            brush.conic = {PrVec2{50, 20}, 0.4f};
        if (kind == PrDrawBrushKind_Diamond)
            brush.diamond = {PrVec2{50, 20}, PrVec2{25, 15}};
        prDrawListRect(list, PrRect{0, 0, 96, 96}, {.brush = &brush});
        std::vector<uint8_t> drawn = device.render(list);
        std::vector<uint8_t> expected = reference(list, 96, 96);
        CHECK(fractionDiffering(drawn, expected, 6) <= 0.01);
        prDrawListRelease(list);
    }
}

TEST_CASE("the fragment renderer clips to rounded rectangles as the cpu backend does") {
    Device device(64, 64);
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.4f), prAffine2Translate(PrVec2{30, 4})));
    PrCorner round = prCornerRound(PrVec2{8, 6});
    prDrawListPushClipCornerRect(list, PrCornerRect{PrRect{0, 0, 30, 36}, {round, round, round, round}},
                                 PrDrawEdges_Antialiased);
    prDrawListPushClipRect(list, PrRect{8.5f, -10, 64, 64}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{-40, -40, 80, 80}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopClip(list);
    prDrawListPopClip(list);
    prDrawListPopTransform(list);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 64, 64);
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.005));
    CHECK(fractionDiffering(drawn, expected, 24) <= 0.01);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer blurs shadows as the cpu backend does") {
    Device device(128, 128);
    PrDrawList* list = prDrawListCreate();
    PrDrawShadow shadow{};
    shadow.shape.rect = PrRect{32, 32, 96, 88};
    for (PrCorner& corner : shadow.shape.corners)
        corner = PrCorner{{10, 10}, 1};
    shadow.offset = PrVec2{4, 6};
    shadow.sigma = 8;
    shadow.spread = 2;
    prDrawListDropShadow(list, &shadow, {.color = PrColor{0, 0, 0, 0.8f}});
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 128, 128);
    // The cpu backend's blur is three box passes, whose tails are lighter than the Gaussian's tails.
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.05));
    CHECK(fractionDiffering(drawn, expected, 8) <= 0.02);

    prDrawListClear(list);
    prDrawListInnerShadow(list, &shadow, {.color = PrColor{0, 0, 0, 0.8f}});
    drawn = device.render(list);
    expected = reference(list, 128, 128);
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.03));
    CHECK(fractionDiffering(drawn, expected, 8) <= 0.03);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer blurs a shadow by the Gaussian") {
    // Compares the outer shadow of a square-cornered rectangle with the Gaussian's integral at each pixel's center.
    Device device(128, 128);
    PrDrawList* list = prDrawListCreate();
    PrDrawShadow shadow{};
    shadow.shape.rect = PrRect{32, 32, 96, 88};
    shadow.sigma = 8;
    prDrawListDropShadow(list, &shadow, {.color = PrColor{0, 0, 0, 1}});
    double sigma = 8;
    auto profile = [&](double x, double a, double b) {
        return 0.5 * (std::erf((x - a) / (sigma * std::sqrt(2.0))) - std::erf((x - b) / (sigma * std::sqrt(2.0))));
    };
    double ink = 0;
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            if (x >= 32 && x < 96 && y >= 32 && y < 88)
                continue;
            ink += profile(x + 0.5, 32, 96) * profile(y + 0.5, 32, 88);
        }
    }
    CHECK(inked(device.render(list)) == doctest::Approx(ink).epsilon(0.015));
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer tiles an image brush in the shader") {
    Device device(64, 64);
    // Quadrants of red, green, blue and white, a texel to a pixel.
    std::vector<uint8_t> texels(16 * 16 * 4);
    for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            uint8_t* t = texels.data() + (y * 16 + x) * 4;
            uint32_t quadrant = (y < 8 ? 0 : 2) + (x < 8 ? 0 : 1);
            t[0] = quadrant == 0 || quadrant == 3 ? 255 : 0;
            t[1] = quadrant == 1 || quadrant == 3 ? 255 : 0;
            t[2] = quadrant >= 2 ? 255 : 0;
            t[3] = 255;
        }
    }
    PrDrawImageDesc desc{PrPixelView{texels.data(), 64, {16, 16}}, PrPixelFormat_Rgba8, PrDrawAlpha_Premultiplied,
                         PrColorSpace_Srgb};
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    for (PrDrawExtend mode : {PrDrawExtend_None, PrDrawExtend_Repeat, PrDrawExtend_Reflect}) {
        CAPTURE(int(mode));
        PrDrawList* list = prDrawListCreate();
        PrDrawBrush brush = prDrawBrushDefault(PrDrawBrushKind_Image);
        brush.image.source = prDrawSourceImage(image, PrRect{0, 0, 16, 16});
        brush.image.tiles.tile = PrRect{8, 8, 24, 24};
        brush.image.tiles.spacing = PrVec2{4, 4};
        brush.image.tiles.x = brush.image.tiles.y = mode;
        brush.image.sampling = PrDrawSampling_Nearest;
        prDrawListRect(list, PrRect{0, 0, 64, 64}, {.brush = &brush});
        std::vector<uint8_t> drawn = device.render(list);
        std::vector<uint8_t> expected = reference(list, 64, 64);
        CHECK(fractionDiffering(drawn, expected, 8) <= 0.01);
        // After the backend drops the resources that it created on the device, the next frame recreates them.
        prDrawFrameDestroy(device.frame);
        device.frame = nullptr;
        prDrawRendererDropResources(device.renderer);
        CHECK(device.render(list) == drawn);
        prDrawListRelease(list);
    }
    prDrawImageRelease(image);
}

TEST_CASE("the fragment renderer composites groups through layers, with masks, operators and blend modes") {
    Device device(96, 96);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 96, 96}, {.color = PrColor{0.9f, 0.9f, 0.8f, 1}});
    // Overlapping shapes in a translucent group. The overlap does not darken.
    PrDrawGroup group = prDrawGroupDefault();
    group.opacity = 0.5f;
    prDrawListPushGroup(list, &group);
    prDrawListEllipse(list, PrVec2{30, 30}, PrVec2{20, 20}, 0, {.color = PrColor{0, 0, 1, 1}});
    prDrawListRect(list, PrRect{30, 30, 60, 60}, {.color = PrColor{1, 0, 0, 1}});
    prDrawListPopGroup(list);
    // A group masked by a gradient's luminance, blended by multiply.
    PrDrawColorStop stops[2] = {{.offset = 0, .color = PrColor{1, 1, 1, 1}},
                                {.offset = 1, .color = PrColor{0, 0, 0, 1}}};
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{50, 0}, PrVec2{90, 0}, stops, 2);
    PrDrawMask mask = prDrawMaskDefault(&brush);
    mask.mode = PrDrawMaskMode_Luminance;
    PrDrawGroup masked = prDrawGroupDefault();
    masked.blendMode = PrDrawBlendMode_Multiply;
    masked.mask = &mask;
    masked.bounds = PrRect{50, 50, 90, 90};
    prDrawListPushGroup(list, &masked);
    prDrawListRect(list, PrRect{50, 50, 90, 90}, {.color = PrColor{0, 0.6f, 0.2f, 1}});
    prDrawListPopGroup(list);
    // An operator on a single command acts where the command's shape covers. A group's operator reaches the group's
    // bounds, which here are the clip's bounds.
    prDrawListEllipse(list, PrVec2{80, 15}, PrVec2{10, 10}, 0,
                      {.color = PrColor{0, 0, 0, 1}, .composite = PrDrawComposite_DestinationOut});
    prDrawListPushClipRect(list, PrRect{0, 70, 40, 96}, PrDrawEdges_Antialiased);
    prDrawListEllipse(list, PrVec2{20, 84}, PrVec2{12, 8}, 0,
                      {.color = PrColor{1, 0, 1, 1}, .composite = PrDrawComposite_SourceIn});
    prDrawListPopClip(list);
    prDrawListPushClipRect(list, PrRect{60, 60, 96, 96}, PrDrawEdges_Antialiased);
    PrDrawGroup sourceIn = prDrawGroupDefault();
    sourceIn.composite = PrDrawComposite_SourceIn;
    prDrawListPushGroup(list, &sourceIn);
    prDrawListEllipse(list, PrVec2{78, 78}, PrVec2{8, 8}, 0, {.color = PrColor{0, 1, 1, 1}});
    prDrawListPopGroup(list);
    prDrawListPopClip(list);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 96, 96);
    CHECK(fractionDiffering(drawn, expected, 8) <= 0.01);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer clips to paths through a mask layer") {
    Device device(64, 64);
    PrDrawList* list = prDrawListCreate();
    PrPath* star = prPathCreate();
    for (int i = 0; i < 5; ++i) {
        float angle = -1.5708f + float(i) * 4 * 3.14159f / 5;
        PrVec2 p{32 + 28 * std::cos(angle), 32 + 28 * std::sin(angle)};
        if (i == 0)
            prPathMoveTo(star, p);
        else
            prPathLineTo(star, p);
    }
    prPathClose(star);
    prDrawListPushClipPath(list, star, PrPathFillRule_EvenOdd, PrDrawEdges_Antialiased);
    prDrawListPushClipRect(list, PrRect{10, 10, 50.5f, 60}, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 64, 64}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopClip(list);
    prDrawListPopClip(list);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 64, 64);
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.01));
    // The miters of the fringe at the star's points cover a little more than the star's area.
    CHECK(fractionDiffering(drawn, expected, 24) <= 0.03);
    prPathDestroy(star);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer runs filters as steps through layers") {
    Device device(128, 128);
    PrDrawList* list = prDrawListCreate();
    PrDrawFilterNode blur = nodeOf(PrDrawFilterKind_Blur);
    blur.blur.sigma = PrVec2{4, 2};
    pushFiltered(list, PrRect{0, 0, 64, 64}, filterOf({blur}), false);
    subject(list, PrRect{0, 0, 64, 64});
    prDrawListPopGroup(list);
    // A color matrix computed in linear light.
    PrDrawFilterNode matrix = nodeOf(PrDrawFilterKind_ColorMatrix, PrColorSpace_SrgbLinear);
    const float gray[20] = {0.2f, 0.7f, 0.1f, 0, 0, 0.2f, 0.7f, 0.1f, 0, 0, 0.2f, 0.7f, 0.1f, 0, 0, 0, 0, 0, 0.8f, 0};
    for (uint32_t k = 0; k < 20; ++k)
        matrix.matrix.m[k / 5][k % 5] = gray[k];
    pushFiltered(list, PrRect{64, 0, 128, 64}, filterOf({matrix}), false);
    subject(list, PrRect{64, 0, 128, 64});
    prDrawListPopGroup(list);
    PrDrawFilterNode drop = nodeOf(PrDrawFilterKind_DropShadow);
    drop.shadow.offset = PrVec2{4, 5};
    drop.shadow.sigma = PrVec2{3, 3};
    drop.shadow.color = PrColor{0, 0, 0, 0.6f};
    pushFiltered(list, PrRect{0, 64, 64, 128}, filterOf({drop}), false);
    subject(list, PrRect{0, 64, 64, 128});
    prDrawListPopGroup(list);
    PrDrawFilterNode noise = nodeOf(PrDrawFilterKind_Turbulence);
    noise.turbulence.baseFrequency = PrVec2{0.05f, 0.08f};
    noise.turbulence.octaves = 3;
    noise.turbulence.seed = 7;
    pushFiltered(list, PrRect{64, 64, 128, 128}, filterOf({noise}), false);
    subject(list, PrRect{64, 64, 128, 128});
    prDrawListPopGroup(list);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 128, 128);
    // The cpu backend's blur is three box blurs, which fall short of the Gaussian's tails. This backend samples the
    // Gaussian.
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.03));
    CHECK(fractionDiffering(drawn, expected, 32) <= 0.005);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer reads past an input's region by its edge mode, and displaces by a map in its space, as cpu does") {
    Device device(128, 64);
    PrDrawList* list = prDrawListCreate();
    // A region of the subject at the cell's top left, blurred across the cell by each edge mode.
    PrImageEdgeMode edges[3] = {PrImageEdgeMode_Duplicate, PrImageEdgeMode_Wrap, PrImageEdgeMode_Mirror};
    for (uint32_t i = 0; i < 3; ++i) {
        PrRect cell{float(i) * 32, 0, float(i) * 32 + 32, 32};
        PrDrawFilterNode part = nodeOf(PrDrawFilterKind_Offset);
        part.region = PrRect{cell.left + 4, 4, cell.left + 20, 20};
        PrDrawFilterNode blur = nodeOf(PrDrawFilterKind_Blur);
        blur.inputs[0] = 0;
        blur.blur.sigma = PrVec2{2, 2};
        blur.blur.edge = edges[i];
        pushFiltered(list, cell, filterOf({part, blur}), false);
        prDrawListRect(list, PrRect{cell.left + 4, 4, cell.left + 12, 20}, {.color = PrColor{0.9f, 0.2f, 0.1f, 1}});
        prDrawListRect(list, PrRect{cell.left + 12, 4, cell.left + 20, 20}, {.color = PrColor{0.1f, 0.3f, 0.9f, 1}});
        prDrawListPopGroup(list);
    }
    // A map whose red, in linear light, moves the subject to the right.
    PrDrawFilterNode map = nodeOf(PrDrawFilterKind_Flood, PrColorSpace_SrgbLinear);
    map.flood = prColor16FromColor(PrColor{0.5f, 0.5f, 0.5f, 0.5f});
    PrDrawFilterNode displace = nodeOf(PrDrawFilterKind_DisplacementMap, PrColorSpace_SrgbLinear);
    displace.inputs[1] = 0;
    displace.displacement = PrDrawFilterDisplacement{.scale = 20, .x = PrDrawChannel_R, .y = PrDrawChannel_A};
    pushFiltered(list, PrRect{96, 0, 128, 64}, filterOf({map, displace}), false);
    prDrawListRect(list, PrRect{96, 8, 112, 56}, {.color = PrColor{0.5f, 0.6f, 0.2f, 1}});
    prDrawListPopGroup(list);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 128, 64);
    CHECK(fractionDiffering(drawn, expected, 32) <= 0.01);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer stitches turbulence over its tile as cpu does") {
    Device device(128, 64);
    PrDrawList* list = prDrawListCreate();
    // Two tiles, each stitched over its own region, whose lattice wraps at bounds past the lattice's mask.
    for (uint32_t i = 0; i < 2; ++i) {
        PrRect cell{float(i) * 64, 0, float(i) * 64 + 64, 64};
        PrDrawFilterNode noise = nodeOf(PrDrawFilterKind_Turbulence);
        noise.region = cell;
        noise.turbulence.baseFrequency = PrVec2{0.05f, 0.08f};
        noise.turbulence.octaves = 3;
        noise.turbulence.seed = 7;
        noise.turbulence.kind = i == 0 ? PrDrawTurbulenceKind_FractalNoise : PrDrawTurbulenceKind_Turbulence;
        noise.turbulence.stitch = true;
        pushFiltered(list, cell, filterOf({noise}), false);
        subject(list, cell);
        prDrawListPopGroup(list);
    }
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 128, 64);
    CHECK(fractionDiffering(drawn, expected, 4) <= 0.005);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer filters what lies beneath a group as its backdrop") {
    Device device(128, 64);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 128, 64}, {.color = PrColor{0.95f, 0.95f, 0.9f, 1}});
    subject(list, PrRect{0, 0, 64, 64});
    subject(list, PrRect{64, 0, 128, 64});
    PrDrawFilterNode blur = nodeOf(PrDrawFilterKind_Blur);
    blur.blur.sigma = PrVec2{5, 5};
    pushFiltered(list, PrRect{8, 8, 56, 56}, filterOf({blur}), true);
    prDrawListRect(list, PrRect{8, 8, 56, 56}, {.color = PrColor{1, 1, 1, 0.3f}});
    prDrawListPopGroup(list);
    // The backdrop is sharp at the top and most blurred at the bottom.
    PrDrawColorStop stops[2] = {{.offset = 0, .color = PrColor{0, 0, 0, 0}},
                                {.offset = 1, .color = PrColor{0, 0, 0, 1}}};
    PrDrawFilterNode progressive = nodeOf(PrDrawFilterKind_ProgressiveBlur);
    progressive.progressive.sigma = 8;
    progressive.progressive.brush = prDrawBrushLinear(PrVec2{0, 4}, PrVec2{0, 60}, stops, 2);
    pushFiltered(list, PrRect{68, 4, 124, 60}, filterOf({progressive}), true);
    prDrawListPopGroup(list);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 128, 64);
    // The cpu backend's box blurs fall short of the Gaussian's tails here, too.
    CHECK(fractionDiffering(drawn, expected, 32) <= 0.005);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer composites a group in sRGB-linear as the cpu backend does") {
    Device device(64, 32);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 64, 32}, {.color = PrColor{0, 0, 0.2f, 1}});
    PrDrawGroup group = prDrawGroupDefault();
    group.space = PrColorSpace_SrgbLinear;
    group.opacity = 0.75f;
    prDrawListPushGroup(list, &group);
    prDrawListEllipse(list, PrVec2{20, 16}, PrVec2{14, 12}, 0, {.color = PrColor{1, 0.8f, 0.4f, 0.6f}});
    prDrawListPopGroup(list);
    group.opacity = 1;
    group.blendMode = PrDrawBlendMode_Screen;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{36, 4, 60, 28}, {.color = PrColor{0.5f, 0.5f, 0.5f, 1}});
    prDrawListPopGroup(list);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 64, 32);
    CHECK(fractionDiffering(drawn, expected, 4) <= 0.01);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer reads what the host provides beneath a render at its place, as a texture or drawn again") {
    // Stripes, and over them at (16, 0) a panel whose backdrop inverts what lies beneath it and what the panel drew
    // before it.
    constexpr uint32_t kWidth = 48;
    constexpr uint32_t kHeight = 16;
    constexpr uint32_t kPanel = 16;
    constexpr int32_t kAt = 16;
    Device device(kWidth, kHeight);
    PrDrawList* background = prDrawListCreate();
    for (uint32_t x = 0; x < kWidth; x += 4) {
        PrColor color = (x / 4) % 2 == 0 ? PrColor{0.8f, 0.2f, 0.2f, 1} : PrColor{0.2f, 0.2f, 0.8f, 1};
        prDrawListRect(background, PrRect{float(x), 0, float(x + 4), float(kHeight)}, {.color = color});
    }
    PrDrawList* shifted = prDrawListCreate();
    prDrawListPushTransformAffine2(shifted, prAffine2Translate(PrVec2{-float(kAt), 0}));
    for (uint32_t x = 0; x < kWidth; x += 4) {
        PrColor color = (x / 4) % 2 == 0 ? PrColor{0.8f, 0.2f, 0.2f, 1} : PrColor{0.2f, 0.2f, 0.8f, 1};
        prDrawListRect(shifted, PrRect{float(x), 0, float(x + 4), float(kHeight)}, {.color = color});
    }
    prDrawListPopTransform(shifted);
    PrDrawList* panel = prDrawListCreate();
    prDrawListRect(panel, PrRect{2, 2, 8, 8}, {.color = PrColor{0.1f, 0.7f, 0.1f, 1}});
    PrDrawFilterNode invert = nodeOf(PrDrawFilterKind_ColorMatrix);
    invert.inputs[0] = PrDrawFilterInput_Backdrop;
    const float matrix[20] = {-1, 0, 0, 0, 1, 0, -1, 0, 0, 1, 0, 0, -1, 0, 1, 0, 0, 0, 1, 0};
    for (uint32_t i = 0; i < 20; ++i)
        invert.matrix.m[i / 5][i % 5] = matrix[i];
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{1, 1, 15, 15};
    group.backdrop = filterOf({invert});
    prDrawListPushGroup(panel, &group);
    prDrawListRect(panel, PrRect{4, 4, 12, 12}, {.color = PrColor{1, 1, 1, 0.5f}});
    prDrawListPopGroup(panel);
    prDrawFilterRelease(group.backdrop);

    // A second backend redraws the background, in the way that a host would draw its own content.
    PrDrawRenderer* redraw = createRenderer(device.gpuDevice);
    PrDrawFrame* again = nullptr;
    PrDrawFrame* backgroundTarget = nullptr;
    PrDrawFrame* overTexture = nullptr;
    PrDrawFrame* overRedraw = nullptr;
    PrGpuTextureDesc desc =
        prGpuTextureDesc2d(prGpuFormatFromPixelFormat(PrPixelFormat_Rgba8), PrUVec2{kWidth, kHeight},
                           PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    PrGpuTexture backgroundTexture{};
    PrGpuTexture second{};
    REQUIRE(prGpuTextureCreate(device.gpuDevice, &desc, &backgroundTexture) == PrStatus_Ok);
    REQUIRE(prGpuTextureCreate(device.gpuDevice, &desc, &second) == PrStatus_Ok);

    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(device.gpuDevice, nullptr, &commands));
    PrDrawTarget whole{.extent = {kWidth, kHeight},
                       .format = PrPixelFormat_Rgba8,
                       .commands = commands};
    if (backgroundTarget == nullptr)
        backgroundTarget = prDrawFrameCreate(device.renderer);
    REQUIRE(prDrawFramePrepare(backgroundTarget, background, &whole) == PrStatus_Ok);
    PrGpuPassDesc backgroundPass = prGpuPassDescColor(backgroundTexture, PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
    PrGpuPass* pass = beginPass(commands, &backgroundPass);
    prDrawFrameRender(backgroundTarget, pass, PrIVec2{0, 0});
    endPass(pass, &backgroundPass);
    PrDrawBeneath texture{};
    texture.kind = PrDrawBeneathKind_Texture;
    texture.texture = prDrawGpuTexture(backgroundTexture);
    texture.extent = prGpuTextureExtent(device.gpuDevice, backgroundTexture);
    texture.origin = PrIVec2{kAt, 0};
    PrDrawTarget part{.extent = {kPanel, kPanel},
                      .format = PrPixelFormat_Rgba8,
                      .commands = commands};
    if (again == nullptr)
        again = prDrawFrameCreate(redraw);
    REQUIRE(prDrawFramePrepare(again, shifted, &part) == PrStatus_Ok);
    PrDrawBeneath drawn{};
    drawn.kind = PrDrawBeneathKind_Draw;
    drawn.draw = renderBeneath;
    drawn.context = again;
    part.beneath = &texture;
    if (overTexture == nullptr)
        overTexture = prDrawFrameCreate(device.renderer);
    REQUIRE(prDrawFramePrepare(overTexture, panel, &part) == PrStatus_Ok);
    part.beneath = &drawn;
    if (overRedraw == nullptr)
        overRedraw = prDrawFrameCreate(device.renderer);
    REQUIRE(prDrawFramePrepare(overRedraw, panel, &part) == PrStatus_Ok);
    PrDrawFrame* panels[2] = {overTexture, overRedraw};
    PrGpuTexture frames[2] = {device.texture, second};
    for (uint32_t i = 0; i < 2; ++i) {
        PrGpuPassDesc framePass = prGpuPassDescColor(frames[i], PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
        pass = beginPass(commands, &framePass);
        prDrawFrameRender(backgroundTarget, pass, PrIVec2{0, 0});
        prDrawFrameRender(panels[i], pass, PrIVec2{kAt, 0});
        endPass(pass, &framePass);
    }
    prGpuCommandsSubmitAndWait(commands);
    std::vector<uint8_t> results[2];
    for (uint32_t i = 0; i < 2; ++i) {
        results[i].resize(size_t(kWidth) * kHeight * 4);
        PrPixels out{results[i].data(), size_t(kWidth) * 4, {kWidth, kHeight}};
        prGpuTextureRead(device.gpuDevice, frames[i], PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, out);
    }

    // The cpu backend reads what lies beneath from the pixels it draws into.
    std::vector<uint8_t> expected = reference(background, kWidth, kHeight);
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrPixels place{expected.data() + size_t(kAt) * 4, size_t(kWidth) * 4, {kPanel, kPanel}};
    REQUIRE(prDrawRendererRenderPixels(cpu, panel, place, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(cpu);
    CHECK(results[0] == results[1]);
    // The backdrop reads the host's texture under the panel's layer without a pass to combine them. Redrawing what lies
    // beneath takes one pass into one layer, and nothing else.
    CHECK(prDrawFrameStats(overRedraw).passes == prDrawFrameStats(overTexture).passes + 1);
    CHECK(prDrawFrameStats(overRedraw).layers == prDrawFrameStats(overTexture).layers + 1);
    CHECK(fractionDiffering(results[0], expected, 2) == 0);
    // The inverted red stripe beneath the panel is cyan, and the stripes beside it are unchanged.
    CHECK(int(results[0][(5 * kWidth + 17) * 4 + 1]) > 150);
    CHECK(results[0][(5 * kWidth + 1) * 4] == expected[(5 * kWidth + 1) * 4]);
    // Preparing again returns the resources that the last preparation took, except the host's texture.
    REQUIRE(prGpuCommandsBegin(device.gpuDevice, nullptr, &commands));
    part.commands = commands;
    part.beneath = &texture;
    if (overTexture == nullptr)
        overTexture = prDrawFrameCreate(device.renderer);
    CHECK(prDrawFramePrepare(overTexture, panel, &part) == PrStatus_Ok);
    prGpuCommandsSubmitAndWait(commands);

    prGpuTextureDestroy(device.gpuDevice, second);
    prGpuTextureDestroy(device.gpuDevice, backgroundTexture);
    prDrawFrameDestroy(overRedraw);
    prDrawFrameDestroy(overTexture);
    prDrawFrameDestroy(backgroundTarget);
    prDrawFrameDestroy(again);
    prDrawRendererDestroy(redraw);
    prDrawListRelease(panel);
    prDrawListRelease(shifted);
    prDrawListRelease(background);
}

TEST_CASE("the fragment renderer reads the host's texture alone beneath a backdrop where the list drew nothing") {
    // The host's columns, copied into the target before the pass, and a backdrop over them that inverts them.
    constexpr uint32_t kSize = 16;
    Device device(kSize, kSize);
    std::vector<uint8_t> columns(size_t(kSize) * kSize * 4);
    for (uint32_t i = 0; i < kSize * kSize; ++i) {
        bool red = (i % kSize) / 4 % 2 == 0;
        columns[i * 4] = red ? 200 : 40;
        columns[i * 4 + 1] = 60;
        columns[i * 4 + 2] = red ? 40 : 200;
        columns[i * 4 + 3] = 255;
    }
    PrGpuTextureDesc desc = prGpuTextureDesc2d(prGpuFormatFromPixelFormat(PrPixelFormat_Rgba8), PrUVec2{kSize, kSize},
                                               PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    PrGpuTexture host{};
    REQUIRE(prGpuTextureCreate(device.gpuDevice, &desc, &host) == PrStatus_Ok);
    PrPixels texels{columns.data(), size_t(kSize) * 4, {kSize, kSize}};
    prGpuTextureWrite(device.gpuDevice, host, PrIVec2{0, 0}, prPixelsView(texels));
    PrDrawList* list = prDrawListCreate();
    PrDrawFilterNode invert = nodeOf(PrDrawFilterKind_ColorMatrix);
    invert.inputs[0] = PrDrawFilterInput_Backdrop;
    const float matrix[20] = {-1, 0, 0, 0, 1, 0, -1, 0, 0, 1, 0, 0, -1, 0, 1, 0, 0, 0, 1, 0};
    for (uint32_t i = 0; i < 20; ++i)
        invert.matrix.m[i / 5][i % 5] = matrix[i];
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{2, 2, 14, 14};
    group.backdrop = filterOf({invert});
    prDrawListPushGroup(list, &group);
    prDrawListPopGroup(list);
    prDrawFilterRelease(group.backdrop);

    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(device.gpuDevice, nullptr, &commands));
    PrDrawBeneath beneath{};
    beneath.kind = PrDrawBeneathKind_Texture;
    beneath.texture = prDrawGpuTexture(host);
    beneath.extent = prGpuTextureExtent(device.gpuDevice, host);
    PrDrawTarget prepared{.extent = {kSize, kSize},
                          .format = PrPixelFormat_Rgba8,
                          .commands = commands,
                          .beneath = &beneath};
    if (device.frame == nullptr)
        device.frame = prDrawFrameCreate(device.renderer);
    REQUIRE(prDrawFramePrepare(device.frame, list, &prepared) == PrStatus_Ok);
    copyTexture(commands, host, device.texture);
    PrGpuPassDesc passDesc = prGpuPassDescColor(device.texture, PrGpuLoad_Keep, PrColor{});
    PrGpuPass* pass = beginPass(commands, &passDesc);
    prDrawFrameRender(device.frame, pass, PrIVec2{0, 0});
    endPass(pass, &passDesc);
    prGpuCommandsSubmitAndWait(commands);
    std::vector<uint8_t> drawn(columns.size());
    PrPixels out{drawn.data(), size_t(kSize) * 4, {kSize, kSize}};
    prGpuTextureRead(device.gpuDevice, device.texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, out);

    std::vector<uint8_t> expected = columns;
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrPixels into{expected.data(), size_t(kSize) * 4, {kSize, kSize}};
    REQUIRE(prDrawRendererRenderPixels(cpu, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(cpu);
    CHECK(fractionDiffering(drawn, expected, 2) == 0);
    CHECK(int(drawn[(8 * kSize + 8) * 4]) == 255 - 200);
    prDrawListRelease(list);
    prGpuTextureDestroy(device.gpuDevice, host);
}

TEST_CASE("the fragment renderer reads what lies beneath a turned group, as a texture or drawn again") {
    // The host's columns, beneath a group turned a quarter turn about the center. The group's backdrop moves what lies
    // beneath along the group's x axis, which points down on the device.
    constexpr uint32_t kSize = 32;
    Device device(kSize, kSize);
    PrDrawList* columns = prDrawListCreate();
    for (uint32_t x = 0; x < kSize; x += 4) {
        PrColor color = (x / 4) % 2 == 0 ? PrColor{0.8f, 0.2f, 0.2f, 1} : PrColor{0.2f, 0.2f, 0.8f, 1};
        prDrawListRect(columns, PrRect{float(x), 0, float(x + 4), float(kSize / 2)}, {.color = color});
    }
    PrDrawList* list = prDrawListCreate();
    PrDrawFilterNode offset = nodeOf(PrDrawFilterKind_Offset);
    offset.inputs[0] = PrDrawFilterInput_Backdrop;
    offset.offset = PrVec2{6, 0};
    PrAffine2 turn = prAffine2Concat(
        prAffine2Concat(prAffine2Translate(PrVec2{-16, -16}), prAffine2Rotate(3.14159265f / 2)),
        prAffine2Translate(PrVec2{16, 16}));
    prDrawListPushTransformAffine2(list, turn);
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{4, 4, 28, 28};
    group.backdrop = filterOf({offset});
    prDrawListPushGroup(list, &group);
    prDrawListPopGroup(list);
    prDrawListPopTransform(list);
    prDrawFilterRelease(group.backdrop);

    // The host's texture of the columns, and a frame of another backend that redraws them.
    PrGpuTextureDesc desc = prGpuTextureDesc2d(prGpuFormatFromPixelFormat(PrPixelFormat_Rgba8), PrUVec2{kSize, kSize},
                                               PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    PrGpuTexture host{};
    PrGpuTexture second{};
    REQUIRE(prGpuTextureCreate(device.gpuDevice, &desc, &host) == PrStatus_Ok);
    REQUIRE(prGpuTextureCreate(device.gpuDevice, &desc, &second) == PrStatus_Ok);
    PrDrawRenderer* redraw = createRenderer(device.gpuDevice);
    PrDrawFrame* hostTarget = nullptr;
    PrDrawFrame* again = nullptr;
    PrDrawFrame* overRedraw = nullptr;
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(device.gpuDevice, nullptr, &commands));
    PrDrawTarget whole{.extent = {kSize, kSize},
                       .format = PrPixelFormat_Rgba8,
                       .commands = commands};
    if (hostTarget == nullptr)
        hostTarget = prDrawFrameCreate(redraw);
    REQUIRE(prDrawFramePrepare(hostTarget, columns, &whole) == PrStatus_Ok);
    if (again == nullptr)
        again = prDrawFrameCreate(redraw);
    REQUIRE(prDrawFramePrepare(again, columns, &whole) == PrStatus_Ok);
    PrGpuPassDesc hostPass = prGpuPassDescColor(host, PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
    PrGpuPass* pass = beginPass(commands, &hostPass);
    prDrawFrameRender(hostTarget, pass, PrIVec2{0, 0});
    endPass(pass, &hostPass);
    PrDrawBeneath texture{};
    texture.kind = PrDrawBeneathKind_Texture;
    texture.texture = prDrawGpuTexture(host);
    texture.extent = prGpuTextureExtent(device.gpuDevice, host);
    PrDrawBeneath drawn{};
    drawn.kind = PrDrawBeneathKind_Draw;
    drawn.draw = renderBeneath;
    drawn.context = again;
    whole.beneath = &texture;
    if (device.frame == nullptr)
        device.frame = prDrawFrameCreate(device.renderer);
    REQUIRE(prDrawFramePrepare(device.frame, list, &whole) == PrStatus_Ok);
    whole.beneath = &drawn;
    if (overRedraw == nullptr)
        overRedraw = prDrawFrameCreate(device.renderer);
    REQUIRE(prDrawFramePrepare(overRedraw, list, &whole) == PrStatus_Ok);
    PrDrawFrame* renders[2] = {device.frame, overRedraw};
    PrGpuTexture frames[2] = {device.texture, second};
    for (uint32_t i = 0; i < 2; ++i) {
        copyTexture(commands, host, frames[i]);
        PrGpuPassDesc framePass = prGpuPassDescColor(frames[i], PrGpuLoad_Keep, PrColor{});
        pass = beginPass(commands, &framePass);
        prDrawFrameRender(renders[i], pass, PrIVec2{0, 0});
        endPass(pass, &framePass);
    }
    prGpuCommandsSubmitAndWait(commands);
    std::vector<uint8_t> results[2];
    for (uint32_t i = 0; i < 2; ++i) {
        results[i].resize(size_t(kSize) * kSize * 4);
        PrPixels out{results[i].data(), size_t(kSize) * 4, {kSize, kSize}};
        prGpuTextureRead(device.gpuDevice, frames[i], PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, out);
    }

    std::vector<uint8_t> expected = reference(columns, kSize, kSize);
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrPixels into{expected.data(), size_t(kSize) * 4, {kSize, kSize}};
    REQUIRE(prDrawRendererRenderPixels(cpu, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(cpu);
    CHECK(results[0] == results[1]);
    CHECK(fractionDiffering(results[0], expected, 8) <= 0.01);
    // The columns reach below their own bottom, moved down by the backdrop.
    CHECK(results[0][(20 * kSize + 10) * 4 + 3] == 255);

    prDrawFrameDestroy(overRedraw);
    prDrawFrameDestroy(again);
    prDrawFrameDestroy(hostTarget);
    prDrawRendererDestroy(redraw);
    prGpuTextureDestroy(device.gpuDevice, second);
    prGpuTextureDestroy(device.gpuDevice, host);
    prDrawListRelease(list);
    prDrawListRelease(columns);
}

TEST_CASE("the fragment renderer reads a solid color beneath a render as the cpu backend reads pixels of it") {
    // A disc, and a backdrop that inverts what lies beneath it: both the disc and the color that the pass clears to.
    constexpr uint32_t kSize = 32;
    const PrColor color{0.9f, 0.6f, 0.1f, 1};
    Device device(kSize, kSize);
    PrDrawList* list = prDrawListCreate();
    prDrawListEllipse(list, PrVec2{12, 12}, PrVec2{8, 8}, 0, {.color = PrColor{0.1f, 0.2f, 0.8f, 1}});
    PrDrawFilterNode invert = nodeOf(PrDrawFilterKind_ColorMatrix);
    invert.inputs[0] = PrDrawFilterInput_Backdrop;
    const float matrix[20] = {-1, 0, 0, 0, 1, 0, -1, 0, 0, 1, 0, 0, -1, 0, 1, 0, 0, 0, 1, 0};
    for (uint32_t i = 0; i < 20; ++i)
        invert.matrix.m[i / 5][i % 5] = matrix[i];
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{8, 8, 28, 28};
    group.backdrop = filterOf({invert});
    prDrawListPushGroup(list, &group);
    prDrawListPopGroup(list);
    prDrawFilterRelease(group.backdrop);

    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(device.gpuDevice, nullptr, &commands));
    PrDrawBeneath beneath{};
    beneath.color = color;
    PrDrawTarget prepared{.extent = {kSize, kSize},
                          .format = PrPixelFormat_Rgba8,
                          .commands = commands,
                          .beneath = &beneath};
    if (device.frame == nullptr)
        device.frame = prDrawFrameCreate(device.renderer);
    REQUIRE(prDrawFramePrepare(device.frame, list, &prepared) == PrStatus_Ok);
    PrGpuPassDesc desc = prGpuPassDescColor(device.texture, PrGpuLoad_Clear,
                                            PrColor{color.r, color.g, color.b, color.a});
    PrGpuPass* pass = beginPass(commands, &desc);
    prDrawFrameRender(device.frame, pass, PrIVec2{0, 0});
    endPass(pass, &desc);
    prGpuCommandsSubmitAndWait(commands);
    std::vector<uint8_t> drawn(size_t(kSize) * kSize * 4);
    PrPixels out{drawn.data(), size_t(kSize) * 4, {kSize, kSize}};
    prGpuTextureRead(device.gpuDevice, device.texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, out);

    std::vector<uint8_t> expected(drawn.size());
    for (size_t i = 0; i < expected.size(); i += 4) {
        expected[i] = drawn[0];
        expected[i + 1] = drawn[1];
        expected[i + 2] = drawn[2];
        expected[i + 3] = drawn[3];
    }
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrPixels into{expected.data(), size_t(kSize) * 4, {kSize, kSize}};
    REQUIRE(prDrawRendererRenderPixels(cpu, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
    prDrawRendererDestroy(cpu);
    // The disc's edge differs by the backends' coverage, in the same way as for fills.
    CHECK(fractionDiffering(drawn, expected, 12) == 0);
    // Beneath the backdrop and beside the disc, the color is inverted.
    CHECK(int(drawn[(24 * kSize + 24) * 4 + 2]) > 200);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer composites a knockout group's children against its initial state") {
    Device device(64, 64);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 64, 64}, {.color = PrColor{0.9f, 0.9f, 0.8f, 1}});
    PrDrawGroup group = prDrawGroupDefault();
    group.knockout = true;
    prDrawListPushGroup(list, &group);
    prDrawListEllipse(list, PrVec2{24, 28}, PrVec2{16, 16}, 0, {.color = PrColor{0, 0, 1, 0.5f}});
    prDrawListEllipse(list, PrVec2{40, 36}, PrVec2{16, 16}, 0, {.color = PrColor{1, 0, 0, 0.5f}});
    prDrawListPopGroup(list);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 64, 64);
    CHECK(fractionDiffering(drawn, expected, 8) <= 0.01);
    // Where the shapes overlap, only the later shape covers the group's initial state, so the result is half the
    // background's blue.
    const uint8_t* overlap = drawn.data() + (size_t(32) * 64 + 32) * 4;
    CHECK(std::abs(int(overlap[2]) - 102) <= 2);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer fills paths alike whether tessellated, in strips or in tiles") {
    PrDrawList* list = prDrawListCreate();
    // Overlapping contours under each rule, curves, a thin stroke and a path crossing the target's edge.
    PrPath* star = prPathCreate();
    for (int i = 0; i < 5; ++i) {
        float angle = -1.5708f + float(i) * 4 * 3.14159f / 5;
        PrVec2 p{40 + 34 * std::cos(angle), 40 + 34 * std::sin(angle)};
        if (i == 0)
            prPathMoveTo(star, p);
        else
            prPathLineTo(star, p);
    }
    prPathClose(star);
    prDrawListPath(list, star, PrPathFillRule_EvenOdd, {.color = PrColor{0.8f, 0.1f, 0.1f, 1}});
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{70, 3.3f}));
    prDrawListPath(list, star, PrPathFillRule_NonZero, {.color = PrColor{0.1f, 0.2f, 0.8f, 0.7f}});
    prDrawListPopTransform(list);
    PrPath* wave = prPathCreate();
    prPathMoveTo(wave, PrVec2{-10, 100});
    prPathCubicTo(wave, PrVec2{30, 60}, PrVec2{60, 140}, PrVec2{150, 96.5f});
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 1.5f;
    stroke.shape.join = PrPathJoin_Round;
    stroke.shape.startCap = stroke.shape.endCap = stroke.shape.dashCap = PrPathCap_Round;
    prDrawListPath(list, wave, PrPathFillRule_NonZero, {.color = PrColor{0, 0.5f, 0.2f, 1}, .stroke = &stroke});
    std::vector<uint8_t> expected = reference(list, 144, 112);
    PrDrawFragmentPaths modes[] = {PrDrawFragmentPaths_Tessellate, PrDrawFragmentPaths_Strips,
                                   PrDrawFragmentPaths_Tiles};
    for (PrDrawFragmentPaths paths : modes) {
        Device device(144, 112, paths);
        std::vector<uint8_t> drawn = device.render(list);
        INFO("paths " << int(paths));
        CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.005));
        // Strips and tiles compute the rasterizer's coverage. Tessellation flattens curves more coarsely, and the
        // miters of its fringe cover a little more at sharp points.
        CHECK(fractionDiffering(drawn, expected, paths == PrDrawFragmentPaths_Tessellate ? 40 : 16) <= 0.005);
    }
    prPathDestroy(wave);
    prPathDestroy(star);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer draws glyphs from the atlas as the cpu backend fills them") {
    const PrFont* font = load("NotoSans.ttf");
    PrDrawList* list = prDrawListCreate();
    const char* text = "Handgloves";
    std::vector<PrFontGlyph> glyphs;
    float x = 4.3f;
    for (const char* c = text; *c != 0; ++c) {
        uint16_t id = prFontCodePointGlyph(font, uint32_t(*c));
        glyphs.push_back(PrFontGlyph{id, {x, 30.6f}});
        x += prFontGlyphAdvance(font, id, 0) * 22;
    }
    PrFontGlyphRun run{};
    run.font = font;
    run.size = 22;
    run.glyphs = glyphs.data();
    run.glyphCount = uint32_t(glyphs.size());
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0.1f, 0.1f, 0.1f, 1}});
    Device device(128, 40);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 128, 40);
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.01));
    // Glyphs land within an eighth of a pixel of where they are drawn.
    CHECK(fractionDiffering(drawn, expected, 48) <= 0.01);
    prDrawListRelease(list);
    prFontRelease(font);
}

TEST_CASE("the fragment renderer draws small paths from the atlas as the cpu backend fills and strokes them") {
    // Icons on a grid of 24 units, at several sizes and offsets within a pixel. The last offset is between quarter
    // pixels.
    PrPath* check = prPathCreate();
    prPathMoveTo(check, PrVec2{20, 6});
    prPathLineTo(check, PrVec2{9, 17});
    prPathLineTo(check, PrVec2{4, 12});
    PrPath* circle = prPathCreate();
    prPathAddEllipse(circle, PrVec2{12, 12}, PrVec2{9, 9}, 0);
    PrPath* star = prPathCreate();
    for (int i = 0; i < 5; ++i) {
        float angle = -1.5708f + float(i) * 4 * 3.14159f / 5;
        PrVec2 p{12 + 10 * std::cos(angle), 12 + 10 * std::sin(angle)};
        if (i == 0)
            prPathMoveTo(star, p);
        else
            prPathLineTo(star, p);
    }
    prPathClose(star);
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 2;
    stroke.shape.join = PrPathJoin_Round;
    stroke.shape.startCap = stroke.shape.endCap = PrPathCap_Round;
    PrDrawStroke inside = stroke;
    inside.align = PrDrawStrokeAlign_Inside;
    PrDrawList* list = prDrawListCreate();
    const float sizes[] = {12, 16, 20, 24, 37};
    const PrVec2 offsets[] = {{0, 0}, {0.25f, 0.5f}, {0.75f, 0.25f}, {0.4f, 0.9f}};
    float y = 2;
    for (PrVec2 offset : offsets) {
        float x = 2;
        for (float size : sizes) {
            PrAffine2 scale = prAffine2Scale(PrVec2{size / 24, size / 24});
            prDrawListPushTransformAffine2(
                list, prAffine2Concat(scale, prAffine2Translate(PrVec2{x + offset.x, y + offset.y})));
            prDrawListPath(list, check, PrPathFillRule_NonZero,
                           {.color = PrColor{0.1f, 0.1f, 0.1f, 1}, .stroke = &stroke});
            prDrawListPopTransform(list);
            prDrawListPushTransformAffine2(
                list, prAffine2Concat(scale, prAffine2Translate(PrVec2{x + offset.x, y + 40 + offset.y})));
            prDrawListPath(list, circle, PrPathFillRule_NonZero,
                           {.color = PrColor{0.1f, 0.4f, 0.8f, 1}, .stroke = &inside});
            prDrawListPath(list, star, PrPathFillRule_EvenOdd, {.color = PrColor{0.8f, 0.2f, 0.1f, 0.7f}});
            prDrawListPopTransform(list);
            x += size + 6;
        }
        y += 80;
    }
    std::vector<uint8_t> expected = reference(list, 128, 320);
    Device device(128, 320, PrDrawFragmentPaths_Tiles);
    // The second frame draws the coverage that the first frame rasterized.
    for (int frame = 0; frame < 2; ++frame) {
        std::vector<uint8_t> drawn = device.render(list);
        INFO("frame " << frame);
        CHECK(prDrawFrameStats(device.frame).pathFills == (frame == 0 ? 60u : 0u));
        CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.005));
        // Paths land within an eighth of a pixel of where they are drawn, and exactly at quarter pixels.
        CHECK(fractionDiffering(drawn, expected, 48) <= 0.01);
    }
    prDrawListRelease(list);
    prPathDestroy(star);
    prPathDestroy(circle);
    prPathDestroy(check);
}

TEST_CASE("the fragment renderer samples images as the cpu backend does, by every sampling, tile mode and tile flag") {
    // A gradient of texels with a hard diagonal, which each sampling filters differently.
    std::vector<uint8_t> texels(12 * 10 * 4);
    for (uint32_t y = 0; y < 10; ++y) {
        for (uint32_t x = 0; x < 12; ++x) {
            uint8_t* t = texels.data() + (y * 12 + x) * 4;
            t[0] = uint8_t(x * 21);
            t[1] = uint8_t(y * 25);
            t[2] = x > y ? 200 : 20;
            t[3] = 255;
        }
    }
    PrDrawImageDesc desc{PrPixelView{texels.data(), 48, {12, 10}}, PrPixelFormat_Rgba8, PrDrawAlpha_Premultiplied,
                         PrColorSpace_Srgb};
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    Device device(96, 96);
    // Each case samples a part of the image, which texel reads serve, and the whole image, which the samplers serve, as
    // spaced tiles and as tiles that abut.
    struct Case {
        PrRect texels;
        PrVec2 spacing;
    };
    const Case cases[] = {{PrRect{1, 1, 11, 9}, PrVec2{2, 3}}, {PrRect{0, 0, 12, 10}, PrVec2{2, 3}},
                          {PrRect{0, 0, 12, 10}, PrVec2{0, 0}}};
    const PrDrawTileFlags kFlagSets[] = {0, PrDrawTileFlag_Clamped};
    for (PrDrawSampling sampling :
         {PrDrawSampling_Nearest, PrDrawSampling_Linear, PrDrawSampling_Cubic, PrDrawSampling_Mipmap}) {
        for (PrDrawExtend mode :
             {PrDrawExtend_None, PrDrawExtend_Pad, PrDrawExtend_Repeat, PrDrawExtend_Reflect}) {
            for (PrDrawTileFlags flags : kFlagSets) {
                for (const Case& c : cases) {
                    CAPTURE(int(sampling));
                    CAPTURE(int(mode));
                    CAPTURE(int(flags));
                    CAPTURE(c.texels.left);
                    CAPTURE(c.spacing.x);
                    PrDrawList* list = prDrawListCreate();
                    PrDrawBrush brush = prDrawBrushDefault(PrDrawBrushKind_Image);
                    // Magnified along x, and minified along y, so that mipmaps apply in y.
                    brush.transform = prAffine2Concat(prAffine2Scale(PrVec2{1.7f, 0.3f}), prAffine2Rotate(0.2f));
                    brush.image.source = prDrawSourceImage(image, c.texels);
                    brush.image.tiles.tile = PrRect{3, 2, 23, 18};
                    brush.image.tiles.spacing = c.spacing;
                    brush.image.tiles.x = brush.image.tiles.y = mode;
                    brush.image.tiles.flags = flags;
                    brush.image.sampling = sampling;
                    prDrawListRect(list, PrRect{0, 0, 96, 96}, {.brush = &brush});
                    std::vector<uint8_t> drawn = device.render(list);
                    std::vector<uint8_t> expected = reference(list, 96, 96);
                    // The hardware's anisotropic filter picks its levels and taps by its own approximation of the
                    // footprint. The cpu backend takes the footprint's minor axis, with even taps along its major axis.
                    CHECK(fractionDiffering(drawn, expected, sampling == PrDrawSampling_Mipmap ? 32 : 2) <= 0.002);
                    prDrawListRelease(list);
                }
            }
        }
    }
    prDrawImageRelease(image);
}

TEST_CASE("the fragment renderer samples a sub-rectangle with a gutter through the hardware, as the cpu backend samples its taps") {
    // A motif of 8 texels repeated over 32: the texels around the middle 8 continue its tiles for Repeat, four texels
    // wide, which covers the bilinear and cubic filters and the first two mip levels.
    std::vector<uint8_t> texels(32 * 32 * 4);
    for (uint32_t y = 0; y < 32; ++y) {
        for (uint32_t x = 0; x < 32; ++x) {
            uint8_t* t = texels.data() + (y * 32 + x) * 4;
            t[0] = uint8_t((x % 8) * 30);
            t[1] = uint8_t((y % 8) * 30);
            t[2] = (x % 8) > (y % 8) ? 200 : 20;
            t[3] = 255;
        }
    }
    PrDrawImageDesc desc{PrPixelView{texels.data(), 128, {32, 32}}, PrPixelFormat_Rgba8, PrDrawAlpha_Premultiplied,
                         PrColorSpace_Srgb};
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    Device device(96, 96);
    for (PrDrawSampling sampling : {PrDrawSampling_Linear, PrDrawSampling_Cubic, PrDrawSampling_Mipmap}) {
        CAPTURE(int(sampling));
        PrDrawList* list = prDrawListCreate();
        PrDrawBrush brush = prDrawBrushDefault(PrDrawBrushKind_Image);
        brush.transform = prAffine2Concat(prAffine2Scale(PrVec2{1.7f, 0.6f}), prAffine2Rotate(0.2f));
        brush.image.source = prDrawSourceImage(image, PrRect{8, 8, 16, 16});
        brush.image.tiles.tile = PrRect{3, 2, 19, 18};
        brush.image.tiles.x = brush.image.tiles.y = PrDrawExtend_Repeat;
        brush.image.sampling = PrDrawSampling(sampling | PrDrawSamplingFlag_Gutter);
        prDrawListRect(list, PrRect{0, 0, 96, 96}, {.brush = &brush});
        std::vector<uint8_t> drawn = device.render(list);
        std::vector<uint8_t> expected = reference(list, 96, 96);
        // As for the whole image, the hardware's anisotropic filter differs from cpu's in its levels and taps.
        CHECK(fractionDiffering(drawn, expected, sampling == PrDrawSampling_Mipmap ? 32 : 2) <= 0.002);
        prDrawListRelease(list);
    }
    prDrawImageRelease(image);
}

TEST_CASE("the fragment renderer maps a brush along strokes by each pixel's nearest point of the centerline") {
    PrDrawList* list = prDrawListCreate();
    // Closed contours that end on their start, as font contours do, and one contour that retraces an edge.
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{10, 10});
    prPathLineTo(path, PrVec2{50, 12});
    prPathQuadTo(path, PrVec2{60, 40}, PrVec2{30, 50});
    prPathLineTo(path, PrVec2{10, 10});
    prPathClose(path);
    prPathMoveTo(path, PrVec2{70, 10});
    prPathLineTo(path, PrVec2{90, 50});
    prPathLineTo(path, PrVec2{70, 10});
    prPathClose(path);
    PrDrawColorStop stops[3] = {{.offset = 0, .color = PrColor{1, 0, 0, 1}},
                                {.offset = 0.5f, .color = PrColor{0, 1, 0, 1}},
                                {.offset = 1, .color = PrColor{0, 0, 1, 1}}};
    PrDrawBrush along = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{1, 0}, stops, 3);
    PrDrawStroke stroke = prDrawStrokeDefault();
    stroke.shape.width = 6;
    stroke.shape.join = PrPathJoin_Round;
    stroke.shape.startCap = stroke.shape.endCap = stroke.shape.dashCap = PrPathCap_Round;
    stroke.mapping = PrDrawStrokeMapping_Along;
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.brush = &along, .stroke = &stroke});
    // Across, with a width profile.
    PrPathWidthPoint profile[2] = {{0, 0.2f, 0.2f}, {1, 0.8f, 0.4f}};
    stroke.shape.profile = profile;
    stroke.shape.profileCount = 2;
    stroke.mapping = PrDrawStrokeMapping_Across;
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{0, 50}));
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.brush = &along, .stroke = &stroke});
    prDrawListPopTransform(list);
    std::vector<uint8_t> expected = reference(list, 100, 110);
    // Sparse coverage matches the rasterizer's coverage, so any difference comes from the mapping.
    for (PrDrawFragmentPaths paths : {PrDrawFragmentPaths_Strips, PrDrawFragmentPaths_Tiles}) {
        CAPTURE(int(paths));
        Device device(100, 110, paths);
        std::vector<uint8_t> drawn = device.render(list);
        CHECK(fractionDiffering(drawn, expected, 16) <= 0.001);
    }
    prPathDestroy(path);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer draws a list source as a texture at its scale in target pixels, as a brush and as a mask") {
    const PrFont* font = load("NotoSans.ttf");
    PrDrawList* maskList = prDrawListCreate();
    PrFontGlyph glyphs[1] = {{prFontCodePointGlyph(font, 'H'), {30, 70}}};
    PrFontGlyphRun run{};
    run.font = font;
    run.size = 60;
    run.glyphs = glyphs;
    run.glyphCount = 1;
    prDrawListGlyphs(maskList, &run, PrVec2{}, {.color = PrColor{1, 1, 1, 1}});
    PrRect photo{20, 20, 90, 90};
    PrDrawSource maskSource = prDrawSourceList(maskList, photo);
    PrDrawBrush maskBrush = prDrawBrushImage(maskSource, photo, PrDrawSampling_Linear);
    PrDrawMask mask = prDrawMaskDefault(&maskBrush);
    PrDrawGroup masked = prDrawGroupDefault();
    masked.mask = &mask;
    masked.bounds = photo;
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{1.3f, 1.3f}));
    prDrawListPushGroup(list, &masked);
    std::vector<uint8_t> texels(8 * 8 * 4, 200);
    PrDrawImageDesc desc{PrPixelView{texels.data(), 32, {8, 8}}, PrPixelFormat_Rgba8, PrDrawAlpha_Premultiplied,
                         PrColorSpace_Srgb};
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    PrDrawSource source = prDrawSourceImage(image, PrRect{0, 0, 8, 8});
    PrDrawBrush brush = prDrawBrushImage(source, photo, PrDrawSampling_Cubic);
    prDrawListRect(list, photo, {.brush = &brush});
    prDrawImageRelease(image);
    prDrawListPopGroup(list);
    prDrawListPopTransform(list);
    Device device(100, 100);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 100, 100);
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.01));
    CHECK(fractionDiffering(drawn, expected, 8) <= 0.01);
    // A brush of the list on a plain rect.
    PrDrawList* plain = prDrawListCreate();
    prDrawListPushTransformAffine2(plain, prAffine2Scale(PrVec2{1.3f, 1.3f}));
    prDrawListRect(plain, photo, {.brush = &mask.brush});
    prDrawListPopTransform(plain);
    std::vector<uint8_t> drawn2 = device.render(plain);
    std::vector<uint8_t> expected2 = reference(plain, 100, 100);
    CHECK(inked(drawn2) == doctest::Approx(inked(expected2)).epsilon(0.01));
    CHECK(fractionDiffering(drawn2, expected2, 8) <= 0.01);
    prDrawListRelease(plain);
    prDrawListRelease(list);
    prDrawListRelease(maskList);
    prFontRelease(font);
}

TEST_CASE("the fragment renderer filters a rotated group in the group's own space") {
    PrDrawList* list = prDrawListCreate();
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.5f), prAffine2Translate(PrVec2{64, 64})));
    PrDrawFilterNode blur = nodeOf(PrDrawFilterKind_Blur);
    blur.blur.sigma = PrVec2{6, 0};
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{-60, -60, 60, 60};
    group.filter = filterOf({blur});
    prDrawListPushGroup(list, &group);
    prDrawFilterRelease(group.filter);
    prDrawListRect(list, PrRect{-25, -25, 25, 25}, {.color = PrColor{0.5f, 0.2f, 0.8f, 1}});
    prDrawListPopGroup(list);
    prDrawListPopTransform(list);
    Device device(128, 128);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 128, 128);
    // The blur runs along the group's x axis, including the part of its space left of its origin.
    CHECK(inked(drawn) == doctest::Approx(inked(expected)).epsilon(0.005));
    CHECK(fractionDiffering(drawn, expected, 16) <= 0.002);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer draws a group whose filter reaches far at a lower resolution, as cpu does") {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode blur{};
    blur.kind = PrDrawFilterKind_Blur;
    blur.inputs[0] = PrDrawFilterInput_Source;
    blur.blur.sigma = PrVec2{2000, 2000};
    prDrawFilterBuilderAdd(builder, &blur);
    const PrDrawFilter* filter = nullptr;
    filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    PrDrawList* list = prDrawListCreate();
    PrDrawGroup group = prDrawGroupDefault();
    group.filter = filter;
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, PrRect{0, 0, 64, 64}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopGroup(list);
    Device device(64, 64);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 64, 64);
    CHECK(fractionDiffering(drawn, expected, 4) <= 0.01);
    prDrawListRelease(list);
    prDrawFilterRelease(filter);
}

TEST_CASE("the fragment renderer composites a projected group's layer through the projection, as the cpu backend does") {
    Device device(128, 128);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 128, 128}, {.color = PrColor{0.9f, 0.9f, 0.8f, 1}});
    // A plane tilted back, whose far part samples smaller mip levels.
    PrMatrix4 tilted = tiltedPlane(1.1f, 90, PrVec2{36, 40});
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{-30, -30, 30, 30};
    group.projection = &tilted;
    prDrawListPushGroup(list, &group);
    planeContent(list, group.bounds);
    prDrawListPopGroup(list);
    // A plane partly behind the eye, translucent and blended by multiply through a clip.
    PrMatrix4 steep = tiltedPlane(1.0f, 20, PrVec2{96, 36});
    PrDrawGroup cut = prDrawGroupDefault();
    cut.bounds = PrRect{-16, -60, 16, 60};
    cut.projection = &steep;
    cut.opacity = 0.75f;
    cut.blendMode = PrDrawBlendMode_Multiply;
    prDrawListPushClipRect(list, PrRect{70, 0, 128, 64}, PrDrawEdges_Antialiased);
    prDrawListPushGroup(list, &cut);
    planeContent(list, cut.bounds);
    prDrawListPopGroup(list);
    prDrawListPopClip(list);
    // A projected group inside another, under a rotation.
    PrMatrix4 outer = tiltedPlane(0.6f, 120, PrVec2{0, 0});
    PrMatrix4 inner = tiltedPlane(-0.5f, 150, PrVec2{4, -4});
    prDrawListPushTransformAffine2(list, prAffine2Concat(prAffine2Rotate(0.3f), prAffine2Translate(PrVec2{64, 96})));
    PrDrawGroup outerGroup = prDrawGroupDefault();
    outerGroup.bounds = PrRect{-28, -28, 28, 28};
    outerGroup.projection = &outer;
    prDrawListPushGroup(list, &outerGroup);
    prDrawListRect(list, outerGroup.bounds, {.color = PrColor{0.2f, 0.6f, 0.3f, 1}});
    PrDrawGroup innerGroup = prDrawGroupDefault();
    innerGroup.bounds = PrRect{-16, -16, 16, 16};
    innerGroup.projection = &inner;
    prDrawListPushGroup(list, &innerGroup);
    planeContent(list, innerGroup.bounds);
    prDrawListPopGroup(list);
    prDrawListPopGroup(list);
    prDrawListPopTransform(list);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 128, 128);
    CHECK(fractionDiffering(drawn, expected, 16) <= 0.002);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer composites only the part of a projected group's layer that its contents drew") {
    // Contents that stop short of the bounds, at fractional edges, over an opaque background. The composite ends where
    // the contents end, so their edge fades as the cpu backend fades it.
    Device device(96, 96);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 96, 96}, {.color = PrColor{0.9f, 0.9f, 0.8f, 1}});
    PrMatrix4 tilted = tiltedPlane(0.7f, 120, PrVec2{48, 48});
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = PrRect{-40, -40, 40, 40};
    group.projection = &tilted;
    prDrawListPushGroup(list, &group);
    planeContent(list, PrRect{-20.4f, -15.6f, 19.7f, 24.3f});
    prDrawListPopGroup(list);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 96, 96);
    CHECK(fractionDiffering(drawn, expected, 8) <= 0.002);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer reads what lies beneath through a projected group's plane, as the cpu backend does") {
    PrDrawFilterNode offset = nodeOf(PrDrawFilterKind_Offset);
    offset.inputs[0] = PrDrawFilterInput_Backdrop;
    offset.offset = PrVec2{16, 0};
    const PrDrawFilter* filter = filterOf({offset});
    PrMatrix4 seen = tiltedPlane(0.4f, 200, PrVec2{32, 32});
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 32, 64}, {.color = PrColor{1, 0, 0, 1}});
    PrDrawGroup projected = prDrawGroupDefault();
    projected.bounds = PrRect{-32, -32, 32, 32};
    projected.projection = &seen;
    projected.backdrop = filter;
    prDrawListPushGroup(list, &projected);
    prDrawListRect(list, PrRect{-32, 0, 0, 32}, {.color = PrColor{0, 1, 0, 1}});
    PrDrawGroup inner = prDrawGroupDefault();
    inner.bounds = projected.bounds;
    inner.backdrop = filter;
    prDrawListPushGroup(list, &inner);
    prDrawListPopGroup(list);
    prDrawListPopGroup(list);
    Device device(64, 64);
    std::vector<uint8_t> drawn = device.render(list);
    std::vector<uint8_t> expected = reference(list, 64, 64);
    // The hardware's trilinear filter may blend in the next level at a hard edge where cpu reads the first level alone.
    CHECK(fractionDiffering(drawn, expected, 32) <= 0.002);
    // Red beneath the plane shows on the plane's right, where both backdrops move it.
    CHECK(fractionDiffering(drawn, std::vector<uint8_t>(drawn.size(), 0), 16) > 0.5);
    prDrawListRelease(list);

    // A plane drawn at twice its size reads what lies beneath in its own units.
    PrMatrix4 doubled = prMatrix4Scale(PrVec3{2, 2, 1});
    list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 16, 64}, {.color = PrColor{1, 0, 0, 1}});
    PrDrawGroup plane = prDrawGroupDefault();
    plane.bounds = PrRect{0, 0, 32, 32};
    plane.projection = &doubled;
    prDrawListPushGroup(list, &plane);
    prDrawListPushGroup(list, &inner);
    prDrawListPopGroup(list);
    prDrawListPopGroup(list);
    drawn = device.render(list);
    expected = reference(list, 64, 64);
    CHECK(fractionDiffering(drawn, expected, 2) <= 0.002);
    prDrawListRelease(list);
    prDrawFilterRelease(filter);
}

TEST_CASE("the fragment renderer draws a kept embed, in place and moved by whole pixels, as it draws the embed walked") {
    const PrFont* font = load("NotoSans.ttf");
    std::vector<PrFontGlyph> glyphs;
    float x = 2.3f;
    for (const char* c = "Kept"; *c != 0; ++c) {
        uint16_t id = prFontCodePointGlyph(font, uint32_t(*c));
        glyphs.push_back(PrFontGlyph{id, {x, 14.6f}});
        x += prFontGlyphAdvance(font, id, 0) * 14;
    }
    PrFontGlyphRun run{};
    run.font = font;
    run.size = 14;
    run.glyphs = glyphs.data();
    run.glyphCount = uint32_t(glyphs.size());
    PrDrawList* inner = prDrawListCreate();
    prDrawListRect(inner, PrRect{0, 20, 30, 26}, {.color = PrColor{0.8f, 0.2f, 0.1f, 1}});
    prDrawListGlyphs(inner, &run, PrVec2{}, {.color = PrColor{0.1f, 0.1f, 0.1f, 1}});
    PrDrawList* content = prDrawListCreate();
    prDrawListRect(content, PrRect{0, 0, 60, 40}, {.color = PrColor{0.9f, 0.9f, 0.8f, 1}});
    PrCornerRect rounded = prCornerRectRound(PrRect{36, 4, 56, 24}, 6);
    prDrawListCornerRect(content, rounded, {.color = PrColor{0.2f, 0.4f, 0.9f, 1}});
    prDrawListPushClipRect(content, PrRect{0, 0, 44, 40}, PrDrawEdges_Antialiased);
    prDrawListEmbed(content, inner, PrDVec2{0, 0});
    prDrawListPopClip(content);
    // Frames that embed the same list where the last frame did, moved by whole pixels, moved by part of a pixel, partly
    // past the clip and back, and at another scale.
    struct Place {
        PrVec2 at;
        float scale;
    };
    const Place places[] = {{{10, 10}, 1}, {{10, 10}, 1}, {{13, 6}, 1}, {{13, 6}, 1}, {{13.5f, 6}, 1},
                            {{13.5f, 6}, 1}, {{120, 50}, 1}, {{120, 50}, 1}, {{20, 20}, 1}, {{20, 20}, 2}};
    Device kept(160, 80);
    Device walked(160, 80, PrDrawFragmentPaths_Tessellate, PrDrawFragmentEmbeds_Walk);
    for (const Place& place : places) {
        PrDrawList* frame = prDrawListCreate();
        prDrawListPushClipRect(frame, PrRect{4, 4, 150, 76}, PrDrawEdges_Antialiased);
        prDrawListPushTransformAffine2(frame, prAffine2Concat(prAffine2Scale(PrVec2{place.scale, place.scale}),
                                                              prAffine2Translate(place.at)));
        prDrawListEmbed(frame, content, PrDVec2{0, 0});
        prDrawListPopTransform(frame);
        prDrawListPopClip(frame);
        CHECK(kept.render(frame) == walked.render(frame));
        prDrawListRelease(frame);
    }
    prDrawListRelease(content);
    prDrawListRelease(inner);
    prFontRelease(font);
}

TEST_CASE("the fragment renderer keeps colors past white in a half-float target, as drawn") {
    // A bright solid, a gradient up to three times white, and a half-float image of twice white, each over a column of
    // the target.
    constexpr uint32_t kWidth = 48;
    constexpr uint32_t kHeight = 8;
    const PrColor bright{4, 2, 0.5f, 1};
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 16, 8}, {.color = bright});
    PrDrawColorStop stops[2] = {{.offset = 0, .color = PrColor{3, 3, 3, 1}},
                                {.offset = 1, .color = PrColor{3, 3, 3, 1}}};
    PrDrawBrush gradient = prDrawBrushLinear(PrVec2{16, 0}, PrVec2{32, 0}, stops, 2);
    prDrawListRect(list, PrRect{16, 0, 32, 8}, {.brush = &gradient});
    PrFloat16 texels[4 * 4];
    for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t k = 0; k < 4; ++k)
            texels[i * 4 + k] = prFloat16FromFloat(k < 3 ? 2.0f : 1.0f);
    }
    PrDrawImageDesc desc{.pixels = PrPixelView{reinterpret_cast<const uint8_t*>(texels), 16, {2, 2}},
                         .format = PrPixelFormat_RgbaF16,
                         .alpha = PrDrawAlpha_Premultiplied,
                         .space = PrColorSpace_Srgb};
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    PrDrawBrush brush = prDrawBrushImage(prDrawSourceImage(image, PrRect{0, 0, 2, 2}), PrRect{32, 0, 48, 8},
                                         PrDrawSampling_Nearest);
    prDrawListRect(list, PrRect{32, 0, 48, 8}, {.brush = &brush});

    PrGpuDevice* gpuDevice = createDevice();
    PrDrawRenderer* renderer = createRenderer(gpuDevice);
    PrGpuTextureDesc textureDesc =
        prGpuTextureDesc2d(prGpuFormatFromPixelFormat(PrPixelFormat_RgbaF16), PrUVec2{kWidth, kHeight},
                           PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    PrGpuTexture texture{};
    REQUIRE(prGpuTextureCreate(gpuDevice, &textureDesc, &texture) == PrStatus_Ok);
    PrDrawFrame* frame = prDrawFrameCreate(renderer);
    // Returns the color of the pixel at the center of each column.
    auto render = [&](PrColor out[3]) {
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(gpuDevice, nullptr, &commands));
        PrDrawTarget target{.extent = {kWidth, kHeight},
                            .format = PrPixelFormat_RgbaF16,
                            .commands = commands};
        REQUIRE(prDrawFramePrepare(frame, list, &target) == PrStatus_Ok);
        PrGpuPassDesc pass = prGpuPassDescColor(texture, PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
        PrGpuPass* encoder = beginPass(commands, &pass);
        prDrawFrameRender(frame, encoder, PrIVec2{0, 0});
        endPass(encoder, &pass);
        prGpuCommandsSubmitAndWait(commands);
        std::vector<PrFloat16> pixels(size_t(kWidth) * kHeight * 4);
        PrPixels read{reinterpret_cast<uint8_t*>(pixels.data()), size_t(kWidth) * 8, {kWidth, kHeight}};
        prGpuTextureRead(gpuDevice, texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0}, read);
        for (uint32_t column = 0; column < 3; ++column) {
            const PrFloat16* p = pixels.data() + (size_t(4) * kWidth + column * 16 + 8) * 4;
            out[column] = PrColor16{p[0], p[1], p[2], p[3]};
        }
    };
    auto near = [](PrColor a, PrColor b) {
        return std::abs(a.r - b.r) < 0.01f && std::abs(a.g - b.g) < 0.01f && std::abs(a.b - b.b) < 0.01f
               && std::abs(a.a - b.a) < 0.01f;
    };
    PrColor colors[3];
    render(colors);
    CHECK(near(colors[0], bright));
    CHECK(near(colors[1], PrColor{3, 3, 3, 1}));
    CHECK(near(colors[2], PrColor{2, 2, 2, 1}));
    // An image brush with a limit scales its texels down to it in linear light.
    prDrawListClear(list);
    brush.image.limit = 1.5f;
    prDrawListRect(list, PrRect{32, 0, 48, 8}, {.brush = &brush});
    render(colors);
    float limit = 1.055f * std::pow(1.5f, 1 / 2.4f) - 0.055f;
    CHECK(near(colors[2], PrColor{limit, limit, limit, 1}));
    prDrawFrameDestroy(frame);
    prDrawRendererDestroy(renderer);
    prGpuTextureDestroy(gpuDevice, texture);
    prGpuDeviceDestroy(gpuDevice);
    prDrawImageRelease(image);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer renders into a half-float target without a layer of its own") {
    PrGpuDevice* gpuDevice = createDevice();
    PrDrawRenderer* renderer = createRenderer(gpuDevice);
    PrGpuTextureDesc textureDesc =
        prGpuTextureDesc2d(prGpuFormatFromPixelFormat(PrPixelFormat_RgbaF16), PrUVec2{16, 16},
                           PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    PrGpuTexture texture{};
    REQUIRE(prGpuTextureCreate(gpuDevice, &textureDesc, &texture) == PrStatus_Ok);
    PrDrawList* list = prDrawListCreate();
    prDrawListRect(list, PrRect{0, 0, 16, 16}, {.color = PrColor{2, 0.6f, 0.4f, 1}});
    PrDrawFrame* frame = prDrawFrameCreate(renderer);
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(gpuDevice, nullptr, &commands));
    PrDrawTarget target{.extent = {16, 16},
                        .format = PrPixelFormat_RgbaF16,
                        .commands = commands};
    REQUIRE(prDrawFramePrepare(frame, list, &target) == PrStatus_Ok);
    PrGpuPassDesc pass = prGpuPassDescColor(texture, PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
    PrGpuPass* encoder = beginPass(commands, &pass);
    prDrawFrameRender(frame, encoder, PrIVec2{0, 0});
    endPass(encoder, &pass);
    prGpuCommandsSubmitAndWait(commands);
    CHECK(prDrawFrameStats(frame).layers == 0);
    // A target of 10-bit channels is refused.
    PrGpuCommands* refused = nullptr;
    REQUIRE(prGpuCommandsBegin(gpuDevice, nullptr, &refused));
    PrDrawTarget packed{.extent = {16, 16},
                        .format = PrPixelFormat_Rgb10A2,
                        .commands = refused};
    CHECK(prDrawFramePrepare(frame, list, &packed) == PrStatus_NotSupported);
    prGpuCommandsSubmitAndWait(refused);
    prDrawFrameDestroy(frame);
    prDrawListRelease(list);
    prDrawRendererDestroy(renderer);
    prGpuTextureDestroy(gpuDevice, texture);
    prGpuDeviceDestroy(gpuDevice);
}

TEST_CASE("the fragment renderer renders colors past white into a half-float target as the cpu backend does") {
    constexpr uint32_t kSize = 64;
    PrDrawList* list = prDrawListCreate();
    prDrawListEllipse(list, PrVec2{16, 16}, PrVec2{12, 9}, 0.3f, {.color = PrColor{3, 1, 0.2f, 1}});
    PrDrawColorStop stops[2] = {{.offset = 0, .color = PrColor{0, 0, 0, 1}},
                                {.offset = 1, .color = PrColor{4, 2, 1, 1}}};
    PrDrawBrush ramp = prDrawBrushLinear(PrVec2{32, 0}, PrVec2{64, 0}, stops, 2);
    prDrawListRect(list, PrRect{32, 0, 64, 32}, {.brush = &ramp});
    // Light added in a group, and a group blurred, both past white.
    PrDrawGroup added = prDrawGroupDefault();
    added.opacity = 0.75f;
    prDrawListPushGroup(list, &added);
    prDrawListRect(list, PrRect{4, 36, 28, 60}, {.color = PrColor{2, 0.5f, 0.5f, 1}});
    prDrawListCornerRect(list, prCornerRectRound(PrRect{14, 40, 30, 56}, 4),
                         {.color = PrColor{0.5f, 2, 2, 1}, .composite = PrDrawComposite_PlusLighter});
    prDrawListPopGroup(list);
    PrDrawFilterNode blur = nodeOf(PrDrawFilterKind_Blur);
    blur.inputs[0] = PrDrawFilterInput_Source;
    blur.blur.sigma = PrVec2{2, 2};
    PrDrawGroup blurred = prDrawGroupDefault();
    blurred.filter = filterOf({blur});
    prDrawListPushGroup(list, &blurred);
    prDrawListRect(list, PrRect{40, 40, 56, 56}, {.color = PrColor{3, 3, 3, 1}});
    prDrawListPopGroup(list);
    prDrawFilterRelease(blurred.filter);

    PrGpuDevice* gpuDevice = createDevice();
    PrDrawRenderer* renderer = createRenderer(gpuDevice);
    PrGpuTextureDesc textureDesc =
        prGpuTextureDesc2d(prGpuFormatFromPixelFormat(PrPixelFormat_RgbaF16), PrUVec2{kSize, kSize},
                           PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    PrGpuTexture texture{};
    REQUIRE(prGpuTextureCreate(gpuDevice, &textureDesc, &texture) == PrStatus_Ok);
    PrDrawFrame* frame = prDrawFrameCreate(renderer);
    PrGpuCommands* commands = nullptr;
    REQUIRE(prGpuCommandsBegin(gpuDevice, nullptr, &commands));
    PrDrawTarget target{.extent = {kSize, kSize},
                        .format = PrPixelFormat_RgbaF16,
                        .commands = commands};
    REQUIRE(prDrawFramePrepare(frame, list, &target) == PrStatus_Ok);
    PrGpuPassDesc pass = prGpuPassDescColor(texture, PrGpuLoad_Clear, PrColor{0, 0, 0, 0});
    PrGpuPass* encoder = beginPass(commands, &pass);
    prDrawFrameRender(frame, encoder, PrIVec2{0, 0});
    endPass(encoder, &pass);
    prGpuCommandsSubmitAndWait(commands);
    std::vector<PrFloat16> drawn(size_t(kSize) * kSize * 4);
    prGpuTextureRead(gpuDevice, texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0},
                     PrPixels{reinterpret_cast<uint8_t*>(drawn.data()), size_t(kSize) * 8, {kSize, kSize}});

    std::vector<PrFloat16> expected(drawn.size());
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrPixels into{reinterpret_cast<uint8_t*>(expected.data()), size_t(kSize) * 8, {kSize, kSize}};
    REQUIRE(prDrawRendererRenderPixels(cpu, list, into, PrPixelFormat_RgbaF16) == PrStatus_Ok);
    prDrawRendererDestroy(cpu);
    // The backends differ at edges by their coverage, as in 8 bits, scaled by how far past white a color is. The blur
    // is compared by its sum, because the cpu backend's is three box blurs where this backend samples the Gaussian.
    uint32_t differing = 0;
    float brightest = 0;
    double blurDrawn = 0;
    double blurExpected = 0;
    for (size_t i = 0; i < drawn.size(); i += 4) {
        bool inBlur = (i / 4) % kSize >= 34 && (i / 4) / kSize >= 34;
        blurDrawn += inBlur ? double(prFloat16ToFloat(drawn[i])) : 0;
        blurExpected += inBlur ? double(prFloat16ToFloat(expected[i])) : 0;
        for (size_t k = 0; k < 4 && !inBlur; ++k) {
            float a = prFloat16ToFloat(drawn[i + k]);
            float b = prFloat16ToFloat(expected[i + k]);
            brightest = std::max(brightest, a);
            if (std::abs(a - b) > 32.0f / 255 * std::max(1.0f, std::abs(b))) {
                ++differing;
                break;
            }
        }
    }
    CHECK(double(differing) / double(kSize * kSize) <= 0.01);
    CHECK(blurDrawn == doctest::Approx(blurExpected).epsilon(0.03));
    CHECK(blurExpected > 3.0 * 16 * 16 * 0.9);
    CHECK(brightest > 2.5f);
    prDrawFrameDestroy(frame);
    prDrawRendererDestroy(renderer);
    prGpuTextureDestroy(gpuDevice, texture);
    prGpuDeviceDestroy(gpuDevice);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer draws a group into the host's texture, which keeps each revision's contents once") {
    Device device(8, 8);
    PrGpuTextureDesc desc = prGpuTextureDesc2d(prGpuFormatFromPixelFormat(PrPixelFormat_Rgba8), PrUVec2{4, 4},
                                               PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
    PrGpuTexture held{};
    REQUIRE(prGpuTextureCreate(device.gpuDevice, &desc, &held) == PrStatus_Ok);
    uint64_t drawn = 0;
    PrDrawGroupLayer layer{.texture = prDrawGpuTexture(held), .extent = {4, 4}, .drawn = &drawn,
                           .sampling = PrDrawSampling_Nearest};
    // A list of revisions `first` to `last` of a group whose bounds cover the target. Revision 1 replaces the texture's
    // texels with red, and each later one draws half-opaque blue over the left half, which would darken if it drew
    // twice.
    auto list = [&](uint64_t first, uint64_t last) {
        PrDrawList* l = prDrawListCreate();
        layer.revision = last;
        PrDrawGroup group = prDrawGroupDefault();
        group.bounds = PrRect{0, 0, 8, 8};
        group.layer = &layer;
        prDrawListPushGroup(l, &group);
        for (uint64_t revision = first; revision <= last; ++revision) {
            PrDrawGroup segment = prDrawGroupDefault();
            segment.revision = revision;
            prDrawListPushGroup(l, &segment);
            if (revision == 1)
                prDrawListRect(l, PrRect{0, 0, 8, 8},
                               {.color = PrColor{1, 0, 0, 1}, .composite = PrDrawComposite_Copy});
            else
                prDrawListRect(l, PrRect{0, 0, 4, 8}, {.color = PrColor{0, 0, 1, 0.5f}});
            prDrawListPopGroup(l);
        }
        prDrawListPopGroup(l);
        return l;
    };
    auto pixel = [](const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y) {
        const uint8_t* p = &pixels[(size_t(y) * 8 + x) * 4];
        return std::to_string(p[0]) + "," + std::to_string(p[1]) + "," + std::to_string(p[2]) + "," +
               std::to_string(p[3]);
    };
    PrDrawList* first = list(1, 1);
    std::vector<uint8_t> once = device.render(first);
    CHECK(drawn == 1);
    CHECK(pixel(once, 6, 6) == "255,0,0,255");
    // The same list again draws nothing new into the texture, which still shows what it holds.
    CHECK(pixel(device.render(first), 6, 6) == "255,0,0,255");
    // A list of revisions 1 and 2 draws only 2 over what the texture holds, and drawn again draws nothing new.
    PrDrawList* second = list(1, 2);
    std::vector<uint8_t> blended = device.render(second);
    CHECK(drawn == 2);
    CHECK(pixel(blended, 6, 6) == "255,0,0,255");
    CHECK(pixel(blended, 1, 1) != "255,0,0,255");
    CHECK(pixel(device.render(second), 1, 1) == pixel(blended, 1, 1));
    prDrawListRelease(first);
    prDrawListRelease(second);
    prGpuTextureDestroy(device.gpuDevice, held);
}

TEST_CASE("the fragment header compiles as C") {
    CHECK(prTestDrawFragmentCApi() == 0);
}

TEST_CASE("the fragment renderer draws a rounded rectangle as one instance, and tessellates a large path") {
    Device device(64, 64);
    PrDrawList* list = prDrawListCreate();
    prDrawListCornerRect(list, prCornerRectRound(PrRect{8, 8, 40, 30}, 4), {.color = PrColor{1, 0, 0, 1}});
    device.render(list);
    CHECK(device.stats().primitives == 1);
    // A path too large for the atlas is tessellated, with one instance for each triangle.
    prDrawListClear(list);
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{4, 4});
    prPathLineTo(path, PrVec2{140, 10});
    prPathLineTo(path, PrVec2{10, 140});
    prPathClose(path);
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = PrColor{0, 0, 1, 1}});
    device.render(list);
    CHECK(device.stats().primitives > 1);
    prPathDestroy(path);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer clips to rectangles and rounded rectangles without a layer, and to paths with one") {
    Device device(64, 64);
    PrDrawList* list = prDrawListCreate();
    prDrawListPushClipRect(list, PrRect{2, 2, 40.5f, 40}, PrDrawEdges_Antialiased);
    prDrawListPushClipCornerRect(list, prCornerRectRound(PrRect{4, 4, 30, 30}, 6), PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 64, 64}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopClip(list);
    prDrawListPopClip(list);
    device.render(list);
    CHECK(device.stats().layers == 0);
    prDrawListClear(list);
    PrPath* triangle = prPathCreate();
    prPathMoveTo(triangle, PrVec2{4, 4});
    prPathLineTo(triangle, PrVec2{30, 4});
    prPathLineTo(triangle, PrVec2{4, 30});
    prPathClose(triangle);
    prDrawListPushClipPath(list, triangle, PrPathFillRule_NonZero, PrDrawEdges_Antialiased);
    prDrawListRect(list, PrRect{0, 0, 64, 64}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPopClip(list);
    prPathDestroy(triangle);
    device.render(list);
    CHECK(device.stats().layers == 1);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer paints a gradient's hard stops as the cpu backend does") {
    Device device(96, 32);
    PrDrawList* list = prDrawListCreate();
    PrDrawColorStop hard[4] = {{.offset = 0, .color = PrColor{0, 1, 0, 1}},
                               {.offset = 0.25f, .color = PrColor{0, 1, 0, 1}},
                               {.offset = 0.25f, .color = PrColor{1, 1, 0, 1}},
                               {.offset = 1, .color = PrColor{1, 0, 1, 1}}};
    PrDrawBrush brush = prDrawBrushLinear(PrVec2{0, 0}, PrVec2{96, 0}, hard, 4);
    prDrawListRect(list, PrRect{0, 0, 96, 32}, {.brush = &brush});
    std::vector<uint8_t> drawn = device.render(list);
    CHECK(fractionDiffering(drawn, reference(list, 96, 32), 6) <= 0.01);
    prDrawListRelease(list);
}

TEST_CASE("the fragment renderer uploads an image, glyphs and small paths' coverage once") {
    Device device(64, 64);
    const PrFont* font = load("NotoSans.ttf");
    uint8_t texels[4] = {255, 0, 0, 255};
    PrDrawImageDesc desc{PrPixelView{texels, 4, {1, 1}}, PrPixelFormat_Rgba8, PrDrawAlpha_Premultiplied,
                         PrColorSpace_Srgb};
    const PrDrawImage* image = nullptr;
    REQUIRE(prDrawImageCreate(&desc, &image) == PrStatus_Ok);
    PrPath* path = prPathCreate();
    prPathMoveTo(path, PrVec2{4, 4});
    prPathLineTo(path, PrVec2{20, 12});
    prPathLineTo(path, PrVec2{4, 20});
    PrDrawList* list = prDrawListCreate();
    PrDrawBrush brush = prDrawBrushImage(prDrawSourceImage(image, PrRect{0, 0, 1, 1}), PrRect{0, 0, 16, 16},
                                         PrDrawSampling_Nearest);
    prDrawListRect(list, PrRect{0, 0, 16, 16}, {.brush = &brush});
    PrFontGlyph glyphs[2] = {{prFontCodePointGlyph(font, 'H'), {20, 30}},
                             {prFontCodePointGlyph(font, 'i'), {40, 30}}};
    PrFontGlyphRun run{};
    run.font = font;
    run.size = 24;
    run.glyphs = glyphs;
    run.glyphCount = 2;
    prDrawListGlyphs(list, &run, PrVec2{}, {.color = PrColor{0, 0, 0, 1}});
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{30, 36}));
    prDrawListPath(list, path, PrPathFillRule_NonZero, {.color = PrColor{1, 0, 0, 1}});
    prDrawListPopTransform(list);
    device.render(list);
    PrDrawStats first = device.stats();
    CHECK(first.pathFills == 1);
    // The next frame finds the texels and the coverage that the first uploaded.
    device.render(list);
    PrDrawStats second = device.stats();
    CHECK(second.pathFills == 0);
    CHECK(second.uploadBytes < first.uploadBytes);
    CHECK(second.primitives == first.primitives);
    prPathDestroy(path);
    prDrawListRelease(list);
    prDrawImageRelease(image);
    prFontRelease(font);
}
