// The perspective example: Studio Kit panels of laid-out content standing in 3D, drawn and hit-tested through one
// projection, over a background that the application renders with its own pipeline, as a 3D engine would.
//
// Four panels are planes (prKitPlane) in a row that is itself a plane with a perspective, so they share one eye. The
// panels turn to face the pointer, and a click on a panel tweens it to the front and raises its level. The button and
// the slider on each panel answer where they are drawn, under the turn. Each panel's ground blurs what lies beneath it,
// which is the application's background, read through the panel's projection.
//
// A frame of the window is one command buffer. The application draws its background with its own pipeline, and lib/draw
// renders the UI over it in the same pass. The UI is prepared before that pass begins, so its backdrops cannot read the
// pass's target. A toggle chooses what the application provides instead: the background rendered first into a texture
// of its own, which it then copies into the pass, or a function that draws the background again into a texture that the
// renderer owns (PrDrawBeneath).

#include "pr/app.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/compress.h"
#include "pr/draw_filter.h"
#include "pr/draw_image.h"
#include "pr/draw_renderer.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/font_bundle.h"
#include "pr/gpu.h"
#include "pr/image.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_input.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/layout_scene.h"
#include "pr/layout_unit.h"
#include "pr/print.h"
#include "pr/text.h"

#include "examples/perspective/perspective_gpu.h"

