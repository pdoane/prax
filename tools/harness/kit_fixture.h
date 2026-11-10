// The fixture for the tests of Studio Kit and of the libraries over it. It holds a kit in Studio's dark theme, for a
// context and a tree that set text in the font stack of Studio's applications: Noto and the Nerd Fonts symbols. Its
// frames declare a root surface and the test's widgets.

#pragma once

#include "pr/array.h"
#include "pr/callback.h"
#include "pr/compress.h"
#include "pr/count_of.h"
#include "pr/draw_commands.h"
#include "pr/draw_glyph.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/font_bundle.h"
#include "pr/font_set.h"
#include "pr/function.h"
#include "pr/image.h"
#include "pr/kit.h"
#include "pr/kit_surface.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_tree.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/text_harfbuzz.h"
#include "pr/ui.h"

#include "tools/harness/harness_png.h"

#include "tools/harness/test.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

using namespace pr;

// Returns true if the tree's incremental work matches a layout from scratch, and reports the first difference if not.
inline bool kitVerified(PrLayoutTree* tree) {
    PrLayoutTreeDifference difference{};
    bool same = prLayoutTreeVerify(tree, &difference);
    if (!same) {
        PrStringView what = prRawStringView(&difference.what);
        MESSAGE("node " << uint32_t(difference.node) << ": " << String(what));
        prRawStringReset(&difference.what);
    }
    return same;
}

// The pixels that KitFixture::render starts from, beneath the tree's content. render plays the scene without filling
// the canvas (PrLayoutScenePlayOptions::canvasFilled), so these pixels stand in for it.
enum {
    KitCanvas_Cleared,          // the canvas color, as a Studio window's pass clears to it (KitFixture::canvas)
    KitCanvas_Unfilled,         // transparent, so that the pixels show only what the content draws
};
typedef uint8_t KitCanvas;

// The fonts of a fixture's font set: the first `bundleCount` of `bundles`, which it installs, and the font system that
// it queries, if any.
struct KitFonts {
    PrFontBundle bundles[4] = {PrFontBundle_Complete, PrFontBundle_NerdFontSymbols};
    uint32_t bundleCount = 2;
    PrFontSystem* PR_NULLABLE system = nullptr;
};

// A kit with a context and tree of its own. Each frame declares the root and then runs the test's build.
struct KitFixture {
    PrFontSet* fonts;
    PrTextShaper* shaper;
    PrLayoutTree* tree;
    PrUi* ui;
    PrKit* kit;

    // The decoder for glyphs that are images, as a Studio window's renderer has, and the codecs that it uses.
    PrCompressCodec* compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
    PrImageCodec* images = prImageCodecCreate(PrImageCodecBackend_Default, compress);
    PrDrawGlyphDecoder* glyphs = prDrawGlyphBitmapCreate(images);

    PrLayoutNode root = kPrLayoutNodeNone;            // a surface that fills the viewport and holds the build's widgets
    PrKitBackground background = PrKitBackground_App; // what the root is drawn on
    double time = 0;
    PrAppDisplay display = prAppDisplayDefault();  // the display that each frame is shown on
    PrAppUserPreferences preferences = prAppUserPreferencesDefault();
    Function<void(PrLayoutNode root)> build;

    // The edit sink that each frame sets on the root, if `edit` is not null.
    PrUiEditFn PR_NULLABLE edit = nullptr;
    void* PR_NULLABLE editContext = nullptr;

