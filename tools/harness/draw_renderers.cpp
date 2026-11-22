// The renderers of lib/draw and its comparisons, in their configurations (draw_renderers.h).

#include "tools/harness/draw_renderers.h"

#include "pr/compress_select.h"
#include "pr/draw_blend2d.h"
#include "pr/draw_cpu.h"
#include "pr/draw_fragment.h"
#include "pr/draw_imgui.h"
#include "pr/draw_triangles.h"
#include "pr/gpu_memory.h"
#include "pr/image_select.h"
#include "pr/svg_glyph.h"

#if defined(__APPLE__)
#include "pr/draw_coregraphics.h"
#endif

#include <cstring>
#include <vector>

namespace harness {

namespace {

PrCompressCodecSet* compressCodec;
PrImageCodecSet* imageCodec;

// The memory device that the renderers in memory share, the cache on that device, and the cache on the GPU device.
PrGpuDevice* memoryDevice;
PrTextureCache* memoryTextures;
PrTextureCache* deviceTextures;

// One decoder for each renderer, because a decoder is used by one thread at a time and panes render renderers
// independently.
std::vector<PrDrawGlyphDecoder*> glyphDecoders;

// Returns a decoder of SVG and bitmap glyphs for one renderer. The images of the glyphs realize in `textures`.
PrDrawGlyphDecoder* glyphDecoder(PrTextureCache* textures) {
    glyphDecoders.push_back(prSvgGlyphDecoderCreate(imageCodec, compressCodec, textures));
    return glyphDecoders.back();
}

// Returns a cache on `device` that packs as `pack` says. The cache has no task pool, so it realizes a source inside the
// ask, and a drawing has its images in its first render.
PrTextureCache* cacheOn(PrGpuDevice* device, PrTextureCachePack pack) {
    PrTextureCacheDesc desc = prTextureCacheDescDefault(device);
    desc.pack = pack;
    return prTextureCacheCreate(&desc);
}

} // namespace

std::vector<RendererConfig> createConfigs(PrGpuDevice* PR_NULLABLE device, PrTextureCachePack pack) {
    compressCodec = prCompressCodecSetCreateDefault();
    imageCodec = prImageCodecSetCreateDefault(compressCodec);
    memoryDevice = prGpuMemoryCreate(nullptr);
    memoryTextures = cacheOn(memoryDevice, pack);
    deviceTextures = device != nullptr ? cacheOn(device, pack) : nullptr;
    PrDrawCpuOptions cpu = prDrawCpuOptionsDefault();
    cpu.glyphDecoder = glyphDecoder(memoryTextures);
    cpu.device = memoryDevice;
    PrDrawBlend2dOptions blend2d = prDrawBlend2dOptionsDefault();
    blend2d.glyphDecoder = glyphDecoder(memoryTextures);
    blend2d.device = memoryDevice;
    // Families in the order that the viewer's number keys choose them.
    std::vector<RendererConfig> configs{RendererConfig{"cpu", prDrawCpuCreate(&cpu), "cpu", 0, memoryTextures}};
    uint32_t family = 1;
    if (device != nullptr) {
        PrDrawTrianglesOptions triangles = prDrawTrianglesOptionsDefault();
        triangles.glyphDecoder = glyphDecoder(deviceTextures);
        PrDrawRenderer* trianglesRenderer = nullptr;
        if (prDrawTrianglesCreate(device, &triangles, &trianglesRenderer) == PrStatus_Ok)
            configs.push_back(RendererConfig{"triangles", trianglesRenderer, "triangles", family++, deviceTextures});
        // Fragment with each way of preparing paths, the default first.
        struct FragmentVariant {
            const char* name;
            PrDrawFragmentPaths paths;
        };
        const FragmentVariant fragments[] = {
            {"fragment-tiles", PrDrawFragmentPaths_Tiles},
            {"fragment-strips", PrDrawFragmentPaths_Strips},
            {"fragment", PrDrawFragmentPaths_Tessellate},
        };
        PrDrawFragmentPaths standard = prDrawFragmentOptionsDefault().paths;
        bool anyFragment = false;
        for (bool first : {true, false}) {
            for (const FragmentVariant& variant : fragments) {
                if ((variant.paths == standard) != first)
                    continue;
                PrDrawFragmentOptions fragment = prDrawFragmentOptionsDefault();
                fragment.glyphDecoder = glyphDecoder(deviceTextures);
                fragment.paths = variant.paths;
                PrDrawRenderer* fragmentRenderer = nullptr;
                if (prDrawFragmentCreate(device, &fragment, &fragmentRenderer) == PrStatus_Ok) {
                    configs.push_back(RendererConfig{
                        variant.name, fragmentRenderer, "fragment", family,
                        deviceTextures,
                    });
                    anyFragment = true;
                }
            }
        }
        if (anyFragment)
            ++family;
    }
    configs.push_back(RendererConfig{"blend2d", prDrawBlend2dCreate(&blend2d), "blend2d", family++, memoryTextures});
#if defined(__APPLE__)
    PrDrawCoreGraphicsOptions coreGraphics = prDrawCoreGraphicsOptionsDefault();
    coreGraphics.glyphDecoder = glyphDecoder(memoryTextures);
    coreGraphics.device = memoryDevice;
    configs.push_back(RendererConfig{
        "coregraphics", prDrawCoreGraphicsCreate(&coreGraphics), "coregraphics",
        family++, memoryTextures,
    });
#endif
    PrDrawRenderer* imgui = nullptr;
    if (device != nullptr && prDrawImGuiCreate(device, &imgui))
        configs.push_back(RendererConfig{"imgui", imgui, "imgui", family++, deviceTextures});
    return configs;
}

void trimConfigs() {
    if (memoryTextures != nullptr)
        prTextureCacheTrim(memoryTextures);
    if (deviceTextures != nullptr)
        prTextureCacheTrim(deviceTextures);
}

void destroyConfigs(std::vector<RendererConfig>& configs) {
    for (RendererConfig& config : configs)
        prDrawRendererDestroy(config.renderer);
    configs.clear();
    for (PrDrawGlyphDecoder* decoder : glyphDecoders)
        prDrawGlyphDecoderDestroy(decoder);
    glyphDecoders.clear();
    prTextureCacheDestroy(memoryTextures);
    prTextureCacheDestroy(deviceTextures);
    prGpuDeviceDestroy(memoryDevice);
    memoryTextures = nullptr;
    deviceTextures = nullptr;
    memoryDevice = nullptr;
    prImageCodecSetDestroy(imageCodec);
    prCompressCodecSetDestroy(compressCodec);
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

PrDrawFrame* PR_NONNULL frameFor(PrDrawFrame* PR_NULLABLE& frame, PrDrawRenderer* PR_NULLABLE& madeBy,
        PrDrawRenderer* PR_NONNULL renderer) {
    if (frame != nullptr && madeBy != renderer) {
        prDrawFrameDestroy(frame);
        frame = nullptr;
    }
    if (frame == nullptr) {
        frame = prDrawFrameCreate(renderer);
        madeBy = renderer;
    }
    return frame;
}

} // namespace harness