namespace {

using namespace perspective;
using namespace pr;

constexpr uint32_t kPanels = 4;

// The distance from the eye to the row of panels, in px.
constexpr float kEye = 900;

// The height of a panel, in px.
constexpr float kPanelHeight = 400;

// How long a panel takes to come to the front or go back, in seconds.
constexpr double kTurnSeconds = 0.4;

// The tweens of a panel, kept on the row under the example's owner, at the key of the panel times Channel_Count plus
// the channel. The row exists before the panels' records are made, which need the tweened values.
enum {
    Channel_Depth,
    Channel_Turn,
    Channel_Scale,
    Channel_Count,
};
typedef uint32_t Channel;

// The address that owns the example's tweens.
constexpr char kTweenOwner = 0;

// What a panel shows.
struct PanelText {
    const char* caption;
    const char* heading;
    const char* body;
    const char* action;
};

constexpr PanelText kPanelText[kPanels] = {
    {
        "LAYOUT", "Boxes the engine placed",
        "Every panel is laid out flat by the layout engine: a heading, a picture, a paragraph that wraps to the width it "
            "was given, and controls. Nothing in its layout knows that it will be turned.",
        "Measure",
    },
    {
        "DRAW", "One projection to the target",
        "A panel is a projected group. Its content is drawn flat into a layer, and the layer is composited through the "
            "projection, sampled from mip levels so that the far edge stays smooth.",
        "Record",
    },
    {
        "INPUT", "The pointer takes the same map",
        "Hit testing maps the pointer back through the projection into the panel's plane, so a click lands where the "
            "panel is drawn. The button and the slider below answer under the turn.",
        "Route",
    },
    {
        "MOTION", "Tweened to the front",
        "Clicking a panel tweens its depth, its turn and its scale toward new targets, and raises its level so that it "
            "paints over its neighbors, while the pointer keeps turning the row.",
        "Replay",
    },
};

// ---------------------------------------------------------------------------------------------------------------------
// The host's background

// The background, in logical pixels: a vertical gradient under a grid of cells that drifts with time. Some cells are
// lit in a color of the palette, whose strength pulses (perspective_gpu.slang).
constexpr float kCell = 64;
constexpr float kGap = 8;
constexpr float kDrift[2] = {18, 10};       // px per second
constexpr float kPalette[4][3] = {
    {0.20f, 0.45f, 0.95f}, {0.85f, 0.25f, 0.55f}, {0.95f, 0.62f, 0.18f}, {0.15f, 0.75f, 0.65f},
};
constexpr float kTop[3] = {0.06f, 0.07f, 0.15f};
constexpr float kBottom[3] = {0.20f, 0.09f, 0.24f};

// ---------------------------------------------------------------------------------------------------------------------
// The panels

// The example's state: the panels' values, which panel is in front, and what the panels' painters draw with.
struct Panels {
    const PrDrawImage* picture = nullptr;
    const PrDrawFilter* frosted = nullptr;      // the blur that a panel's ground applies to what lies beneath it
    PrColor glass{};
    PrColor rim{};
    float sliders[kPanels] = {0.35f, 0.5f, 0.65f, 0.8f};
    uint32_t presses[kPanels] = {};
    uint32_t chosen = kPanels;                  // the panel in front, or kPanels for none
    bool redraw = false;                        // the host provides what lies beneath as a function, not a texture
};

// Draws a panel's ground: a rounded pane that blurs what lies beneath it under a translucent tint, and its rim.
void paintGlass(void* context, PrVec2 size, PrDrawList* list) {
    const Panels& p = *static_cast<const Panels*>(context);
    PrRect pane{0, 0, size.x, size.y};
    PrCornerRect shape = prCornerRectRound(pane, 12);
    PrDrawGroup group = prDrawGroupDefault();
    group.bounds = pane;
    group.backdrop = p.frosted;
    prDrawListPushClipCornerRect(list, shape, PrDrawEdges_Antialiased);
    prDrawListPushGroup(list, &group);
    prDrawListRect(list, pane, {.color = p.glass});
    prDrawListPopGroup(list);
    prDrawListPopClip(list);
    PrDrawStroke rim = prDrawStrokeDefault();
    rim.shape.width = 1;
    rim.align = PrDrawStrokeAlign_Inside;
    prDrawListCornerRect(list, shape, {.color = p.rim, .stroke = &rim});
}

// Draws the picture so that it covers the box, centered, with rounded corners.
void paintPicture(void* context, PrVec2 size, PrDrawList* list) {
    const Panels& p = *static_cast<const Panels*>(context);
    if (p.picture == nullptr)
        return;
    PrUVec2 natural = prDrawImageExtent(p.picture);
    float fit = prF32Max(size.x / float(natural.x), size.y / float(natural.y));
    PrVec2 drawn{float(natural.x) * fit, float(natural.y) * fit};
    PrRect at{(size.x - drawn.x) / 2, (size.y - drawn.y) / 2, (size.x + drawn.x) / 2, (size.y + drawn.y) / 2};
    PrDrawSource source = prDrawSourceImage(p.picture, PrRect{0, 0, float(natural.x), float(natural.y)});
    PrDrawBrush brush = prDrawBrushImage(source, at, PrDrawSampling_Mipmap);
    prDrawListCornerRect(list, prCornerRectRound(PrRect{0, 0, size.x, size.y}, 6), {.brush = &brush});
}

// Returns a panel's transform about its center: scaled, turned about x and then y, and moved along z.
PrMatrix4 panelTransform(float tilt, float turn, float depth, float scale) {
    PrMatrix4 m = prMatrix4Scale(PrVec3{scale, scale, 1});
    PrMatrix4 x = prMatrix4Rotate(PrVec3{1, 0, 0}, tilt);
    m = prMatrix4Concat(&m, &x);
    PrMatrix4 y = prMatrix4Rotate(PrVec3{0, 1, 0}, turn);
    m = prMatrix4Concat(&m, &y);
    PrMatrix4 z = prMatrix4Translate(PrVec3{0, 0, depth});
    return prMatrix4Concat(&m, &z);
}

void buildPanel(PrKit* kit, PrLayoutNode row, uint32_t index, Panels& p, const PrMatrix4& transform) {
    PrUi* ui = kit->ui;
    PrLayoutTree* tree = kit->tree;
    const PanelText& text = kPanelText[index];
    // The panel's content is a layer, which takes no room, so the panel's height is given.
    PrKitPlaneStyle plane{
        .box = {.place = {.height = prKitPx(kPanelHeight), .minWidth = prKitPx(210), .flex = PrKitFlex_Fill}},
        .zIndex = p.chosen == index ? 1 : 0,
        .backfaceHidden = true,
    };
    PrLayoutNode panel = prKitPlane(kit, row, {.transform = &transform}, plane);
    prUiSetFlags(ui, panel, PrUiFlag_Pressable);
    prUiSetCursor(ui, panel, PrAppCursor_Hand);
    if (prUiClicked(ui, panel, 0))
        p.chosen = p.chosen == index ? kPanels : index;

    // The ground and the content are layers of the panel, so that the content paints over the ground.
    PrKitBoxStyle cover{};
    PrLayoutNode under = prKitLayer(kit, panel, {}, cover);
    PrLayoutNode glass = prLayoutTreeFrameReplaced(tree, under, 0, 0);
    PrKitPlace fill{.flex = PrKitFlex_Fill};
    prKitPlace(kit, glass, &fill);
    PrLayoutContent ground{.kind = PrLayoutContentKind_Painter};
    ground.painter.paint = paintGlass;
    ground.painter.context = &p;
    prLayoutTreeFrameSetContent(tree, glass, &ground);

    PrKitBoxStyle column{.gap = 10, .padding = prKitEdgesAll(16)};
    PrLayoutNode content = prKitLayer(kit, panel, {}, column);
    PrKitTextStyle caption{.role = PrKitTextRole_Caption, .tone = PrKitTone_Accent};
    prKitLabel(kit, content, {.text = text.caption}, caption);
    PrKitTextStyle heading{.role = PrKitTextRole_Heading};
    prKitLabel(kit, content, {.text = text.heading}, heading);
    PrLayoutNode picture = prLayoutTreeFrameReplaced(tree, content, 0, 0);
    PrKitPlace pictureBox{.height = prKitPx(96), .flex = PrKitFlex_Fixed};
    prKitPlace(kit, picture, &pictureBox);
    PrLayoutContent painted{.kind = PrLayoutContentKind_Painter};
    painted.painter.paint = paintPicture;
    painted.painter.context = &p;
    prLayoutTreeFrameSetContent(tree, picture, &painted);
    PrKitTextStyle body{.tone = PrKitTone_Secondary};
    prKitLabel(kit, content, {.text = text.body}, body);

    PrKitBoxStyle controls{.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = 10};
    PrLayoutNode line = prKitBox(kit, content, {}, controls);
    PrKitButton action{.text = text.action};
    PrKitButtonStyle small{.size = PrKitControlSize_Sm};
    if (prKitActivated(kit, prKitButton(kit, line, action, small)))
        ++p.presses[index];
    prKitSlider(kit, line, {.value = prKitScalarF32(&p.sliders[index]), .min = 0, .max = 1}, {});
    char counted[64];
    PrStringView written = formatToBuffer(Span(counted), "{} x{}, slider {:.2f}", text.action, p.presses[index],
        p.sliders[index]);
    PrKitTextStyle meta{.role = PrKitTextRole_Meta, .tone = PrKitTone_Muted};
    prKitLabel(kit, content, {.text = written}, meta);
}

// Declares the round: a header with the backdrop's toggle, and the row of panels in perspective.
void buildApp(void* context, PrKit* kit, PrLayoutNode ground) {
    Panels& p = *static_cast<Panels*>(context);
    const PrKitPalette& palette = kit->theme.palette;
    p.glass = PrColor{palette.bg1.r, palette.bg1.g, palette.bg1.b, 0.5f};
    p.rim = palette.line2;
    PrUi* ui = kit->ui;

    PrKitSurfaceStyle header{
        .box = {
            .direction = PrKitDirection_Row,
            .align = PrKitAlign_Center,
            .gap = 16,
            .padding = prKitEdgesXY(10, 14),
        },
        .ground = PrKitGround_Hud,
        .rules = PrKitRuleFlag_All,
    };
    // The header stands at a level above the row, so that a panel tweened to the front passes under it.
    PrLayoutNode bar = prKitSurface(kit, prKitPlane(kit, ground, {}, {.zIndex = 1}), {}, header);
    PrLayoutNode titles = prKitBox(kit, bar, {}, {.gap = 4});
    PrKitPlace grow{.flex = PrKitFlex_Grow};
    prKitPlace(kit, titles, &grow);
    PrKitTextStyle caption{.role = PrKitTextRole_Caption, .tone = PrKitTone_Accent};
    prKitLabel(kit, titles, {.text = "PRAX / PANELS IN PERSPECTIVE"}, caption);
    PrKitTextStyle title{.role = PrKitTextRole_Title};
    prKitLabel(kit, titles, {.text = "A user interface that the layout engine placed, standing in 3D"}, title);
    PrKitTextStyle secondary{.tone = PrKitTone_Secondary};
    prKitLabel(kit, titles, {
        .text = "Move the pointer to turn the row. Click a panel to bring it forward. The panels blur the "
            "host's background through their projection.",
    }, secondary);
    prKitToggle(kit, bar, {.value = &p.redraw, .label = "Backdrop redrawn by the host"}, {});

    PrKitPlaneStyle stage{
        .box = {
            .direction = PrKitDirection_Row,
            .align = PrKitAlign_Center,
            .gap = 24,
            .padding = prKitEdgesXY(24, 8),
            .place = {.flex = PrKitFlex_Fill},
        },
        .perspective = kEye,
    };
    PrLayoutNode row = prKitPlane(kit, ground, {}, stage);

    PrVec2 extent{};
    PrVec2 pointer{};
    float reach = 0;
    float lift = 0;
    PrRect viewport = prLayoutRectToPx(prLayoutNodeBounds(kit->tree, ground, PrLayoutCoordinates_Viewport));
    extent = PrVec2{viewport.right - viewport.left, viewport.bottom - viewport.top};
    if (prUiPointer(ui, &pointer) && extent.x > 1 && extent.y > 1) {
        reach = pointer.x / extent.x - 0.5f;
        lift = pointer.y / extent.y - 0.5f;
    }
    PrLayoutEasing easeOut{.x1 = 0, .y1 = 0, .x2 = 0.58, .y2 = 1, .kind = PrLayoutEasingKind_CubicBezier};
    for (uint32_t i = 0; i < kPanels; ++i) {
        bool front = p.chosen == i;
        auto tween = [&](Channel channel, float target) {
            return prUiTween(ui, row, &kTweenOwner, i * Channel_Count + channel, target, kTurnSeconds, &easeOut);
        };
        float depth = tween(Channel_Depth, front ? 100.0f : -60.0f * float(i) - 90.0f);
        float rest = tween(Channel_Turn, front ? 0.0f : 0.18f * float(i) - 0.27f);
        float scale = tween(Channel_Scale, front ? 1.08f : 1.0f);
        // Each panel turns its front toward the pointer: the edge nearer the pointer comes toward the eye.
        PrMatrix4 transform = panelTransform(lift * 0.55f, rest - reach * 0.9f, depth, scale);
        buildPanel(kit, row, i, p, transform);
    }
}

// Opens the picture and the blur that the panels' painters draw with. Returns false if the picture could not be read.
bool openPanels(Panels& p) {
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterNode blur{};
    blur.kind = PrDrawFilterKind_Blur;
    blur.inputs[0] = PrDrawFilterInput_Backdrop;
    blur.blur.sigma = PrVec2{12, 12};
    blur.blur.edge = PrImageEdgeMode_Duplicate;
    prDrawFilterBuilderAdd(builder, &blur);
    p.frosted = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);

