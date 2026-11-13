// The reftest harness, which the runners of every suite that compares renders drive (tools/suite/reftest.py,
// tools/suite/wpt/wpt.py). It renders a document and compares it with its reference.
//
// `--render <settings>` renders as a suite's settings say (tools/suite/suite_render.h), `--shaper <name>` shapes text
// with the shaper of lib/text that it names, the default shaper otherwise, and `--hinting <name>` hints fonts as it
// names with the default hinter, none by default. A case's input is lines: the document; its reference, a PNG or
// another document; the size to render at, `<width>x<height>`, or `-` for the reference PNG's size; where to write the
// render, the reference and their difference, each `-` for nowhere; and, optionally, when an animation is rendered: a
// time in seconds for an SVG document, and a frame, counted from the in point, for a Lottie animation.
//
// The cpu backend renders a document according to its type. lib/svg renders an SVG. lib/lottie renders a Lottie
// animation (`.json`, or a dotLottie archive's first animation) over the settings' background, fitted to the size as
// its players fit it. Its text is set in the settings' fonts, or in the system's without settings. A PNG is a render
// made elsewhere, such as a page that pr_shell captured (--screenshot), and must have the requested size. A type that
// the engine does not render returns NotSupported.
//
// The answer holds the status; the image corpora's figures (draw/tests/compare.h); WPT's figures, which are the largest
// channel difference and the number of pixels that differ; whether the reference draws anything at all; and the number
// of milliseconds that rendering the document took.

#include "pr/assert.h"
#include "pr/compress.h"
#include "pr/dom_xml.h"
#include "pr/draw_cpu.h"
#include "pr/draw_glyph.h"
#include "pr/font_hinter.h"
#include "pr/font_set.h"
#include "pr/image.h"
#include "pr/lottie.h"
#include "pr/lottie_archive.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/svg_render.h"
#include "pr/text.h"
#include "pr/time.h"
#include "pr/url_data.h"

#include "tools/harness/compare.h"
#include "tools/harness/harness.h"
#include "tools/harness/harness_png.h"
#include "tools/suite/suite_render.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pr;
using namespace harness;
using namespace suite;

namespace {

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;          // straight
};

// Reads the PNG at `path` as straight RGBA. Returns false if it cannot be read.
bool readPng(PrImageCodec* imageCodec, const std::string& path, Image* out) {
    PrTextBuffer bytes = {};
    if (!harnessReadFile(&bytes, path.c_str()))
        return false;
    PrStringView view = prTextBufferView(&bytes);
    PrImageSource source{};
    source.memory = PrByteView{reinterpret_cast<const uint8_t*>(view.data), view.size};
    PrImageDecoder* decoder = prImageDecoderCreate(imageCodec);
    bool ok = prImageDecoderOpen(decoder, source, nullptr) == PrStatus_Ok;
    if (ok) {
        const PrImageInfo* info = prImageDecoderInfo(decoder);
        const bool swaps = prImageOrientationSwapsAxes(info->orientation);
        out->width = swaps ? info->extent.y : info->extent.x;
        out->height = swaps ? info->extent.x : info->extent.y;
        out->rgba.resize(size_t(out->width) * out->height * 4);
        const PrPixels pixels{out->rgba.data(), size_t(out->width) * 4, {out->width, out->height}};
        ok = prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, info->orientation, pixels) == PrStatus_Ok;
    }
    prImageDecoderDestroy(decoder);
    prTextBufferReset(&bytes);
    return ok;
}

bool writePng(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
    return harnessWritePng(path.c_str(), PrPixelView{rgba.data(), size_t(width) * 4, {width, height}});
}

// The documents that a render has loaded, each with the path that it came from, against which the references in it
// resolve.
struct Documents {
    std::vector<std::pair<PrDomDocument*, std::string>> loaded;

    ~Documents() {
        for (auto& [document, path] : loaded)
            prDomDocumentRelease(document);
    }

    PrDomDocument* PR_NULLABLE load(const std::string& path) {
        for (auto& [document, from] : loaded)
            if (from == path)
                return document;
        PrTextBuffer source = {};
        if (!harnessReadFile(&source, path.c_str()))
            return nullptr;
        PrDomDocument* document = prDomParseXml(prTextBufferView(&source), "image/svg+xml", nullptr);
        prSvgEnable(document);
        prTextBufferReset(&source);
        loaded.emplace_back(document, path);
        return document;
    }
};

