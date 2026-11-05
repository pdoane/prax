// rlottie playing Lottie animations as sources (documents.h): how they load and draw, for the switches in
// documents.cpp. Nothing outside those switches calls them. A loader returns null for a file it cannot read.

#pragma once

#include "pr/base.h"
#include "pr/vec.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// rlottie playing a Lottie animation, which renders its own pixels: the frame that it plays at the time, fitted to a
// viewport of `viewport` CSS pixels and centered (fittedIn), over white. The pixels are `width` by `height` target
// pixels from the viewport's top left, at `pixelRatio` target pixels per CSS pixel.
struct RlottieSource;
std::shared_ptr<RlottieSource> loadRlottie(const std::filesystem::path& path);
PrVec2 rlottieSize(const RlottieSource& rlottie);
double rlottieFrameAt(const RlottieSource& rlottie, double time);
void rlottiePixels(RlottieSource& rlottie, PrVec2 viewport, uint32_t width, uint32_t height, float pixelRatio,
    double frame, std::vector<uint8_t>& bgra);

} // namespace viewer

PR_ASSUME_NONNULL_END
