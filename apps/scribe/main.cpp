// Scribe: Studio's text editor, in a window.
//
// main() is the composition root: Scribe's interface in Studio's window (pr/studio_window.h), its text set in the
// monospace face of the window's fonts.
//
// Usage: scribe [--trace] [--debug program] [file | --diff old new]
//
// --diff shows the comparison of two files, each editable. --debug makes lldb-dap the debugger, which launches
// `program` (Debug > Start or Continue Debugging). --trace prints each frame that takes long, by its phases, each long
// gap between frames, and how long input waited for the frame that took it, all in milliseconds.

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/font_bundle.h"
#include "pr/print.h"
#include "pr/string_view.h"
#include "pr/studio_window.h"
#include "pr/text_font.h"

#include "apps/scribe/scribe.h"

using namespace scribe;

namespace {

// How long a frame takes, or how long a gap or input's wait lasts, for --trace to report it, in milliseconds.
constexpr double kTraceSlowFrame = 16;
constexpr double kTraceLongGap = 100;

// What the command line asks for, and the editor that it opens.
struct Run {
    Scribe scribe;
    PrStringView path;
    PrStringView compared;          // the old file of --diff
    PrStringView debuggee;          // the program of --debug
    bool diff = false;
};

Run& runOf(void* context) {
    return *static_cast<Run*>(context);
}

// Makes the editor over the window's kit and its monospace font, and opens the command line's documents.
bool open(void* context, PrStudioWindow* window) {
    Run& r = runOf(context);
    Scribe& s = r.scribe;
    PrFontFamily mono{.generic = PrFontGeneric_Monospace};
    PrFontStyle style = prFontStyleDefault();
    style.size = 13;
    PrTextFontDesc font{.set = prStudioWindowFonts(window), .families = &mono, .familyCount = 1, .style = style};
    s.font = prTextFontCreate(&font);
    s.kit = prStudioWindowKit(window);
    s.ui = prStudioWindowUi(window);
    s.app = prStudioWindowApp(window);
    openEditor(s);
    if (!r.debuggee.isEmpty())
        setDebuggee(s, r.debuggee);
    bool opened = r.diff ? openDiff(s, r.compared, r.path) : openDocument(s, r.path);
    if (!opened && r.diff)
        printError("scribe: cannot read {} or {}\n", r.compared, r.path);
    else if (!opened)
        printError("scribe: cannot read {}\n", r.path);
    return opened;
}

void trace(void*, const PrStudioWindowFrameTimes* t) {
    if (t->total <= kTraceSlowFrame && t->gap <= kTraceLongGap && t->inputWait <= kTraceLongGap)
        return;
    printError("frame {:.0f}x{:.0f}: total {:.1f} (wait {:.1f}, rounds {} {:.1f}, scene {:.1f}, play {:.1f}, "
        "render {:.1f}), gap {:.1f}, input waited {:.1f}\n", t->extent.x, t->extent.y, t->total, t->wait, t->rounds,
        t->build, t->scene, t->play, t->render, t->gap, t->inputWait);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Run r;
    bool tracing = false;
    for (int i = 1; i < argc; ++i) {
        PrStringView argument = argv[i];
        if (argument == "--trace") {
            tracing = true;
        } else if (argument == "--debug" && i + 1 < argc) {
            r.debuggee = argv[++i];
        } else if (argument == "--diff" && i + 2 < argc) {
            r.diff = true;
            r.compared = argv[++i];
            r.path = argv[++i];
        } else if (r.path.isEmpty()) {
            r.path = argument;
        } else {
            printError("usage: scribe [--trace] [--debug program] [file | --diff old new]\n");
            return 2;
        }
    }
    // The text is set in Noto Sans Mono, and the terminal adds icons from Nerd Fonts. Everything else uses the system's
    // fonts.
    PrFontSet* fonts = prFontSetCreate(nullptr);
    const PrFontBundle bundles[] = {PrFontBundle_NotoMono, PrFontBundle_NerdFontSymbols};
    if (prFontBundleInstall(fonts, bundles, uint32_t(countOf(bundles)), PR_FONT_BUNDLE_DIR) == 0) {
        printError("scribe: the fonts under {} could not be installed\n", PR_FONT_BUNDLE_DIR);
        prFontSetDestroy(fonts);
        return 1;
    }
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    prFontSetSetSystem(fonts, system);
    PrStudioWindowDesc desc{
        .title = "Scribe",
        .extent = {1200, 900},
        .fonts = fonts,
        .open = open,
        .build = [](void* context, PrStudioWindow*, PrLayoutNode root, PrVec2 extent) {
            buildScribe(runOf(context).scribe, root, extent);
        },
        .laidOut = [](void* context, PrStudioWindow*) { return afterLayout(runOf(context).scribe); },
        .close = [](void* context, PrStudioWindow*) {
            Scribe& s = runOf(context).scribe;
            if (s.editor != nullptr)
                closeEditor(s);
        },
        .trace = tracing ? trace : nullptr,
        .context = &r,
    };
    PrStatus status = prStudioWindowRun(&desc);
    prFontSetDestroy(fonts);
    prFontSystemDestroy(system);
    if (status == PrStatus_NotSupported)
        printError("scribe: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