// What loads the images that a document names: its codecs, and the documents whose paths references in them resolve
// against. An SVG image is in secure static mode, whose own images load only from data: URLs, so its loader has no
// documents.
struct Images {
    PrCompressCodec* compressCodec;
    PrImageCodec* imageCodec;
    Documents* PR_NULLABLE documents;
};

// Replaces `bytes` with what they decompress to, if they are gzip, as an svgz file is. Returns false if they do not
// decompress.
bool gunzip(PrCompressCodec* codec, std::string* bytes) {
    if (bytes->size() < 2 || uint8_t((*bytes)[0]) != 0x1f || uint8_t((*bytes)[1]) != 0x8b)
        return true;
    PrByteView input{reinterpret_cast<const uint8_t*>(bytes->data()), bytes->size()};
    for (size_t capacity = bytes->size() * 4;; capacity *= 2) {
        std::string output(capacity, '\0');
        PrCompressRead read{};
        PrStatus status = prCompressCodecDecode(codec, PrCompressFormat_Gzip, input,
            reinterpret_cast<uint8_t*>(output.data()), capacity, &read);
        if (status != PrStatus_Ok)
            return false;
        if (read.progress == PrCompressProgress_Output)
            continue;
        output.resize(read.written);
        *bytes = std::move(output);
        return true;
    }
}

bool loadImage(void* PR_NULLABLE context, PrDomDocument* from, PrStringView url, PrSvgLoadedImage* out);

// Writes the image that `bytes` hold to `out`: an SVG image, named by a URL whose fragment is `fragment`, or a raster
// image.
bool decodeImage(const Images& images, std::string bytes, PrStringView fragment, PrSvgLoadedImage* out) {
    if (!gunzip(images.compressCodec, &bytes))
        return false;
    PrStringView text{bytes.data(), bytes.size()};
    PrStringView head = prStringSlice(text, 0, text.size < 512 ? text.size : 512);
    if (prStringContains(head, "<svg")) {
        PrDomDocument* document = prDomParseXml(text, "image/svg+xml", nullptr);
        if (document == nullptr)
            return false;
        prSvgEnable(document);
        PrDomElement* root = prDomDocumentElement(document);
        PrSvgElement* svg = root != nullptr ? prSvgElementQuery(prDomElementAsEventTarget(root)) : nullptr;
        Images secure{images.compressCodec, images.imageCodec, nullptr};
        PrSvgDrawOptions options = prSvgDrawOptionsDefault();
        options.loadImage = loadImage;
        options.loadContext = &secure;
        if (svg != nullptr)
            out->svg = prSvgImageCreate(svg, fragment, &options);
        prDomDocumentRelease(document);
        return out->svg != nullptr;
    }
    PrByteView view{reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()};
    return prDrawImageCreateFromBytes(images.imageCodec, view, &out->raster, nullptr) == PrStatus_Ok;
}

// Loads an image that a reference names (PrSvgLoadImageFn): a data: URL, or a file beside the document that names it.
bool loadImage(void* PR_NULLABLE context, PrDomDocument* from, PrStringView url, PrSvgLoadedImage* out) {
    const Images& images = *static_cast<const Images*>(context);
    size_t hash = prStringFindByte(url, '#');
    PrStringView fragment = hash < url.size ? prStringSlice(url, hash + 1, url.size - hash - 1) : PrStringView{};
    PrStringView address = prStringSlice(url, 0, hash);
    PrRawString body{};
    PrRawString type{};
    if (prUrlProcessData(address, &body, &type) == PrStatus_Ok) {
        std::string bytes(prRawStringView(&body).data, prRawStringView(&body).size);
        prRawStringReset(&body);
        prRawStringReset(&type);
        return decodeImage(images, std::move(bytes), fragment, out);
    }
    if (images.documents == nullptr)
        return false;
    for (auto& [document, path] : images.documents->loaded) {
        if (document != from)
            continue;
        std::string file = path.substr(0, path.find_last_of('/') + 1) + std::string(address.data, address.size);
        PrTextBuffer source = {};
        bool read = harnessReadFile(&source, file.c_str());
        PrStringView sourceText = prTextBufferView(&source);
        std::string bytes(sourceText.data, sourceText.size);
        prTextBufferReset(&source);
        return read && decodeImage(images, std::move(bytes), fragment, out);
    }
    return false;
}