    PrByteView file = {};
    if (!prFileMap(PR_PERSPECTIVE_PICTURE, &file))
        return false;
    PrCompressCodec* compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
    PrImageCodec* images = prImageCodecCreate(PrImageCodecBackend_Default, compress);
    PrByteView bytes{file.data, file.size};
    bool read = prDrawImageCreateFromBytes(images, bytes, &p.picture, nullptr) == PrStatus_Ok;
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
    prFileUnmap(&file);
    return read;
}

void closePanels(Panels& p) {
    prDrawImageRelease(p.picture);
    prDrawFilterRelease(p.frosted);
    p.picture = nullptr;
    p.frosted = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// The window

// The example in a window: its panels, its UI, the device that it renders on, and its own pipelines.
struct Window {
    Panels panels;

    PrFontSet* fonts = nullptr;
    PrTextShaper* shaper = nullptr;
    PrLayoutTree* tree = nullptr;
    PrUi* ui = nullptr;
    PrKit* kit = nullptr;
    const PrLayoutScene* scene = nullptr;   // the scene of the last frame, which its list plays
    PrDrawList* list = nullptr;
    Array<PrLayoutSceneScroll> scrolls;

    PrApp* app = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrDrawRenderer* renderer = nullptr;
    PrDrawFrame* frame = nullptr;
    Pipelines* pipelines = nullptr;
    PrGpuTexture background{};      // the background, rendered first when the example provides it as a texture

    float time = 0;                 // the background's time in this frame, in seconds
    double origin = -1;             // the app's time at the first frame
    float pixelRatio = 1;           // this frame's display's
    float height = 0;               // this frame's viewport's, in logical pixels
};

// Declares a round: a surface on no ground, so that the background shows beneath the UI, and the panels under it.
void build(void* context, PrUi*, const PrUiRound*) {
    Window& w = *static_cast<Window*>(context);
    PrKitSurfaceStyle style{
        .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
        .ground = PrKitGround_None,
    };
    buildApp(&w.panels, w.kit, prKitSurface(w.kit, kPrLayoutNodeNone, {}, style));
}

// Draws the background into the pass. The pass's pixel (0, 0) is the background's target pixel `origin`.
void drawBackground(const Window& w, PrGpuPass* pass, PrIVec2 origin) {
    BackgroundArguments arguments{};
    for (uint32_t i = 0; i < 4; ++i) {
        for (uint32_t k = 0; k < 3; ++k)
            arguments.palette[i][k] = kPalette[i][k];
    }
    for (uint32_t k = 0; k < 3; ++k) {
        arguments.top[k] = kTop[k];
        arguments.bottom[k] = kBottom[k];
    }
    arguments.origin[0] = float(origin.x);
    arguments.origin[1] = float(origin.y);
    arguments.drift[0] = kDrift[0];
    arguments.drift[1] = kDrift[1];
    arguments.pixelRatio = w.pixelRatio;
    arguments.time = w.time;
    arguments.height = w.height;
    arguments.cell = kCell;
    arguments.gap = kGap;
    pipelinesDrawBackground(w.pipelines, pass, arguments);
}

// Draws what lies beneath a region of the UI again, for a backdrop. The UI renders at the target's pixel (0, 0).
void redrawBeneath(void* PR_NULLABLE context, PrGpuPass* pass, PrIRect area) {
    drawBackground(*static_cast<const Window*>(context), pass, PrIVec2{area.left, area.top});
}

// Frees what the window made on its device, and the device.
void closeDevice(Window& w) {
    if (w.device != nullptr)
        prGpuTextureDestroy(w.device, w.background);
    pipelinesDestroy(w.pipelines);
    prDrawFrameDestroy(w.frame);
    prDrawRendererDestroy(w.renderer);
    prGpuSwapChainDestroy(w.swapChain);
    prGpuDeviceDestroy(w.device);
    w.background = PrGpuTexture{};
    w.pipelines = nullptr;
    w.frame = nullptr;
    w.renderer = nullptr;
    w.swapChain = nullptr;
    w.device = nullptr;
}

// Makes the platform's device, a swap chain over the window for SDR, the renderer and the example's pipelines. Returns
// false if the window has no device to render with.
bool openDevice(Window& w) {
    PrAppSurface surface{};
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Sdr};
    if (prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &w.device) != PrStatus_Ok ||
            prAppSurface(w.app, &surface) != PrStatus_Ok ||
            prGpuSwapChainCreate(w.device, &surface, &desc, &w.swapChain) != PrStatus_Ok ||
            prDrawRendererCreate(PrDrawRendererBackend_Default, w.device, nullptr, &w.renderer) != PrStatus_Ok ||
            (w.pipelines = pipelinesCreate(w.device)) == nullptr) {
        closeDevice(w);
        return false;
    }
    w.frame = prDrawFrameCreate(w.renderer);
    return true;
}

