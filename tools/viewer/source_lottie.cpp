// Lottie animations as sources (source_lottie.h).

#include "tools/viewer/source_lottie.h"

#include "pr/compress_builtin.h"
#include "pr/font_set.h"
#include "pr/image_builtin.h"
#include "pr/lottie.h"

#include "tools/suite/suite_render.h"
#include "tools/viewer/documents.h"

#include <cmath>
#include <deque>

using namespace pr;
using namespace suite;

namespace viewer {

namespace fs = std::filesystem;

namespace {

constexpr const char* kThirdParty = PR_THIRD_PARTY_DIR;

// The files that an animation names, beside it, kept until they are read.
struct Beside {
    fs::path directory;
    std::deque<std::string> read;       // keeps each file's bytes at a stable address
};

bool loadBeside(void* PR_NULLABLE context, PrStringView path, PrByteView* out) {
    Beside* beside = static_cast<Beside*>(context);
    fs::path file = beside->directory / std::string(path.data, path.size);
    if (!fs::exists(file))
        return false;
    beside->read.push_back(readFile(file));
    const std::string& bytes = beside->read.back();
    *out = PrByteView{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
    return true;
}

// Returns the fonts that Lottie text is set in without a suite's settings: the system's fonts, as a browser has them.
// The set is created when an animation is first loaded, and kept for the next.
PrFontSet* lottieFonts() {
    static PrFontSet* fonts = nullptr;
    if (fonts == nullptr) {
        fonts = prFontSetCreate(nullptr);
        prFontSetSetSystem(fonts, prFontSystemCreate(PrFontSystemBackend_Default));
        prFontSetSetDefaultFamily(fonts, prFontFamilyFromName("Times"));
    }
    return fonts;
}

} // namespace

struct LottieSource {
    PrLottieAnimation* PR_NULLABLE animation = nullptr;
    SuiteRender render{};       // the suite's settings, which outlive the animation that sets text in their fonts

    // Without settings, a set over the system's fonts that hints them as the source asks, which outlives the animation.
    PrFontSet* PR_NULLABLE system = nullptr;

    LottieSource() = default;
    LottieSource(const LottieSource&) = delete;
    LottieSource& operator=(const LottieSource&) = delete;
    ~LottieSource() {
        prLottieAnimationDestroy(animation);
        suiteRenderReset(&render);
        prFontSetDestroy(system);
    }
};

std::shared_ptr<LottieSource> loadLottie(const fs::path& path, const fs::path& settings, PrTextShaper* shaper,
        PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter) {
    std::string json = readFile(path);
    Beside beside{path.parent_path(), {}};
    PrCompressCodec* compressCodec = prCompressBuiltinCreate();
    PrImageCodec* imageCodec = prImageBuiltinCreate(compressCodec);
    PrLottieOptions options{};
    options.codec = imageCodec;
    options.load = loadBeside;
    options.loadContext = &beside;
    std::shared_ptr<LottieSource> lottie = std::make_shared<LottieSource>();
    if (!settings.empty())
        suiteRenderRead(&lottie->render, settings.string(), fs::path(kThirdParty).parent_path().string());
    if (lottie->render.fonts == nullptr)
        lottie->system = prFontSetCreate(lottieFonts());
    PrFontSet* fonts = lottie->render.fonts != nullptr ? lottie->render.fonts : lottie->system;
    prFontSetSetHinting(fonts, hinting, hinter);
    options.fonts = fonts;
    options.shaper = shaper;
    prLottieAnimationCreate(json, &options, &lottie->animation);
    prImageCodecDestroy(imageCodec);
    prCompressCodecDestroy(compressCodec);
    return lottie->animation != nullptr ? lottie : nullptr;
}

PrVec2 lottieSize(const LottieSource& lottie) {
    return prLottieAnimationInfo(lottie.animation).extent;
}

double lottieFrameAt(const LottieSource& lottie, double time) {
    return prLottieAnimationFrameAt(lottie.animation, time);
}

void drawLottie(LottieSource& lottie, PrDrawList* list, PrVec2 viewport, double frame) {
    prDrawListRect(list, PrRect{0, 0, viewport.x, viewport.y}, {.color = PrColor{1, 1, 1, 1}});
    PrVec2 extent = lottieSize(lottie);
    PrRect fitted = fittedIn(extent, viewport);
    float scale = extent.x > 0 ? prRectWidth(fitted) / extent.x : 1;
    PrAffine2 placed = prAffine2Concat(prAffine2Scale(PrVec2{scale, scale}),
        prAffine2Translate(PrVec2{fitted.left, fitted.top}));
    prDrawListPushTransformAffine2(list, placed);
    prLottieAnimationDraw(lottie.animation, frame, list);
    prDrawListPopTransform(list);
}

std::string lottiePlayerUrl(const char* page, const fs::path& animation) {
    fs::path player = fs::path(kThirdParty).parent_path() / "tools" / "suite" / "lottie" / page;
    return fileUrl(player) + "?src=" + queryEscaped(fileUrl(animation));
}

} // namespace viewer