PrDomDocument* PR_NULLABLE loadReferenced(void* PR_NULLABLE context, PrDomDocument* from, PrStringView url) {
    Documents* documents = static_cast<Images*>(context)->documents;
    for (auto& [document, path] : documents->loaded) {
        if (document != from)
            continue;
        std::string directory = path.substr(0, path.find_last_of('/') + 1);
        return documents->load(directory + std::string(url.data, url.size));
    }
    return nullptr;
}

// The files beside a Lottie animation that it names, each kept until the animation is read.
struct LottieFiles {
    std::string directory;
    std::vector<std::unique_ptr<PrTextBuffer>> read;

    ~LottieFiles() {
        for (std::unique_ptr<PrTextBuffer>& buffer : read)
            prTextBufferReset(buffer.get());
    }
};

bool loadLottieFile(void* PR_NULLABLE context, PrStringView path, PrByteView* out) {
    LottieFiles* files = static_cast<LottieFiles*>(context);
    std::unique_ptr<PrTextBuffer> buffer = std::make_unique<PrTextBuffer>();
    std::string full = files->directory + std::string(path.data, path.size);
    if (!harnessReadFile(buffer.get(), full.c_str()))
        return false;
    PrStringView view = prTextBufferView(buffer.get());
    *out = PrByteView{reinterpret_cast<const uint8_t*>(view.data), view.size};
    files->read.push_back(std::move(buffer));
    return true;
}

// The fonts that Lottie text is set in without settings: the system's fonts, with a browser's default family. Created
// when a case first needs them.
struct LottieFonts {
    PrFontSet* fonts = nullptr;

    ~LottieFonts() { prFontSetDestroy(fonts); }
};

PrFontSet* lottieFonts() {
    static LottieFonts text;
    if (text.fonts == nullptr) {
        text.fonts = prFontSetCreate(nullptr);
        prFontSetSetSystem(text.fonts, prFontSystemCreate(PrFontSystemBackend_Default));
        prFontSetSetDefaultFamily(text.fonts, prFontFamilyFromName("Times"));
    }
    return text.fonts;
}

// Records the Lottie animation at `frame`, counted from its in point, fitted and centered in `width` by `height` over
// the suite's background, with its text set in the suite's fonts and shaped by `shaper`. The animation is a JSON file,
// or a dotLottie archive's first animation.
PrStatus recordLottie(PrCompressCodec* compressCodec, PrImageCodec* imageCodec, const SuiteRender& suite,
        PrTextShaper* shaper, const std::string& path, float width, float height, double frame, PrDrawList* list) {
    PrTextBuffer source = {};
    if (!harnessReadFile(&source, path.c_str()))
        return PrStatus_NotFound;
    LottieFiles files;
    files.directory = path.substr(0, path.find_last_of('/') + 1);
    PrLottieOptions options{};
    options.codec = imageCodec;
    options.load = loadLottieFile;
    options.loadContext = &files;
    options.fonts = suite.fonts != nullptr ? suite.fonts : lottieFonts();
    options.shaper = shaper;
    PrLottieAnimation* animation = nullptr;
    PrStatus status = PrStatus_Ok;
    if (path.ends_with(".lottie")) {
        PrStringView bytes = prTextBufferView(&source);
        PrLottieArchive* archive = prLottieArchiveCreate(compressCodec);
        status = prLottieArchiveOpen(archive, PrByteView{reinterpret_cast<const uint8_t*>(bytes.data), bytes.size});
        if (status == PrStatus_Ok && prLottieArchiveAnimationCount(archive) > 0)
            status = prLottieAnimationCreateFromArchive(archive, 0, &options, &animation);
        else if (status == PrStatus_Ok)
            status = PrStatus_NotFound;
        prLottieArchiveDestroy(archive);
    } else {
        status = prLottieAnimationCreate(prTextBufferView(&source), &options, &animation);
    }
    prTextBufferReset(&source);
    if (status != PrStatus_Ok)
        return status;
    PrLottieInfo info = prLottieAnimationInfo(animation);
    // Drawn over the background that the reference draws over, because blend modes blend with it.
    prDrawListClearRect(list, PrRect{0, 0, width, height}, suite.background);
    float scale = info.extent.x > 0 && info.extent.y > 0 ? std::fmin(width / info.extent.x, height / info.extent.y) : 1;
    prDrawListPushTransformAffine2(list, PrAffine2{
        scale, 0, 0, scale, (width - info.extent.x * scale) / 2,
        (height - info.extent.y * scale) / 2,
    });
    prLottieAnimationDraw(animation, info.inPoint + frame, list);
    prDrawListPopTransform(list);
    prLottieAnimationDestroy(animation);
    return PrStatus_Ok;
}

