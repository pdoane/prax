// The word guess game: guess a hidden five-letter word in six tries, in Studio's window (pr/studio_window.h).
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

#include "examples/word_guess/word_guess_view.h"

using namespace pr;
using namespace word_guess;

namespace {

// The font that the game packages (examples/CMakeLists.txt), and its family.
constexpr PrStringView kPackagedFont = "font_bundle/NotoSans[wdth,wght].ttf";
constexpr PrStringView kPackagedFamily = "Noto Sans";

// The game, what its view draws with, which lives as long as the app, and the builder of its view, which lives from the
// window's opening to its closing, as its context does.
struct WordGuess {
    WordGuessApp app{};
    UiResources* resources = nullptr;
    UiBuilder* ui = nullptr;
};

WordGuess& wordGuessOf(void* context) {
    return *static_cast<WordGuess*>(context);
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

// Starts the game that the arguments give, if they give one: its answer, then the guesses already made, each five
// letters. Returns false if they give none, or one that is not five letters.
bool startGiven(Game& game, int argc, char** argv) {
    if (argc < 2)
        return false;
    auto upper = [](PrStringView word, char* out) {
        if (word.size != kWordLength)
            return false;
        for (uint32_t i = 0; i < kWordLength; ++i)
            out[i] = char(word.data[i] >= 'a' ? word.data[i] - 'a' + 'A' : word.data[i]);
        return true;
    };
    startGame(game, 0);
    if (!upper(PrStringView(argv[1]), game.answer))
        return false;
    for (int i = 2; i < argc; ++i) {
        char guess[kWordLength];
        if (!upper(PrStringView(argv[i]), guess))
            return false;
        for (char letter : guess)
            typeLetter(game, letter, 0);
        submitGuess(game, 0);
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static WordGuess wordGuess;
    if (startGiven(wordGuess.app.game, argc, argv))
        wordGuess.app.started = true;
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrFontSystem* system = useFonts(fonts);
    wordGuess.resources = create<UiResources>(fonts);
    PrStudioWindowDesc desc{
        .title = "Word Guess",
        .extent = {540, 900},
        .minExtent = {480, 780},
        .open = [](void* context, PrStudioWindow* window) {
            UiBuilderDesc builder{.ui = prStudioWindowUi(window), .resources = wordGuessOf(context).resources};
            wordGuessOf(context).ui = create<UiBuilder>(builder);
            return true;
        },
        .build = [](void* context, PrStudioWindow*, PrLayoutNode root, PrVec2) {
            WordGuess& w = wordGuessOf(context);
            buildWordGuess(w.app, *w.ui, root);
        },
        .close = [](void* context, PrStudioWindow*) {
            destroy(wordGuessOf(context).ui);
            wordGuessOf(context).ui = nullptr;
        },
        .context = &wordGuess,
    };
    PrStudioAppDesc app{.fonts = fonts, .window = &desc};
    PrStatus status = prStudioAppRun(&app);
    destroy(wordGuess.resources);
    prFontSetDestroy(fonts);
    prFontSystemDestroy(system);
    if (status == PrStatus_NotSupported)
        printError("word_guess: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
