// The fixture for the tests of Studio Kit and of the libraries over it. It holds a kit in Studio's dark theme, for a
// context and a tree that set text in the font stack of Studio's applications: Noto and the Nerd Fonts symbols. Its
// frames declare a root surface and the test's widgets.

#pragma once

#include "pr/callback.h"
#include "pr/count_of.h"
#include "pr/draw_commands.h"
#include "pr/draw_list.h"
#include "pr/font_bundle.h"
#include "pr/font_set.h"
#include "pr/kit.h"
#include "pr/kit_surface.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_tree.h"
#include "pr/string_view.h"
#include "pr/text_harfbuzz.h"
#include "pr/ui.h"

#include "tools/harness/test.h"

#include <functional>
#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// Returns true if the tree's incremental work matches a layout from scratch, and reports the first difference if not.
inline bool kitVerified(PrLayoutTree* tree) {
    PrLayoutTreeDifference difference{};
    bool same = prLayoutTreeVerify(tree, &difference);
    if (!same) {
        PrStringView what = prRawStringView(&difference.what);
        MESSAGE("node " << uint32_t(difference.node) << ": " << std::string(what.data, what.size));
        prRawStringReset(&difference.what);
    }
    return same;
}

// The fonts of a fixture's font set: the bundles that it installs, and the font system that it queries, if any.
struct KitFonts {
    std::vector<PrFontBundle> bundles{PrFontBundle_Complete, PrFontBundle_NerdFontSymbols};
    PrFontSystem* PR_NULLABLE system = nullptr;
};

// A kit with a context and tree of its own. Each frame declares the root and then runs the test's build.
struct KitFixture {
    PrFontSet* fonts;
    PrTextShaper* shaper;
    PrLayoutTree* tree;
    PrUi* ui;
    PrKit* kit;
    PrLayoutNode root = kPrLayoutNodeNone;   // a surface that fills the viewport and holds the build's widgets
    PrKitGround ground = PrKitGround_App;    // what the root is drawn on
    double time = 0;
    PrAppDisplay display = prAppDisplayDefault();  // the display that each frame is shown on
    PrAppUserPreferences preferences{};
    std::function<void(PrLayoutNode root)> build;

    // The edit sink that each frame sets on the root, if `edit` is not null.
    PrUiEditFn PR_NULLABLE edit = nullptr;
    void* PR_NULLABLE editContext = nullptr;

    explicit KitFixture(const KitFonts& kitFonts = KitFonts{}) {
        fonts = prFontSetCreate(nullptr);
        prFontBundleInstall(fonts, kitFonts.bundles.data(), uint32_t(kitFonts.bundles.size()), PR_FONT_BUNDLE_DIR);
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
    }

    // Runs a frame `delta` seconds after the previous one, in a viewport of 800 by 600 px. A frame that gives a key
    // twice fails the test, since each key names one node of the tree.
    uint32_t frame(double delta = 1.0 / 60, uint32_t rounds = 4) {
        time += delta;
        PrUiFrameDesc desc{
            .time = time,
            .delta = delta,
            .extent = PrVec2{800, 600},
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
                .ground = ground,
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
        prUiPushEvent(ui, &event);
    }
    void move(PrVec2 at) {
        push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .position = at});
    }
    void click(PrVec2 at, PrAppModifierFlags modifiers = 0, uint32_t clicks = 1) {
        push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .modifiers = modifiers, .clicks = clicks, .position = at});
        push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .modifiers = modifiers, .position = at});
    }
    void key(PrAppKey key, PrAppModifierFlags modifiers = 0) {
        push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .modifiers = modifiers, .key = key});
        push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .modifiers = modifiers, .key = key});
    }
    void type(PrStringView text) {
        push(PrAppEvent{.kind = PrAppEventKind_Text, .text = text});
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