// Renders the document onto transparent pixels of the given size, as straight RGBA. `time` is a time in seconds for an
// SVG document, and a frame for a Lottie animation.
PrStatus render(PrDrawRenderer* renderer, PrCompressCodec* compressCodec, PrImageCodec* imageCodec,
        const SuiteRender& suite, PrTextShaper* shaper, const std::string& path, uint32_t width,
        uint32_t height, double time, std::vector<uint8_t>* rgba) {
    rgba->assign(size_t(width) * height * 4, 0);
    if (path.ends_with(".png")) {
        Image image;
        if (!readPng(imageCodec, path, &image))
            return PrStatus_NotFound;
        if (image.width != width || image.height != height)
            return PrStatus_InvalidData;
        *rgba = std::move(image.rgba);
        return PrStatus_Ok;
    }
    if (path.ends_with(".json") || path.ends_with(".lottie")) {
        PrDrawList* list = prDrawListCreate();
        PrStatus status =
            recordLottie(compressCodec, imageCodec, suite, shaper, path, float(width), float(height), time, list);
        PrPixels pixels{rgba->data(), size_t(width) * 4, {width, height}};
        if (status == PrStatus_Ok)
            status = prDrawRendererRenderPixels(renderer, list, pixels, PrPixelFormat_Bgra8);
        prDrawListRelease(list);
        straightenBgra(rgba->data(), width, height, size_t(width) * 4);
        return status;
    }
    if (!path.ends_with(".svg"))
        return PrStatus_NotSupported;
    Documents documents;
    PrDomDocument* document = documents.load(path);
    if (document == nullptr)
        return PrStatus_NotFound;
    PrDomElement* root = prDomDocumentElement(document);
    PrSvgElement* svg = root != nullptr ? prSvgElementQuery(prDomElementAsEventTarget(root)) : nullptr;
    PrStatus status = PrStatus_NotSupported;
    PrDrawList* list = prDrawListCreate();
    Images images{compressCodec, imageCodec, &documents};
    PrSvgDrawOptions options{};
    options.fonts = suite.fonts;
    options.shaper = shaper;
    options.load = loadReferenced;
    options.loadImage = loadImage;
    options.loadContext = &images;
    if (svg != nullptr) {
        prSvgElementDraw(svg, PrVec2{float(width), float(height)}, &options, list);
        status = PrStatus_Ok;
    }
    PrPixels pixels{rgba->data(), size_t(width) * 4, {width, height}};
    if (status == PrStatus_Ok)
        status = prDrawRendererRenderPixels(renderer, list, pixels, PrPixelFormat_Bgra8);
    prDrawListRelease(list);
    straightenBgra(rgba->data(), width, height, size_t(width) * 4);
    return status;
}

