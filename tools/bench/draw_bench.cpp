// draw_bench: measures what recording and rendering each drawing costs, by renderer.
//
// ./dev.sh bench draw runs this and records its rows. A row is a drawing of a collection on one renderer, rendered at
// its collection's size times the pixel ratio, onto pixels in memory or, for a renderer that renders on a device, onto
// a texture of that size. A row reports what rendering cost the CPU, and for a device, the time that the device spent
// executing it. A render on a device waits for the device before the next render, so that what the engine does is the
// same from run to run. A frame prepared while the device works on another frame can find the resources it reuses busy,
// and would then make them anew. A row also reports what the render made (PrDrawStats). The drawing's first row also
// reports what recording it cost. Recording and rendering are measured by harnessMeasure, as the engine's other
// benchmarks measure theirs, after a render that warms the caches that a steady frame finds warm (fonts, glyph lists,
// textures).
//
// `draw_bench [--renderer NAME]... [--collection WORD]... [--pixel-ratio <pixels>] [--shaper NAME] [DOCUMENT]...`
// measures the named renderers, the collections whose names contain the words, and the documents named by number or by
// a word of the name, shaping text with the named shaper. By default it measures every feature drawing on the viewer's
// default renderer at a pixel ratio of 2, shaping with HarfBuzz. The collections are those drawn in code.
//
// A renderer on a device renders each drawing twice, each in a row of its own: to 8 bits, and to half floats (`kHdr`),
// whose rows end in "hdr". A renderer in memory renders to 8 bits.
//
// `--images DIR` also writes what each row rendered into DIR, as a PNG named for the row. A half-float render is
// clamped to 8 bits.

#include "pr/assert.h"
#include "pr/compress_builtin.h"
#include "pr/draw_commands.h"
#include "pr/float16.h"
#include "pr/gpu.h"
#include "pr/image_builtin.h"
#include "pr/vec.h"

#include "tools/harness/harness.h"
#include "tools/viewer/documents.h"
#include "tools/viewer/drawing.h"
#include "tools/viewer/features.h"
#include "tools/viewer/panes.h"
#include "tools/viewer/renderers.h"
#include "tools/viewer/suites.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace pr;
using namespace harness;
using namespace viewer;

namespace {

// What a render on a device draws into: the texture's format, named by a suffix of the row.
struct BenchOutput {
    const char* suffix;
    PrPixelFormat format;
};

// An 8-bit render, and one in half floats.
constexpr BenchOutput kSdr{"", PrPixelFormat_Bgra8};
constexpr BenchOutput kHdr{" hdr", PrPixelFormat_RgbaF16};

double nowMillis() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Times of one render, in milliseconds.
struct Times {
    double cpu = 0;             // until the work is submitted
    double gpu = 0;             // the device executing the work
};

// Returns the cost of one call of `run` (harnessMeasure), and the mean of the times that the measured calls report.
// harnessMeasure's first call warms the caches and is not measured.
template <typename Run>
HarnessCounters measure(Run run, Times* mean) {
    Times sum{};
    uint32_t calls = 0;
    HarnessCounters cost = harnessMeasure([&] {
        Times t = run();
        if (calls++ == 0)
            return;
        sum.cpu += t.cpu;
        sum.gpu += t.gpu;
    });
    uint32_t measured = calls > 1 ? calls - 1 : 1;
    *mean = Times{sum.cpu / measured, sum.gpu / measured};
    return cost;
}

bool matches(const std::vector<const char*>& words, const std::string& name) {
    if (words.empty())
        return true;
    for (const char* w : words)
        if (fileNameOf(name).find(fileNameOf(w)) != std::string::npos)
            return true;
    return false;
}

bool named(const Collection& collection, uint32_t index, const std::vector<const char*>& which) {
    if (which.empty())
        return true;
    for (const char* w : which)
        if (documentNamed(collection, index, w))
            return true;
    return false;
}

void record(PrDrawList* list, Loaded& loaded, const Collection& collection, float pixelRatio) {
    prDrawListClear(list);
    prDrawListPushTransformAffine2(list, PrAffine2{pixelRatio, 0, 0, pixelRatio, 0, 0});
    update(loaded, 0);
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = pixelRatio;
    draw(loaded, list, collection.width, collection.height, display, PrAppUserPreferences{});
    prDrawListPopTransform(list);
}

// Where a render goes: pixels in memory, or a texture on the device, which rests in the accesses of shader reads.
struct BenchTarget {
    std::vector<uint8_t> pixels;
    uint32_t width;
    uint32_t height;
    PrGpuDevice* PR_NULLABLE device;
    PrGpuTexture texture;

