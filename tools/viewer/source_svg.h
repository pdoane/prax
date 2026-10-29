// SVG documents as sources (documents.h): how they load and draw, for the switches in documents.cpp. Nothing outside
// those switches calls them. A loader returns null for a file it cannot read.

#pragma once

#include "pr/draw_list.h"
#include "pr/font.h"
#include "pr/text.h"

#include <filesystem>
#include <memory>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// An SVG document, rendered as the settings file specifies (tools/suite/suite_render.h), or with the defaults if there
// is none, with its text shaped by `shaper`, which outlives it, and its fonts hinted as `hinting` says. It loads the
// resources that it references from beside it.
struct SvgSource;
std::shared_ptr<SvgSource> loadSvg(const std::filesystem::path& path, const std::filesystem::path& settings,
                                   PrTextShaper* shaper, PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter);
void drawSvg(SvgSource& svg, PrDrawList* list, float width, float height);

} // namespace viewer

PR_ASSUME_NONNULL_END