void writeFigure(PrTextBuffer* out, const char* name, double value) {
    char text[64];
    std::snprintf(text, sizeof(text), ",\"%s\":%.9g", name, value);
    prTextBufferAppend(out, text);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    SuiteRender suite{};
    PrTextShaperBackend shaper = PrTextShaperBackend_Default;
    PrFontHinting hinting = PrFontHinting_None;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string value = argv[i + 1];
        if (std::string(argv[i]) == "--shaper" && prTextShaperBackendIsBuilt(prTextShaperBackendFromName(value))) {
            shaper = prTextShaperBackendFromName(value);
        } else if (std::string(argv[i]) == "--hinting" && prFontHintingFromName(value) != PrFontHinting_Count) {
            hinting = prFontHintingFromName(value);
        } else if (std::string(argv[i]) != "--render") {
            std::fprintf(stderr, "usage: reftest [--render <settings>] [--shaper <name>] [--hinting none|light|full]\n");
            return 1;
        } else if (!suiteRenderRead(&suite, value, PR_ROOT_DIR)) {
            std::fprintf(stderr, "cannot read %s\n", value.c_str());
            return 1;
        }
    }
    PrTextShaper* textShaper = prTextShaperCreate(shaper);
    // The hinter lives as long as the process, because the fonts that name it are kept.
    PrFontHinter* hinter = hinting != PrFontHinting_None ? prFontHinterCreate(PrFontHinterBackend_Default) : nullptr;
    if (suite.fonts != nullptr)
        prFontSetSetHinting(suite.fonts, hinting, hinter);
    prFontSetSetHinting(lottieFonts(), hinting, hinter);
    PrCompressCodec* compressCodec = prCompressCodecCreate(PrCompressCodecBackend_Default);
    PrImageCodec* imageCodec = prImageCodecCreate(PrImageCodecBackend_Default, compressCodec);
    PrDrawCpuOptions options = prDrawCpuOptionsDefault();
    PrDrawGlyphDecoder* glyphDecoder = prDrawGlyphBitmapCreate(imageCodec);
    options.glyphDecoder = glyphDecoder;
    // Glyphs are drawn at an atlas's phases, so that runs that a document splits differently from its reference draw
    // alike, as reftests expect.
    options.glyphPhases = true;
    PrDrawRenderer* renderer = prDrawCpuCreate(&options);
    HarnessCase test = {};
    while (harnessReadCase(&test)) {
        std::vector<std::string> paths;
        PrStringView input = prTextBufferView(&test.input);
        size_t start = 0;
        for (size_t i = 0; i <= input.size; ++i) {
            if (i == input.size || input.data[i] == '\n') {
                paths.emplace_back(input.data + start, i - start);
                start = i + 1;
            }
        }
        if (paths.size() != 6 && paths.size() != 7)
            return 1;
        double time = paths.size() == 7 ? std::strtod(paths[6].c_str(), nullptr) : 0;

        PrTextBuffer out = {};
        Image reference;
        PrStatus status = PrStatus_Ok;
        if (!paths[1].ends_with(".png")) {
            unsigned width = 0;
            unsigned height = 0;
            if (std::sscanf(paths[2].c_str(), "%ux%u", &width, &height) != 2)
                return 1;
            reference.width = width;
            reference.height = height;
            status = render(renderer, compressCodec, imageCodec, suite, textShaper, paths[1], width, height, time,
                &reference.rgba);
            if (status != PrStatus_Ok)
                reference.rgba.clear();
        } else if (!readPng(imageCodec, paths[1], &reference)) {
            status = PrStatus_NotFound;
        }
        if (status == PrStatus_Ok && paths[4] != "-")
            writePng(paths[4], reference.rgba, reference.width, reference.height);
        std::vector<uint8_t> rendered;
        PrTimePerf started = prTimePerfNow();
        if (status == PrStatus_Ok)
            status = render(renderer, compressCodec, imageCodec, suite, textShaper, paths[0], reference.width,
                reference.height, time, &rendered);
        double ms = prTimePerfMilliseconds(started, prTimePerfNow());
        prTextBufferAppend(&out, "{\"status\":");
        harnessWriteString(&out, prStatusName(status));
        if (!rendered.empty()) {
            std::vector<uint8_t> diff(rendered.size());
            size_t stride = size_t(reference.width) * 4;
            ImageDifference d = compareImages(reference.rgba.data(), rendered.data(), reference.width,
                reference.height, stride, ImageComparison{}, diff.data());
            writeFigure(&out, "changed", d.changedRatio);
            writeFigure(&out, "content", d.contentChangedRatio);
            writeFigure(&out, "mae", d.mae);
            PixelDifference p = countDifferences(reference.rgba.data(), rendered.data(), reference.width,
                reference.height, stride);
            writeFigure(&out, "maxDifference", double(p.maxDifference));
            writeFigure(&out, "differingPixels", double(p.differingPixels));
            bool drawn = imageDrawsAnything(reference.rgba.data(), reference.width, reference.height, stride);
            prTextBufferAppend(&out, drawn ? ",\"blank\":false" : ",\"blank\":true");
            if (paths[3] != "-")
                writePng(paths[3], rendered, reference.width, reference.height);
            if (paths[5] != "-")
                writePng(paths[5], diff, reference.width, reference.height);
        }
        writeFigure(&out, "ms", ms);
        prTextBufferAppendChar(&out, '}');
        harnessWriteLine(prTextBufferView(&out));
        prTextBufferReset(&out);
    }
    harnessCaseReset(&test);
    prDrawRendererDestroy(renderer);
    prDrawGlyphDecoderDestroy(glyphDecoder);
    prImageCodecDestroy(imageCodec);
    prCompressCodecDestroy(compressCodec);
    prTextShaperDestroy(textShaper);
    suiteRenderReset(&suite);
    return 0;
}
