// Terminal: terminals in tabs, in Studio's windows.
//
// main() is the composition root. It runs the app's interface (apps/terminal/terminal_app.h) in a Studio application of
// Multiple windows (pr/studio_app.h), and sets the terminals in the monospace face of the application's fonts.
//
// Usage: terminal [--grid COLUMNSxROWS] [--scrollback LINES] [program [arguments...]]
//
// Each tab runs the program, or the user's shell if no program is given. --grid sizes the first window for that many
// columns and rows, once its first frame knows the cell size. --scrollback sets the number of lines of scrollback that
// each tab keeps, up to kPrVtermMaxScrollback. A tab closes once its program exits, a window closes when its last tab
// closes, and the app quits when its last window closes.

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/font_bundle.h"
#include "pr/integer.h"
#include "pr/print.h"
#include "pr/string_ops.h"
#include "pr/string_view.h"
#include "pr/studio_app.h"
#include "pr/vterm.h"

#include "apps/terminal/terminal_app.h"

using namespace terminal;

namespace {

TerminalApp& appOf(void* context) {
    return *static_cast<TerminalApp*>(context);
}

// Opens the app's first window. A first tab that cannot start fails the run.
bool start(void* context, PrStudioApp* studio) {
    TerminalApp& t = appOf(context);
    t.studio = studio;
    TerminalWindow* w = newWindow(t, nullptr);
    return w != nullptr && startWindow(*w);
}

// The largest number of columns or rows that --grid accepts.
constexpr uint64_t kGridMax = 99999;

// Parses a decimal number in the range [`least`, `most`] into `out`. Returns false if the text is not such a number.
bool parseNumber(PrStringView text, uint64_t least, uint64_t most, uint32_t* out) {
    uint64_t value = 0;
    if (!prU64Parse(text.data, text.size, &value, 10) || value < least || value > most)
        return false;
    *out = uint32_t(value);
    return true;
}

// Parses `COLUMNSxROWS` into `out`. Returns false unless both numbers are from 1 to kGridMax.
bool parseGrid(PrStringView text, PrUVec2* out) {
    size_t x = prStringFindByte(text, 'x');
    PrUVec2 grid{};
    if (x == text.size || !parseNumber(prStringSlice(text, 0, x), 1, kGridMax, &grid.x) ||
            !parseNumber(prStringSlice(text, x + 1, text.size - x - 1), 1, kGridMax, &grid.y))
        return false;
    *out = grid;
    return true;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static TerminalApp t;
    int first = 1;
    while (argc > first + 1 && argv[first][0] == '-' && argv[first][1] == '-') {
        PrStringView option = argv[first];
        PrStringView value = argv[first + 1];
        if (option == "--grid") {
            if (!parseGrid(value, &t.grid)) {
                printError("terminal: --grid takes COLUMNSxROWS, as in 120x40\n");
                return 2;
            }
        } else if (option == "--scrollback") {
            if (!parseNumber(value, 0, kPrVtermMaxScrollback, &t.scrollback)) {
                uint32_t most = kPrVtermMaxScrollback;
                printError("terminal: --scrollback takes a number of lines, up to {}\n", most);
                return 2;
            }
        } else {
            break;
        }
        first += 2;
    }
    if (argc > first) {
        t.program = argv[first];
        for (int i = first + 1; i < argc; ++i)
            t.arguments.push(String(argv[i]));
    }
    // The terminal's text is set in Noto Sans Mono, with icons from Nerd Fonts. Everything else uses the system's
    // fonts.
    PrFontSet* fonts = prFontSetCreate(nullptr);
    const PrFontBundle bundles[] = {PrFontBundle_NotoMono, PrFontBundle_NerdFontSymbols};
    if (prFontBundleInstall(fonts, bundles, uint32_t(countOf(bundles)), PR_FONT_BUNDLE_DIR) == 0) {
        printError("terminal: the fonts under {} could not be installed\n", PR_FONT_BUNDLE_DIR);
        prFontSetDestroy(fonts);
        return 1;
    }
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    prFontSetSetSystem(fonts, system);
    t.fonts = fonts;
    openApp(t);
    PrAppOptions options = prAppOptionsDefault();
    options.windowModel = PrAppWindowModel_Desktop;
    PrStudioAppDesc app{
        .fonts = fonts,
        .options = &options,
        .start = start,
        .event = [](void* context, PrStudioApp*, const PrAppEvent* event) { handleEvent(appOf(context), *event); },
        .context = &t,
    };
    PrStatus status = prStudioAppRun(&app);
    closeApp(t);
    prFontSetDestroy(fonts);
    prFontSystemDestroy(system);
    if (status == PrStatus_NotSupported)
        printError("terminal: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
