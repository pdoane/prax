// The renderers that only one platform has, which createConfigs lists after those that every platform has.
// renderers_platform.cpp defines them.

#pragma once

#include "pr/draw_glyph.h"
#include "pr/draw_renderer.h"

#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// A renderer of the platform's own, which is a family of one configuration.
struct PlatformRenderer {
    const char* PR_NONNULL name;
    PrDrawRenderer* PR_NONNULL renderer;
};

// Creates the platform's own renderers, in the order that the viewer's number keys choose them. Each renderer that
// decodes glyphs takes a decoder from `glyphDecoder`, which createConfigs destroys.
std::vector<PlatformRenderer> createPlatformRenderers(PrDrawGlyphDecoder* PR_NONNULL (*PR_NONNULL glyphDecoder)());

} // namespace viewer

PR_ASSUME_NONNULL_END
