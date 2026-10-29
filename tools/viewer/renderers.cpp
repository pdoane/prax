// The renderers that the viewer and draw_bench render pages with.

#include "tools/viewer/renderers.h"

#include "pr/compress_builtin.h"
#include "pr/draw_blend2d.h"
#include "pr/draw_cpu.h"
#include "pr/draw_fragment.h"
#include "pr/draw_imgui.h"
#include "pr/draw_triangles.h"
#include "pr/image_builtin.h"
#include "pr/svg_glyph.h"

#include "tools/viewer/renderers_platform.h"

#include <cstring>
#include <vector>

namespace viewer {

namespace {

PrCompressCodec* compressCodec;
PrImageCodec* imageCodec;

// One decoder for each renderer, because a decoder is used by one thread at a time and panes render renderers
// independently.
std::vector<PrDrawGlyphDecoder*> glyphDecoders;

// Returns a decoder of SVG and bitmap glyphs for one renderer.
PrDrawGlyphDecoder* glyphDecoder() {
    glyphDecoders.push_back(prSvgGlyphDecoderCreate(imageCodec, compressCodec));
    return glyphDecoders.back();
}

} // namespace

std::vector<RendererConfig> createConfigs(PrGpuDevice* PR_NULLABLE device) {
    compressCodec = prCompressBuiltinCreate();
    imageCodec = prImageBuiltinCreate(compressCodec);
    PrDrawCpuOptions cpu = prDrawCpuOptionsDefault();
    cpu.glyphDecoder = glyphDecoder();
    PrDrawBlend2dOptions blend2d = prDrawBlend2dOptionsDefault();
    blend2d.glyphDecoder = glyphDecoder();
    // Families in the order that the viewer's number keys choose them.
    std::vector<RendererConfig> configs{RendererConfig{"cpu", prDrawCpuCreate(&cpu), "cpu", 0}};
    uint32_t family = 1;
    if (device != nullptr) {
        PrDrawTrianglesOptions triangles = prDrawTrianglesOptionsDefault();
        triangles.glyphDecoder = glyphDecoder();
        PrDrawRenderer* trianglesRenderer = nullptr;
        if (prDrawTrianglesCreate(device, &triangles, &trianglesRenderer) == PrStatus_Ok)
            configs.push_back(RendererConfig{"triangles", trianglesRenderer, "triangles", family++});
        // Fragment with each way of preparing paths, the default first.
        struct FragmentVariant {
            const char* name;
            PrDrawFragmentPaths paths;
        };
        const FragmentVariant fragments[] = {{"fragment-tiles", PrDrawFragmentPaths_Tiles},
                                             {"fragment-strips", PrDrawFragmentPaths_Strips},
                                             {"fragment", PrDrawFragmentPaths_Tessellate}};
        PrDrawFragmentPaths standard = prDrawFragmentOptionsDefault().paths;
        bool anyFragment = false;
        for (bool first : {true, false}) {
            for (const FragmentVariant& variant : fragments) {
                if ((variant.paths == standard) != first)
                    continue;
                PrDrawFragmentOptions fragment = prDrawFragmentOptionsDefault();
                fragment.glyphDecoder = glyphDecoder();
                fragment.paths = variant.paths;
                PrDrawRenderer* fragmentRenderer = nullptr;
                if (prDrawFragmentCreate(device, &fragment, &fragmentRenderer) == PrStatus_Ok) {
                    configs.push_back(RendererConfig{variant.name, fragmentRenderer, "fragment", family});
                    anyFragment = true;
                }
            }
        }
        if (anyFragment)
            ++family;
    }
    configs.push_back(RendererConfig{"blend2d", prDrawBlend2dCreate(&blend2d), "blend2d", family++});
    for (const PlatformRenderer& platform : createPlatformRenderers(glyphDecoder))
        configs.push_back(RendererConfig{platform.name, platform.renderer, platform.name, family++});
    PrDrawRenderer* imgui = nullptr;
    if (device != nullptr && prDrawImGuiCreate(device, &imgui))
        configs.push_back(RendererConfig{"imgui", imgui, "imgui", family++});
    return configs;
}

void destroyConfigs(std::vector<RendererConfig>& configs) {
    for (RendererConfig& config : configs)
        prDrawRendererDestroy(config.renderer);
    configs.clear();
    for (PrDrawGlyphDecoder* decoder : glyphDecoders)
        prDrawGlyphDecoderDestroy(decoder);
    glyphDecoders.clear();
    prImageCodecDestroy(imageCodec);
    prCompressCodecDestroy(compressCodec);
    imageCodec = nullptr;
    compressCodec = nullptr;
}

uint32_t defaultConfig(const std::vector<RendererConfig>& configs) {
    for (uint32_t b = 0; b < uint32_t(configs.size()); ++b)
        if (std::strcmp(configs[b].familyName, "fragment") == 0)
            return b;
    return 0;
}

uint32_t familyCount(const std::vector<RendererConfig>& configs) {
    return configs.empty() ? 0 : configs.back().family + 1;
}

uint32_t nextInFamily(const std::vector<RendererConfig>& configs, uint32_t index) {
    uint32_t family = configs[index].family;
    if (index + 1 < configs.size() && configs[index + 1].family == family)
        return index + 1;
    while (index > 0 && configs[index - 1].family == family)
        --index;
    return index;
}

PrDrawFrame* PR_NONNULL frameFor(PrDrawFrame* PR_NULLABLE& frame, PrDrawRenderer* PR_NONNULL renderer) {
    if (frame != nullptr && frame->renderer != renderer) {
        prDrawFrameDestroy(frame);
        frame = nullptr;
    }
    if (frame == nullptr)
        frame = prDrawFrameCreate(renderer);
    return frame;
}

} // namespace viewer