// Returns `*texture` made again in `format` at `extent` if it is not already that.
PrGpuTexture textureFor(PrGpuDevice* device, PrGpuTexture* texture, PrGpuFormat format, PrUVec2 extent) {
    PrUVec2 held = texture->id != 0 ? prGpuTextureExtent(device, *texture) : PrUVec2{};
    if (texture->id == 0 || held.x != extent.x || held.y != extent.y) {
        prGpuTextureDestroy(device, *texture);
        PrGpuTextureDesc desc = prGpuTextureDesc2d(format, extent,
            PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
        *texture = PrGpuTexture{};
        prGpuTextureCreate(device, &desc, texture);
    }
    return *texture;
}

// Renders the background and the list over it into the swap chain's texture at `extent` target pixels, and presents.
void render(Window& w, PrUVec2 extent) {
    PrGpuTexture drawable{};
    PrGpuCommands* commands = nullptr;
    if (prGpuSwapChainAcquire(w.swapChain, extent, &drawable) != PrStatus_Ok ||
            !prGpuCommandsBegin(w.device, nullptr, &commands))
        return;
    PrGpuFormat format = prGpuSwapChainInfo(w.swapChain).format;
    PrGpuSubresources all{};
    PrDrawBeneath beneath{};
    if (!w.panels.redraw) {
        // The background, rendered into a texture of the example's own before the UI is prepared over it.
        PrGpuTexture background = textureFor(w.device, &w.background, format, extent);
        prGpuCommandsTextureBarrier(commands, background, all, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc first = prGpuPassDescColor(background, PrGpuLoad_Clear, PrColor{0, 0, 0, 1});
        PrGpuPass* pass = prGpuPassBegin(commands, &first);
        drawBackground(w, pass, PrIVec2{0, 0});
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, background, all, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
        beneath.kind = PrDrawBeneathKind_Texture;
        beneath.texture = prDrawGpuTexture(background);
        beneath.extent = extent;
    } else {
        beneath.kind = PrDrawBeneathKind_Draw;
        beneath.draw = redrawBeneath;
        beneath.context = &w;
    }
    PrPixelFormat pixels = PrPixelFormat_Bgra8;
    prGpuFormatToPixelFormat(format, &pixels);
    PrDrawTarget into{.extent = extent, .format = pixels, .commands = commands, .beneath = &beneath};
    prDrawFramePrepare(w.frame, w.list, &into);
    prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_Discard, PrGpuAccessFlag_ColorAttachment);
    PrGpuPassDesc main = prGpuPassDescColor(drawable, PrGpuLoad_Clear, PrColor{0, 0, 0, 1});
    PrGpuPass* pass = prGpuPassBegin(commands, &main);
    if (!w.panels.redraw)
        pipelinesCopy(w.pipelines, pass, w.background);
    else
        drawBackground(w, pass, PrIVec2{0, 0});
    prDrawFrameRender(w.frame, pass, PrIVec2{0, 0});
    prGpuPassEnd(pass);
    prGpuCommandsTextureBarrier(commands, drawable, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_Present);
    prGpuSwapChainPresent(w.swapChain, commands);
    prGpuCommandsSubmit(commands);
}

void start(void* context, PrApp* app, const PrAppFrame*) {
    Window& w = *static_cast<Window*>(context);
    w.app = app;
    if (!openDevice(w)) {
        printError("perspective: no GPU device to render with\n");
        prAppStop(app);
        return;
    }
    prAppRequestFrame(app);
}

PrAppOutput frame(void* context, PrApp*, const PrAppFrame* f) {
    Window& w = *static_cast<Window*>(context);
    // A lost device is made again, with everything on it.
    if (w.device != nullptr && prGpuDeviceIsLost(w.device))
        closeDevice(w);
    if (w.device == nullptr && !openDevice(w))
        return {};
    prGpuSwapChainWait(w.swapChain);
    if (w.origin < 0)
        w.origin = f->time;
    w.time = float(f->time - w.origin);
    w.pixelRatio = f->display.pixelRatio;
    w.height = f->extent.y;

    PrUiFrameDesc desc{
        .time = f->time,
        .delta = f->delta,
        .extent = f->extent,
        .maxRounds = 4,
        .display = f->display,
        .preferences = f->preferences,
    };
    prUiFrame(w.ui, &desc, build, &w);

    // The tree's scene plays into the list in target pixels, each scroll container at the tree's offset for it. The
    // background lies beneath the list in its pass, so the list fills its own canvas.
    const PrLayoutScene* scene = prLayoutTreeScene(w.tree);
    prLayoutSceneRelease(w.scene);
    w.scene = scene;
    w.scrolls.resize(prLayoutSceneScrolls(w.tree, scene, nullptr, 0));
    prLayoutSceneScrolls(w.tree, scene, w.scrolls.data(), w.scrolls.count());
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = w.scrolls.data();
    options.scrollCount = w.scrolls.count();
    prDrawListClear(w.list);
    prDrawListPushTransformAffine2(w.list, prAffine2Scale(PrVec2{w.pixelRatio, w.pixelRatio}));
    prLayoutScenePlay(scene, w.list, &options);
    prDrawListPopTransform(w.list);

    PrUVec2 extent{uint32_t(prF32Round(f->extent.x * w.pixelRatio)), uint32_t(prF32Round(f->extent.y * w.pixelRatio))};
    if (extent.x != 0 && extent.y != 0)
        render(w, extent);
    // The background moves in every frame.
    PrAppOutput output = prUiOutput(w.ui);
    output.wantsFrame = true;
    return output;
}

void event(void* context, PrApp* app, const PrAppEvent* e) {
    Window& w = *static_cast<Window*>(context);
    if (e->kind == PrAppEventKind_KeyDown && e->key == PrAppKey_Escape) {
        prAppStop(app);
        return;
    }
    prUiPushEvent(w.ui, e);
    prAppRequestFrame(app);
}

void shutdown(void* context, PrApp*) {
    closeDevice(*static_cast<Window*>(context));
}

} // namespace

