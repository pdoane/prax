// The headless host that the editor benchmark measures through (host.h).

#include "tools/bench/editor_bench/host.h"

#include "pr/font_bundle.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/text_harfbuzz.h"

namespace editor_bench {

namespace {

// Declares the round: the root, a panel on the app's background that fills the viewport, and the application's widgets
// under it.
void buildRound(void* context, PrUi*, const PrUiRound*) {
    Host& host = *static_cast<Host*>(context);
    PrKitPanelStyle style{
        .box = {.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .height = prKitPercent(100)}},
        .background = PrKitBackground_App,
    };
    PrLayoutNode root = prKitPanel(host.kit, kPrLayoutNodeNone, {}, style);
    if (host.build != nullptr)
        host.build(host.context, host.kit, root);
}

// Creates the host's tree, context and kit over its fonts and shaper, the kit in `theme`.
void createTree(Host& host, const PrKitTheme& theme) {
    PrLayoutTreeDesc desc{.shaper = host.shaper, .targetPixelRatio = host.display.pixelRatio};
    host.tree = prLayoutTreeCreate(&desc);
    host.ui = prUiCreate(host.tree);
    host.kit = prKitCreate(host.ui, &theme);
}

// Destroys what createTree created, and the scene of the tree's last frame.
void destroyTree(Host& host) {
    if (host.attached && host.detach != nullptr)
        host.detach(host.context, host);
    host.attached = false;
    prLayoutSceneRelease(host.scene);
    host.scene = nullptr;
    prKitDestroy(host.kit);
    prUiDestroy(host.ui);
    prLayoutTreeDestroy(host.tree);
}

} // namespace

bool hostOpen(Host& host, const char* fontDirectory) {
    host.fonts = prFontSetCreate(nullptr);
    PrFontBundle bundle = PrFontBundle_Complete;
    if (prFontBundleInstall(host.fonts, &bundle, 1, fontDirectory) == 0)
        return false;
    host.shaper = prTextHarfBuzzCreate();
    createTree(host, prKitThemeStudio(PrKitThemeVariant_Dark, host.fonts));
    host.list = prDrawListCreate();
    return true;
}

void hostClose(Host& host) {
    destroyTree(host);
    prDrawListRelease(host.list);
    prTextShaperDestroy(host.shaper);
    prFontSetDestroy(host.fonts);
    host = Host{};
}

void hostReset(Host& host) {
    PrKitTheme theme = host.kit->theme;
    destroyTree(host);
    createTree(host, theme);
}

void hostEvent(Host& host, const PrAppEvent& event) {
    prUiPushEvent(host.ui, kPrUiViewportMain, &event);
}

void hostFrame(Host& host, double time, double delta, PrVec2 extent, const PrAppDisplay& display,
        const PrAppUserPreferences& preferences, uint32_t rounds) {
    host.display = display;
    PrUiFrameDesc desc{
        .time = time,
        .delta = delta,
        .extent = extent,
        .maxRounds = rounds,
        .display = display,
        .preferences = preferences,
    };
    if (!host.attached && host.attach != nullptr)
        host.attach(host.context, host);
    host.attached = true;
    host.rounds = prUiFrame(host.ui, &desc, buildRound, &host);
    const PrLayoutScene* scene = prLayoutTreeScene(host.tree);
    prLayoutSceneRelease(host.scene);
    host.scene = scene;
    prDrawListClear(host.list);
    float ratio = display.pixelRatio;
    prDrawListPushTransformAffine2(host.list, prAffine2Scale(PrVec2{ratio, ratio}));
    // Every scroll container of the scene plays at the tree's offset for it.
    host.scrolls.resize(prLayoutSceneScrolls(host.tree, scene, nullptr, 0));
    prLayoutSceneScrolls(host.tree, scene, host.scrolls.data(), uint32_t(host.scrolls.size()));
    host.canvas = prLayoutSceneCanvasColor(scene);
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.scrolls = host.scrolls.data();
    options.scrollCount = uint32_t(host.scrolls.size());
    options.canvasFilled = true;
    prLayoutScenePlay(scene, host.list, &options);
    prDrawListPopTransform(host.list);
}

PrColor hostClearColor(const Host& host) {
    PrColor c = prColorFromColor16(host.canvas);
    return PrColor{c.r * c.a, c.g * c.a, c.b * c.a, c.a};
}

} // namespace editor_bench
