// PNGs as sources (documents.h): how they load and draw, for the switches in documents.cpp. Nothing outside those
// switches calls them. A loader returns null for a file it cannot read.

#pragma once

#include "pr/draw_list.h"
#include "pr/vec.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// A PNG, stretched over the size it draws at and sampled by nearest texel. At its own size, its pixels are given
// unchanged: premultiplied BGRA over white.
struct LoadedImage;
std::shared_ptr<LoadedImage> loadImage(const std::filesystem::path& path);
PrVec2 imageSize(const LoadedImage& image);
void drawImage(LoadedImage& image, PrDrawList* list, float width, float height);
bool imagePixels(LoadedImage& image, uint32_t width, uint32_t height, std::vector<uint8_t>& bgra);

} // namespace viewer

PR_ASSUME_NONNULL_END