int main() {
    prCrashHandler();
    static Window w;
    if (!openPanels(w.panels))
        printError("perspective: the picture at {} could not be read\n", PR_PERSPECTIVE_PICTURE);
    w.fonts = prFontSetCreate(nullptr);
    if (prFontBundleInstall(w.fonts, PrFontBundle_Noto, PR_FONT_BUNDLE_DIR) == 0) {
        printError("perspective: the fonts under {} could not be installed\n", PR_FONT_BUNDLE_DIR);
        return 1;
    }
    w.shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrLayoutTreeDesc treeDesc{.shaper = w.shaper};
    w.tree = prLayoutTreeCreate(&treeDesc);
    w.ui = prUiCreate(w.tree);
    PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Dark, w.fonts);
    w.kit = prKitCreate(w.ui, &theme);
    w.list = prDrawListCreate();

    PrAppDesc desc{.title = "Prax Perspective", .extent = {1180, 820}};
    PrApp* app = prAppCreate(PrAppBackend_Default, &desc);
    PrAppHandler handler{&w, start, frame, event, shutdown};
    prAppRun(app, &handler);
    prAppDestroy(app);

    prDrawListRelease(w.list);
    prLayoutSceneRelease(w.scene);
    prKitDestroy(w.kit);
    prUiDestroy(w.ui);
    prLayoutTreeDestroy(w.tree);
    prTextShaperDestroy(w.shaper);
    prFontSetDestroy(w.fonts);
    closePanels(w.panels);
    return 0;
}
