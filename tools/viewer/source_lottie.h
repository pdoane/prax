// Lottie animations as sources (documents.h): how they load and draw, for the switches in documents.cpp. Nothing
// outside those switches calls them. A loader returns null for a file it cannot read.

#pragma once

#include "pr/draw_list.h"
#include "pr/font.h"
#include "pr/text.h"
#include "pr/vec.h"

#include <filesystem>
#include <memory>
#include <string>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// A Lottie animation, with its text set in the fonts of its suite's settings file (tools/suite/suite_render.h), or the
// system's without one, hinted as `hinting` says, and shaped by `shaper`, drawn fitted to a viewport of `viewport` CSS
// pixels and centered (fittedIn) over white, as an image page shows an image and the players place an animation; and
// the page of a player in tools/suite/lottie that plays it in the browser.
struct LottieSource;
std::shared_ptr<LottieSource> loadLottie(const std::filesystem::path& path, const std::filesystem::path& settings,
    PrTextShaper* shaper, PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter);
PrVec2 lottieSize(const LottieSource& lottie);
double lottieFrameAt(const LottieSource& lottie, double time);
void drawLottie(LottieSource& lottie, PrDrawList* list, PrVec2 viewport, double frame);
std::string lottiePlayerUrl(const char* page, const std::filesystem::path& animation);

} // namespace viewer

PR_ASSUME_NONNULL_END
