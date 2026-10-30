// Sources by their types, the documents' references, and the collections that the viewer offers.

#include "tools/viewer/documents.h"

#include "pr/font_hinter.h"

#include "tools/viewer/drawing.h"
#include "tools/viewer/source_html.h"
#include "tools/viewer/source_image.h"
#include "tools/viewer/source_lottie.h"
#include "tools/viewer/source_rlottie.h"
#include "tools/viewer/source_svg.h"
#include "tools/viewer/suite_lottie.h"
#include "tools/viewer/suite_markdown.h"
#include "tools/viewer/suite_prax.h"
#include "tools/viewer/suite_svg.h"
#include "tools/viewer/suite_wpt.h"
#include "tools/viewer/suites.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <sstream>

namespace viewer {

namespace fs = std::filesystem;

namespace {

// Returns the hinter that fits fonts by `hinting`, the default hinter, or null for none. It lives as long as the
// process, because fonts that name it may be kept.
PrFontHinter* PR_NULLABLE hinterOf(PrFontHinting hinting) {
    static PrFontHinter* hinter = prFontHinterCreate(PrFontHinterBackend_Default);
    return hinting == PrFontHinting_None ? nullptr : hinter;
}

// Returns the snapshot nearest `time`, looped over the source's length.
uint32_t snapshotAt(const Source& source, double time) {
    double at = source.length > 0 ? std::fmod(time, source.length) : time;
    uint32_t nearest = 0;
    for (uint32_t i = 1; i < source.snapshots.size(); ++i)
        if (std::fabs(source.snapshots[i].first - at) < std::fabs(source.snapshots[nearest].first - at))
            nearest = i;
    return nearest;
}

} // namespace

struct Loaded {
    Source source;

    // The shaper that sets the source's text. It outlives what the source's type loaded, which refers to it.
    std::unique_ptr<PrTextShaper, decltype(&prTextShaperDestroy)> shaper{nullptr, prTextShaperDestroy};

    std::mutex mutex;           // held by each call, which may come from any pane's thread
    double time = 0;            // the time the source was brought to
    double frame = -1;          // the source's frame at that time

    // What the source's type loaded: the member for its type, which is null if the source did not load.
    std::shared_ptr<SvgSource> svg;
    std::shared_ptr<HtmlSource> html;
    std::shared_ptr<LottieSource> lottie;
    std::shared_ptr<RlottieSource> rlottie;
    std::shared_ptr<LoadedImage> image;
    std::vector<std::shared_ptr<LoadedImage>> snapshots;   // by Source::snapshots, each null if it did not load
    uint32_t snapshot = 0;                                 // the snapshot that the time shows