    explicit KitFixture(const KitFonts& kitFonts = KitFonts{}) {
        fonts = prFontSetCreate(nullptr);
        prFontBundleInstall(fonts, kitFonts.bundles, kitFonts.bundleCount, PR_FONT_BUNDLE_DIR);
        prFontSetSetSystem(fonts, kitFonts.system);
        shaper = prTextHarfBuzzCreate();
        PrLayoutTreeDesc desc{.shaper = shaper, .targetPixelRatio = 1};
        tree = prLayoutTreeCreate(&desc);
        ui = prUiCreate(tree);
        PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Dark, fonts);
        kit = prKitCreate(ui, &theme);
    }

    ~KitFixture() {
        prKitDestroy(kit);
        prUiDestroy(ui);
        prLayoutTreeDestroy(tree);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
        prDrawGlyphDecoderDestroy(glyphs);
        prImageCodecDestroy(images);
        prCompressCodecDestroy(compress);
    }

    // Runs a frame `delta` seconds after the previous one, in a viewport of 800 by 600 px. A frame that gives a key
    // twice fails the test, since each key names one node of the tree.
    uint32_t frame(double delta = 1.0 / 60, uint32_t rounds = 4) {
        time += delta;
        PrUiFrameDesc desc{
            .time = time,
            .delta = delta,
            .extent = PrVec2{float(kRenderWidth), float(kRenderHeight)},
            .maxRounds = rounds,
            .display = display,
            .preferences = preferences,
        };
        auto round = [this](PrUi*, const PrUiRound*) {
            PrKitSurfaceStyle style{
                .box = {
                    .overflow = PrKitOverflow_Clip,
                    .place = {.width = prKitPercent(100), .height = prKitPercent(100)},
                },
                .background = background,
            };
            root = prKitSurface(kit, kPrLayoutNodeNone, {}, style);
            if (edit != nullptr)
                prUiSetEditSink(ui, root, edit, editContext);
            if (build)
                build(root);
        };
        uint32_t ran = prUiFrame(ui, &desc, PR_SCOPED_CALLBACK(round));
        PrLayoutDuplicateKey duplicates[4] = {};
        uint32_t count = prLayoutTreeDuplicateKeys(tree, duplicates, 4);
        for (uint32_t i = 0; i < count && i < 4; ++i) {
            MESSAGE("duplicate key " << duplicates[i].key << " of nodes " << uint32_t(duplicates[i].first) << " and " <<
                uint32_t(duplicates[i].second));
        }
        CHECK(count == 0);
        return ran;
    }

    // Returns the border box of a node, in viewport px, at the last layout.
    PrRect bounds(PrLayoutNode node) {
        return prLayoutRectToPx(prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Viewport));
    }

    // Returns the center of a node's border box.
    PrVec2 center(PrLayoutNode node) {
        PrRect r = bounds(node);
        return PrVec2{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    }

    void push(PrAppEvent event) {
        event.time = time;
        prUiPushEvent(ui, kPrUiViewportMain, &event);
    }
    void move(PrVec2 at) {
        push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .pointer = {.position = at}});
    }
    void click(PrVec2 at, PrAppModifierFlags modifiers = 0, uint32_t clicks = 1) {
        push(PrAppEvent{
            .kind = PrAppEventKind_PointerDown,
            .modifiers = modifiers,
            .pointer = {.position = at, .clicks = clicks},
        });
        push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .modifiers = modifiers, .pointer = {.position = at}});
    }
    void key(PrAppKey key, PrAppModifierFlags modifiers = 0) {
        push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .modifiers = modifiers, .keyboard = {.key = key}});
        push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .modifiers = modifiers, .keyboard = {.key = key}});
    }
    void type(PrStringView text) {
        push(PrAppEvent{.kind = PrAppEventKind_Text, .input = {.text = text}});
    }

    // The extent of the viewport that frames lay out in, and that render draws, in px.
    static constexpr uint32_t kRenderWidth = 800;
    static constexpr uint32_t kRenderHeight = 600;

    // Returns the color that a Studio window would clear to beneath the last frame. It is the scene's canvas color
    // (prLayoutSceneCanvasColor), which comes from the root's background.
    PrColor canvas() {
        const PrLayoutScene* scene = prLayoutTreeScene(tree);
        PrColor color = prColorFromColor16(prLayoutSceneCanvasColor(scene));
        prLayoutSceneRelease(scene);
        return color;
    }

    // Paints the tree as the last frame laid it out and renders it on the CPU, at one target pixel per px. `*rgba`
    // receives kRenderWidth by kRenderHeight pixels of Rgba8, row by row. `canvas` sets the pixels beneath the content.
    // Returns false if rendering fails.
    bool render(Array<uint8_t>* rgba, KitCanvas canvas = KitCanvas_Cleared) {
        PrDrawRenderer* renderer = nullptr;
        PrDrawRendererOptions rendererOptions = prDrawRendererOptionsDefault();
        rendererOptions.glyphDecoder = glyphs;
        if (prDrawRendererCreate(PrDrawRendererBackend_Cpu, nullptr, &rendererOptions, &renderer) != PrStatus_Ok)
            return false;
        PrDrawList* list = prDrawListCreate();
        const PrLayoutScene* scene = prLayoutTreeScene(tree);
        Array<PrLayoutSceneScroll> scrolls(prLayoutSceneScrolls(tree, scene, nullptr, 0));
        prLayoutSceneScrolls(tree, scene, scrolls.data(), scrolls.count());
        PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
        options.scrolls = scrolls.data();
        options.scrollCount = scrolls.count();
        options.canvasFilled = true;
        prLayoutScenePlay(scene, list, &options);
        prLayoutSceneRelease(scene);
        rgba->clear();
        rgba->resize(kRenderWidth * kRenderHeight * 4);
        if (canvas == KitCanvas_Cleared) {
            // Rgba8 holds premultiplied values.
            PrColor fill = this->canvas();
            auto channel = [&](float value) { return uint8_t(value * fill.a * 255 + 0.5f); };
            uint8_t clear[4] = {channel(fill.r), channel(fill.g), channel(fill.b), uint8_t(fill.a * 255 + 0.5f)};
            for (uint32_t i = 0; i < rgba->count(); ++i)
                (*rgba)[i] = clear[i % 4];
        }
        PrPixels pixels{rgba->data(), size_t(kRenderWidth) * 4, {kRenderWidth, kRenderHeight}};
        bool rendered = prDrawRendererRenderPixels(renderer, list, pixels, PrPixelFormat_Rgba8) == PrStatus_Ok;
        prDrawListRelease(list);
        prDrawRendererDestroy(renderer);
        return rendered;
    }

    // Renders the tree (render) and writes it to `path` as a PNG, so that a person can see what a test drove. Returns
    // false if it cannot.
    bool screenshot(const char* path) {
        Array<uint8_t> rgba;
        return render(&rgba) &&
            harnessWritePng(path, PrPixelView{rgba.data(), size_t(kRenderWidth) * 4, {kRenderWidth, kRenderHeight}});
    }

    // Returns the number of commands that painting the tree records, including those in the lists that it embeds.
    uint32_t paintedCommands() {
        PrDrawList* list = prDrawListCreate();
        prLayoutTreePaint(tree, list);
        uint32_t count = prDrawListCommandCount(list);
        prDrawListRelease(list);
        return count;
    }
};

} // namespace harness

PR_ASSUME_NONNULL_END
