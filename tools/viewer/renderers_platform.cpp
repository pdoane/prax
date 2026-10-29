// The renderers that only one platform has: Core Graphics on macOS.

#include "tools/viewer/renderers_platform.h"

#if defined(__APPLE__)
#include "pr/draw_coregraphics.h"
#endif

namespace viewer {

std::vector<PlatformRenderer> createPlatformRenderers(PrDrawGlyphDecoder* PR_NONNULL (*PR_NONNULL glyphDecoder)()) {
    std::vector<PlatformRenderer> configs;
#if defined(__APPLE__)
    PrDrawCoreGraphicsOptions coreGraphics = prDrawCoreGraphicsOptionsDefault();
    coreGraphics.glyphDecoder = glyphDecoder();
    configs.push_back(PlatformRenderer{"coregraphics", prDrawCoreGraphicsCreate(&coreGraphics)});
#else
    (void)glyphDecoder;
#endif
    return configs;
}

} // namespace viewer