    BenchOutput output;             // on a device: what the texture holds
};

// Renders the list over the background onto the target, and waits for completion. In memory, the host fills the pixels
// with the background first. On the device, the pass clears the texture to the background, and the texture lies beneath
// the list for backdrops. On a device, `frame` holds the render's frame, which it keeps for the next render with the
// same renderer.
Times render(PrDrawRenderer* renderer, PrDrawFrame* PR_NULLABLE& frame, BenchTarget& target, PrColor color,
        const std::vector<uint8_t>& background, const PrDrawList* list, PrStatus* status) {
    if (prDrawRendererDevice(renderer) != target.device) {
        target.pixels = background;
        PrPixels into{target.pixels.data(), size_t(target.width) * 4, {target.width, target.height}};
        double started = nowMillis();
        *status = prDrawRendererRenderPixels(renderer, list, into, PrPixelFormat_Bgra8);
        return Times{nowMillis() - started, 0};
    }
    double started = nowMillis();
    PrGpuCommands* commands = nullptr;
    if (!prGpuCommandsBegin(target.device, nullptr, &commands)) {
        *status = PrStatus_Lost;
        return Times{};
    }
    PrDrawBeneath beneath{};
    beneath.color = color;
    PrDrawTarget prepared{
        .extent = {target.width, target.height},
        .format = target.output.format,
        .commands = commands,
        .beneath = &beneath,
    };
    *status = prDrawFramePrepare(frameFor(frame, renderer), list, &prepared);
    if (*status == PrStatus_Ok) {
        // The pass clears to the background: the color of its first pixel, which is premultiplied BGRA, reordered as
        // RGBA.
        PrGpuSubresources all{};
        prGpuCommandsTextureBarrier(commands, target.texture, all, PrGpuAccessFlag_ShaderRead,
            PrGpuAccessFlag_ColorAttachment);
        PrColor clear{
            float(background[2]) / 255, float(background[1]) / 255, float(background[0]) / 255,
            float(background[3]) / 255,
        };
        PrGpuPassDesc desc = prGpuPassDescColor(target.texture, PrGpuLoad_Clear, clear);
        PrGpuPass* pass = prGpuPassBegin(commands, &desc);
        prDrawFrameRender(frame, pass, PrIVec2{0, 0});
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target.texture, all, PrGpuAccessFlag_ColorAttachment,
            PrGpuAccessFlag_ShaderRead);
    }
    double submitted = nowMillis();
    double gpu = prGpuCommandsSubmitAndWait(commands) * 1000;
    return Times{submitted - started, gpu};
}