    PrFontHinting hinting = PrFontHinting_None;   // how the source's fonts are hinted
};

Source fileSource(const fs::path& path, const fs::path& settings) {
    std::string extension = path.extension().string();
    Source source;
    source.path = path;
    source.settings = settings;
    if (extension == ".svg")
        source.type = SourceType_Svg;
    else if (extension == ".png")
        source.type = SourceType_Image;
    else if (extension == ".json")
        source.type = SourceType_Lottie;
    else
        source.type = SourceType_Html;
    return source;
}

Reference browserReference(const Source& source) {
    switch (source.type) {
        case SourceType_Svg:
            return Reference{"browser", {}, fileUrl(source.path)};
        case SourceType_Html:
            // A browser shows Markdown as plain text, so a Markdown document has no browser's render.
            if (source.path.extension() == ".md" || source.path.extension() == ".markdown")
                break;
            return Reference{"browser", {}, fileUrl(source.path)};
        case SourceType_Lottie:
            return Reference{"lottie-web", {}, lottiePlayerUrl("player.html", source.path)};
        case SourceType_Drawing:
        case SourceType_Rlottie:
        case SourceType_Image:
        case SourceType_Snapshots:
            break;
    }
    return Reference{};
}

std::vector<Reference> referencesOf(const Document& document) {
    std::vector<Reference> references = document.references;
    Reference browser = browserReference(document.source);
    bool listed = false;
    for (const Reference& reference : references)
        listed = listed || reference.name == browser.name;
    if (!browser.url.empty() && !listed)
        references.push_back(std::move(browser));
    return references;
}

std::shared_ptr<Loaded> load(const Source& source, TextBackends text, PrGpuDevice* PR_NULLABLE device) {
    std::shared_ptr<Loaded> loaded = std::make_shared<Loaded>();
    loaded->source = source;
    loaded->shaper.reset(prTextShaperCreate(text.shaper));
    loaded->hinting = text.hinting;
    PrTextShaper* textShaper = loaded->shaper.get();
    switch (source.type) {
        case SourceType_Drawing:
            break;
        case SourceType_Svg:
            loaded->svg = loadSvg(source.path, source.settings, textShaper, text.hinting, hinterOf(text.hinting));
            break;
        case SourceType_Html:
            loaded->html = loadHtml(source.path, textShaper, text.hinting, hinterOf(text.hinting), device);
            break;
        case SourceType_Lottie:
            loaded->lottie = loadLottie(source.path, source.settings, textShaper, text.hinting, hinterOf(text.hinting));
            break;
        case SourceType_Rlottie:
            loaded->rlottie = loadRlottie(source.path);
            break;
        case SourceType_Image:
            loaded->image = loadImage(source.path);
            break;
        case SourceType_Snapshots:
            for (const auto& [time, path] : source.snapshots)
                loaded->snapshots.push_back(loadImage(path));
            break;
    }
    return loaded;
}

PrVec2 naturalSize(const Loaded& loaded) {
    if (loaded.lottie != nullptr)
        return lottieSize(*loaded.lottie);
    if (loaded.image != nullptr)
        return imageSize(*loaded.image);
    for (const std::shared_ptr<LoadedImage>& snapshot : loaded.snapshots)
        if (snapshot != nullptr)
            return imageSize(*snapshot);
    return PrVec2{0, 0};
}

PrColor background(const Loaded& loaded) {
    if (loaded.source.type == SourceType_Drawing)
        return kPaper;
    return kWhite;
}

double update(Loaded& loaded, double time) {
    std::lock_guard<std::mutex> lock(loaded.mutex);
    loaded.time = time;
    loaded.frame = -1;
    if (loaded.lottie != nullptr)
        loaded.frame = lottieFrameAt(*loaded.lottie, time);
    else if (loaded.rlottie != nullptr)
        loaded.frame = rlottieFrameAt(*loaded.rlottie, time);
    else if (loaded.html != nullptr)
        htmlAt(*loaded.html, time);
    else if (!loaded.snapshots.empty())
        loaded.snapshot = snapshotAt(loaded.source, time);
    return loaded.frame;
}

bool pixels(Loaded& loaded, uint32_t width, uint32_t height, std::vector<uint8_t>& bgra) {
    std::lock_guard<std::mutex> lock(loaded.mutex);
    switch (loaded.source.type) {
        case SourceType_Rlottie:
            if (loaded.rlottie == nullptr)
                return false;
            rlottiePixels(*loaded.rlottie, width, height, loaded.frame, bgra);
            return true;
        case SourceType_Image:
            return loaded.image != nullptr && imagePixels(*loaded.image, width, height, bgra);
        case SourceType_Snapshots:
            return loaded.snapshot < loaded.snapshots.size() && loaded.snapshots[loaded.snapshot] != nullptr &&
                imagePixels(*loaded.snapshots[loaded.snapshot], width, height, bgra);
        case SourceType_Drawing:
        case SourceType_Svg:
        case SourceType_Html:
        case SourceType_Lottie:
            break;
    }
    return false;
}

bool pixelsOnly(const Loaded& loaded) {
    return loaded.source.type == SourceType_Rlottie;
}

void draw(Loaded& loaded, PrDrawList* list, float width, float height, const PrAppDisplay& display,
        const PrAppUserPreferences& preferences) {
    std::lock_guard<std::mutex> lock(loaded.mutex);
    switch (loaded.source.type) {
        case SourceType_Drawing:
            drawDrawing(*loaded.source.drawing, list, loaded.shaper.get(), hinterOf(loaded.hinting),
                display, width, height, loaded.time);
            break;
        case SourceType_Svg:
            if (loaded.svg != nullptr)
                drawSvg(*loaded.svg, list, width, height);
            break;
        case SourceType_Html:
            if (loaded.html != nullptr)
                drawHtml(*loaded.html, list, width, height, display, preferences);
            break;
        case SourceType_Lottie:
            if (loaded.lottie != nullptr)
                drawLottie(*loaded.lottie, list, width, height, loaded.frame);
            break;
        case SourceType_Image:
            if (loaded.image != nullptr)
                drawImage(*loaded.image, list, width, height);
            break;
        case SourceType_Snapshots:
            if (loaded.snapshot < loaded.snapshots.size() && loaded.snapshots[loaded.snapshot] != nullptr)
                drawImage(*loaded.snapshots[loaded.snapshot], list, width, height);
            break;
        case SourceType_Rlottie:
            break;
    }
}

bool takesInput(const Loaded& loaded) {
    return loaded.html != nullptr;
}

void input(Loaded& loaded, const PrAppEvent& event) {
    std::lock_guard<std::mutex> lock(loaded.mutex);
    if (loaded.html != nullptr)
        htmlInput(*loaded.html, event);
}

const std::vector<Document>& Collection::documents() const {
    if (listed_ == nullptr)
        listed_ = std::make_shared<const std::vector<Document>>(list());
    return *listed_;
}

const std::vector<Collection>& allCollections() {
    static const std::vector<Collection> collections = [] {
        std::vector<Collection> all = drawingCollections();
        for (std::vector<Collection> (*suite)() :
                {praxCollections, wptCollections, svgCollections, lottieCollections, markdownCollections}) {
            for (Collection& c : suite())
                all.push_back(std::move(c));
        }
        return all;
    }();
    return collections;
}

std::string queryEscaped(const std::string& text) {
    std::string out;
    for (char c : text) {
        uint8_t byte = uint8_t(c);
        bool plain = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') ||
            c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':';
        if (plain) {
            out += c;
        } else {
            char escaped[4];
            std::snprintf(escaped, sizeof(escaped), "%%%02X", byte);
            out += escaped;
        }
    }
    return out;
}

std::string fileUrl(const std::filesystem::path& path) {
    // A path that starts with a drive, as on Windows, takes a slash before it.
    std::string absolute = std::filesystem::absolute(path).lexically_normal().generic_string();
    return (absolute.starts_with('/') ? "file://" : "file:///") + queryEscaped(absolute);
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

std::string htmlTitle(const std::filesystem::path& path) {
    std::string text = readFile(path);
    size_t open = text.find("<title>");
    size_t close = open == std::string::npos ? open : text.find("</title>", open);
    if (close == std::string::npos)
        return {};
    return text.substr(open + 7, close - open - 7);
}

std::string fileNameOf(const std::string& name) {
    std::string out = name;
    for (char& c : out)
        c = c >= 'A' && c <= 'Z'   ? char(c - 'A' + 'a') : c >= 'a' && c <= 'z' ? c : c >= '0' && c <= '9' ? c : '-';
    return out;
}

bool documentNamed(const Collection& collection, uint32_t index, const char* which) {
    if (which == nullptr)
        return true;
    char* end = nullptr;
    long number = std::strtol(which, &end, 10);
    if (end != which && *end == 0)
        return number == long(index) + 1;
    return fileNameOf(collection.documents()[index].name).find(fileNameOf(which)) != std::string::npos;
}

} // namespace viewer
