// rlottie playing Lottie animations as sources (documents.h): how they load and draw, for the switches in
// documents.cpp. Nothing outside those switches calls them. A loader returns null for a file it cannot read.

#pragma once

#include "pr/base.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// rlottie playing a Lottie animation, which renders its own pixels: the whole frame that it plays at the time, over
// white.
struct RlottieSource;
std::shared_ptr<RlottieSource> loadRlottie(const std::filesystem::path& path);
double rlottieFrameAt(const RlottieSource& rlottie, double time);
void rlottiePixels(RlottieSource& rlottie, uint32_t width, uint32_t height, double frame, std::vector<uint8_t>& bgra);

} // namespace viewer

PR_ASSUME_NONNULL_END