// Writes what the target holds after a render as a PNG at `path`. The pixels are premultiplied, which a PNG reads as
// straight, so only an opaque render is exact.
void writeImage(PrImageCodec* codec, const BenchTarget& target, const std::string& path) {
    size_t count = size_t(target.width) * target.height;
    std::vector<uint8_t> rgba(count * 4);
    if (target.texture.id == 0) {
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* bgra = &target.pixels[i * 4];
            uint8_t* to = &rgba[i * 4];
            to[0] = bgra[2];
            to[1] = bgra[1];
            to[2] = bgra[0];
            to[3] = bgra[3];
        }
    } else {
        uint32_t bytes = prPixelFormatSize(target.output.format);
        std::vector<uint8_t> texels(count * bytes);
        prGpuTextureRead(target.device, target.texture, PrGpuAccessFlag_ShaderRead, PrIVec2{0, 0},
            PrPixels{texels.data(), size_t(target.width) * bytes, {target.width, target.height}});
        for (size_t i = 0; i < count; ++i) {
            uint8_t* to = &rgba[i * 4];
            if (target.output.format == PrPixelFormat_Bgra8) {
                to[0] = texels[i * 4 + 2];
                to[1] = texels[i * 4 + 1];
                to[2] = texels[i * 4 + 0];
                to[3] = texels[i * 4 + 3];
                continue;
            }
            const PrFloat16* half = reinterpret_cast<const PrFloat16*>(&texels[i * 8]);
            for (int c = 0; c < 4; ++c) {
                float v = prFloat16ToFloat(half[c]);
                v = v < 0 ? 0 : v > 1 ? 1 : v;
                to[c] = uint8_t(v * 255 + 0.5f);
            }
        }
    }
    PrPixelView view{rgba.data(), size_t(target.width) * 4, {target.width, target.height}};
    size_t size = 0;
    prImageCodecEncode(codec, "image/png", view, nullptr, 0, &size);
    std::vector<uint8_t> png(size);
    if (prImageCodecEncode(codec, "image/png", view, png.data(), png.size(), &size) != PrStatus_Ok)
        return;
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return;
    std::fwrite(png.data(), 1, size, file);
    std::fclose(file);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    std::vector<const char*> configNames;
    std::vector<const char*> collectionNames;
    std::vector<const char*> drawings;
    float pixelRatio = 2;
    const char* images = nullptr;
    TextBackends text;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--renderer") == 0 && i + 1 < argc)
            configNames.push_back(argv[++i]);
        else if (std::strcmp(argv[i], "--collection") == 0 && i + 1 < argc)
            collectionNames.push_back(argv[++i]);
        else if (std::strcmp(argv[i], "--images") == 0 && i + 1 < argc)
            images = argv[++i];
        else if (std::strcmp(argv[i], "--pixel-ratio") == 0 && i + 1 < argc)
            pixelRatio = std::strtof(argv[++i], nullptr);
        else if (std::strcmp(argv[i], "--shaper") == 0 && i + 1 < argc &&
                prTextShaperBackendIsBuilt(prTextShaperBackendFromName(argv[i + 1])))
            text.shaper = prTextShaperBackendFromName(argv[++i]);
        else
            drawings.push_back(argv[i]);
    }
    if (collectionNames.empty())
        collectionNames.push_back("features");
    PrGpuDevice* device = nullptr;
    prGpuDeviceCreate(PrGpuBackend_Default, nullptr, &device);
    std::vector<RendererConfig> configs = createConfigs(device);
    if (configNames.empty() && !configs.empty())
        configNames.push_back(configs[defaultConfig(configs)].name);
    PrCompressCodec* compressCodec = images != nullptr ? prCompressBuiltinCreate() : nullptr;
    PrImageCodec* imageCodec = compressCodec != nullptr ? prImageBuiltinCreate(compressCodec) : nullptr;
    PrDrawList* list = prDrawListCreate();
    int status = 0;
    for (const Collection& collection : drawingCollections()) {
        if (!matches(collectionNames, collection.name))
            continue;
        uint32_t width = uint32_t(collection.width * pixelRatio);
        uint32_t height = uint32_t(collection.height * pixelRatio);
        BenchTarget targets[] = {{{}, width, height, device, {}, kSdr}, {{}, width, height, device, {}, kHdr}};
        for (BenchTarget& target : targets) {
            PrGpuTextureDesc textureDesc = prGpuTextureDesc2d(
                prGpuFormatFromPixelFormat(target.output.format), PrUVec2{width, height},
                PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget);
            if (device != nullptr)
                prGpuTextureCreate(device, &textureDesc, &target.texture);
        }
        for (uint32_t d = 0; d < uint32_t(collection.documents().size()); ++d) {
            if (!named(collection, d, drawings))
                continue;
            std::string name = fileNameOf(collection.name) + " " + (d < 9 ? "0" : "") + std::to_string(d + 1) + " " +
                fileNameOf(collection.documents()[d].name);
            std::shared_ptr<Loaded> document = load(collection.documents()[d].source, text, nullptr);
            PrColor clear = background(*document);
            std::vector<uint8_t> backgroundPixels = pixelsOf(clear, size_t(width) * height);
            Times recordTimes{};
            HarnessCounters recorded = measure([&] {
                double started = nowMillis();
                record(list, *document, collection, pixelRatio);
                return Times{nowMillis() - started, 0};
            }, &recordTimes);
            bool first = true;
            for (const RendererConfig& config : configs) {
                bool asked = false;
                for (const char* n : configNames)
                    asked = asked || std::strcmp(n, config.name) == 0;
                if (!asked)
                    continue;
                for (BenchTarget& target : targets) {
                    bool onDevice = prDrawRendererDevice(config.renderer) == target.device;
                    if (!onDevice && target.output.format != PrPixelFormat_Bgra8)
                        continue;
                    PrDrawFrame* frame = nullptr;
                    PrStatus rendered = PrStatus_Ok;
                    Times t{};
                    HarnessCounters rendering{};
                    bool ready = !(onDevice && target.texture.id == 0);
                    if (ready) {
                        rendering = measure([&] {
                            return render(config.renderer, frame, target, clear, backgroundPixels, list, &rendered);
                        }, &t);
                    }
                    if (!ready || rendered != PrStatus_Ok) {
                        std::fprintf(stderr, "draw_bench: %s failed on %s%s\n", name.c_str(), config.name,
                            target.output.suffix);
                        prDrawFrameDestroy(frame);
                        status = 1;
                        continue;
                    }
                    if (imageCodec != nullptr)
                        writeImage(imageCodec, target,
                            std::string(images) + "/" + name + " - " + config.name + target.output.suffix + ".png");
                    double frameMillis = rendering.millis;
                    rendering.gpu = t.gpu;
                    HarnessCounts counts{};
                    harnessCountsAdd(&counts, "render", rendering);
                    PrDrawStats stats = frame != nullptr ? prDrawFrameStats(frame) : PrDrawStats{};
                    prDrawFrameDestroy(frame);
                    // Recording belongs to the drawing, whichever renderer renders it, so it goes on the first row.
                    char recording[48] = "";
                    if (first) {
                        std::snprintf(recording, sizeof(recording), ", \"record\": %.4f", recordTimes.cpu);
                        harnessCountsAdd(&counts, "record", recorded);
                    }
                    first = false;
                    char timing[96];
                    if (onDevice)
                        std::snprintf(timing, sizeof(timing), "\"cpu\": %.4f, \"gpu\": %.4f, \"render\": %.4f",
                            t.cpu, t.gpu, frameMillis);
                    else
                        std::snprintf(timing, sizeof(timing), "\"cpu\": %.4f, \"render\": %.4f", t.cpu, frameMillis);
                    PrTextBuffer row{};
                    harnessAppendFormat(&row,
                        "{\"name\": \"%s / %s%s\", \"commands\": %u%s, %s, \"passes\": %u, \"layers\": %u, "
                            "\"draws\": %u, \"primitives\": %u, \"records\": %u, \"paths\": %u, \"upload\": %llu",
                        name.c_str(), config.name, target.output.suffix, prDrawListCommandCount(list), recording,
                        timing, stats.passes, stats.layers, stats.draws, stats.primitives, stats.records,
                        stats.pathFills, static_cast<unsigned long long>(stats.uploadBytes));
                    harnessCountsWrite(&counts, &row);
                    prTextBufferAppend(&row, "}");
                    harnessWriteLine(prTextBufferView(&row));
                    prTextBufferReset(&row);
                    std::fflush(stdout);
                }
            }
        }
        for (BenchTarget& target : targets) {
            if (device != nullptr)
                prGpuTextureDestroy(device, target.texture);
        }
    }
    prDrawListRelease(list);
    prImageCodecDestroy(imageCodec);
    prCompressCodecDestroy(compressCodec);
    destroyConfigs(configs);
    prGpuDeviceDestroy(device);
    releaseDrawings();
    return status;
}
