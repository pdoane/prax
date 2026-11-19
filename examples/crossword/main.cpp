// The crossword: a grid of clued answers to fill, in Studio's window (pr/studio_window.h).
//
// main() is the composition root: the game, the builder of its elements over the window's context, and the system's
// fonts. Where the build reads no system fonts, as on the web, the game's text is set in a font that it packages.

#include "pr/blob.h"
#include "pr/construct.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/print.h"
#include "pr/resource.h"
#include "pr/studio_app.h"

#include "examples/crossword/crossword_view.h"

using namespace crossword;
using namespace pr;

namespace {

// The font that the game packages (examples/CMakeLists.txt), and its family.
constexpr PrStringView kPackagedFont = "font_bundle/NotoSans[wdth,wght].ttf";
constexpr PrStringView kPackagedFamily = "Noto Sans";

// The game, what its view draws with, which lives as long as the app, and the builder of its view, which lives from the
// window's opening to its closing, as its context does.
struct Crossword {
    CrosswordApp app{.coins = 172, .streak = 1};
    UiResources* resources = nullptr;
    UiBuilder* ui = nullptr;
};

Crossword& crosswordOf(void* context) {
    return *static_cast<Crossword*>(context);
}

// Sets the text of `fonts` in the system's fonts, and returns the system, or else in the packaged font, and returns
// null.
PrFontSystem* PR_NULLABLE useFonts(PrFontSet* fonts) {
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    if (system != nullptr) {
        prFontSetSetSystem(fonts, system);
        return system;
    }
    PrBlob* resource = prResourceOpen(kPackagedFont);
    const PrFont* font = nullptr;
    if (resource != nullptr && prFontCreate(resource, 0, nullptr, &font) == PrStatus_Ok) {
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(fonts, &descriptor, font);
        prFontRelease(font);
        prFontSetSetGeneric(fonts, PrFontGeneric_SystemUi, kPackagedFamily);
    }
    prBlobRelease(resource);
    return nullptr;
}

// Starts the game that the arguments give, if they give one: the index of a puzzle, and whether it starts solved, as
// `solved`. Returns false if they give none.
bool startGiven(CrosswordApp& app, int argc, char** argv) {
    if (argc < 2)
        return false;
    app.puzzle = uint32_t(argv[1][0] - '0') % puzzleCount();
    startGame(app.game, puzzleAt(app.puzzle));
    if (argc > 2 && PrStringView(argv[2]) == "solved") {
        for (uint32_t cell = 0; cell < app.game.size * app.game.size; ++cell) {
            if (app.game.solution[cell] != kBlock)
                app.game.entries[cell] = app.game.solution[cell];
        }
        app.game.hints = 2;
        app.game.elapsed = 728;
        app.game.solved = true;
    } else if (argc > 2) {
        // A part played game: the first answers filled.
        for (uint32_t i = 0; i < 6 && i < app.game.slotCount; ++i) {
            const Slot& slot = app.game.slots[i];
            for (uint32_t k = 0; k < slot.length; ++k) {
                uint32_t cell = slotCell(app.game, slot, k);
                app.game.entries[cell] = app.game.solution[cell];
            }
        }
        app.game.elapsed = 95;
        app.game.cursor = app.game.slots[app.game.slotCount / 2].start;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Crossword crossword;
    startGiven(crossword.app, argc, argv);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrFontSystem* system = useFonts(fonts);
    crossword.resources = create<UiResources>(fonts);
    PrStudioWindowDesc desc{
        .title = "Crossword",
        .extent = {1280, 860},
        .minExtent = {1040, 700},
        .open = [](void* context, PrStudioWindow* window) {
            UiBuilderDesc builder{.ui = prStudioWindowUi(window), .resources = crosswordOf(context).resources};
            crosswordOf(context).ui = create<UiBuilder>(builder);
            return true;
        },
        .build = [](void* context, PrStudioWindow*, PrLayoutNode root, PrVec2 extent) {
            Crossword& c = crosswordOf(context);
            buildCrossword(c.app, *c.ui, root, extent);
        },
        .close = [](void* context, PrStudioWindow*) {
            destroy(crosswordOf(context).ui);
            crosswordOf(context).ui = nullptr;
        },
        .context = &crossword,
    };
    PrStudioAppDesc app{.fonts = fonts, .window = &desc};
    PrStatus status = prStudioAppRun(&app);
    destroy(crossword.resources);
    prFontSetDestroy(fonts);
    prFontSystemDestroy(system);
    if (status == PrStatus_NotSupported)
        printError("crossword: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
