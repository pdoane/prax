// Sources by their types, the documents' references, and the collections that the viewer offers.

#include "tools/viewer/documents.h"

#include "pr/font_hinter.h"
#include "pr/hash.h"

#include "tools/viewer/source_html.h"
#include "tools/viewer/source_lottie.h"
#include "tools/viewer/source_pdf.h"
#include "tools/viewer/source_rlottie.h"
#include "tools/viewer/suite_lottie.h"
#include "tools/viewer/suite_markdown.h"
#include "tools/viewer/suite_mathml.h"
#include "tools/viewer/suite_prax.h"
#include "tools/viewer/suite_prince.h"
#include "tools/viewer/suite_svg.h"
#include "tools/viewer/suite_wpt.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
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

// Returns the time of the last change to a file in `directory` or its subdirectories.
fs::file_time_type lastChange(const fs::path& directory) {
    std::error_code error;
    fs::file_time_type latest = fs::last_write_time(directory, error);
    for (fs::recursive_directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        fs::file_time_type changed = fs::last_write_time(it->path(), error);
        if (!error && changed > latest)
            latest = changed;
    }
    return latest;
}

// Writes `text` to the page named for `key` in the viewer's cache, unless it holds that text, and returns its path. The
// page's name is a hash of the key, since a key that names several files is longer than a file's name may be.
fs::path madePage(const std::string& key, const std::string& text) {
    char name[24];
    std::snprintf(name, sizeof(name), "%016llx.html", static_cast<unsigned long long>(prHashBytes(key.data(),
        key.size(), 0)));
    fs::path page = viewerCache() / "pages" / name;
    std::error_code error;
    if (fs::exists(page, error) && readFile(page) == text)
        return page;
    fs::create_directories(page.parent_path(), error);
    std::ofstream(page, std::ios::binary) << text;
    return page;
}

} // namespace

struct Loaded {
    Source source;

    // The shaper that sets the source's text. It outlives what the source's type loaded, which refers to it.
    std::unique_ptr<PrTextShaper, decltype(&prTextShaperDestroy)> shaper{nullptr, prTextShaperDestroy};

    std::mutex mutex;           // held by each call, which may come from any pane's thread
    double time = 0;            // the time the source was brought to
    double frame = -1;          // the source's frame at that time
    PrVec2 viewport{0, 0};      // the extent of the viewport that the source last drew in, in CSS pixels

    // What the source's type loaded: the member for its type, which is null if the source did not load.
    std::shared_ptr<HtmlSource> html;
    std::shared_ptr<LottieSource> lottie;
    std::shared_ptr<RlottieSource> rlottie;
    std::shared_ptr<PdfSource> pdf;
};

bool prints(const Source& source) {
    return source.type == SourceType_Page;
}

fs::path viewerCache() {
    return fs::path(PR_VIEWER_CACHE_DIR);
}

Source fileSource(const fs::path& path, const fs::path& settings) {
    std::string extension = path.extension().string();
    if (extension == ".json")
        return Source{.type = SourceType_Lottie, .path = path, .settings = settings};
    if (extension == ".png")
        return Source{.path = imagePage(path)};
    return Source{.path = path, .settings = settings};
}

fs::path imagePage(const fs::path& path) {
    // An image fills the viewport, fitted and centered, as an SVG document without a size of its own fills it.
    std::string style = "html, body { margin: 0; height: 100% } img { display: block; width: 100%; height: 100%;"
        " object-fit: contain }";
    std::string text = "<!DOCTYPE html>\n<style>" + style + "</style>\n<img src=\"" + fileUrl(path) + "\">\n";
    return madePage("image " + fs::absolute(path).string(), text);
}

fs::path pagesPage(const std::vector<fs::path>& pages) {
    // Each document is on a page named for it, whose size is the document's, which its root's width and height give.
    std::string style = "html, body { margin: 0 } img { display: block }\n";
    std::string images;
    std::string key = "pages";
    for (size_t i = 0; i < pages.size(); ++i) {
        std::string svg = readFile(pages[i]);
        size_t root = svg.find("<svg");
        auto attribute = [&](const char* name) {
            std::string quoted = std::string(" ") + name + "=\"";
            size_t at = svg.find(quoted, root);
            return at == std::string::npos ? 0.0 : std::strtod(svg.c_str() + at + quoted.size(), nullptr);
        };
        char rule[160];
        std::snprintf(rule, sizeof(rule), "@page p%zu { size: %gpx %gpx; margin: 0 } #p%zu { page: p%zu }\n", i,
            attribute("width"), attribute("height"), i, i);
        style += rule;
        images += "<img id=\"p" + std::to_string(i) + "\" src=\"" + fileUrl(pages[i]) + "\">\n";
        key += " " + fs::absolute(pages[i]).string();
    }
    return madePage(key, "<!DOCTYPE html>\n<style>" + style + "</style>\n" + images);
}

fs::path snapshotPage(const std::vector<std::pair<double, fs::path>>& snapshots, double length) {
    // Each image shows from the midpoint after the snapshot before it to the midpoint before the snapshot after it, by
    // an animation of its visibility that loops over the length, so that the page's clock shows the nearest snapshot.
    std::string style = "html, body { margin: 0; height: 100% } img { position: absolute; left: 0; top: 0;"
        " width: 100%; height: 100%; object-fit: contain }\n";
    std::string images;
    double period = length > 0 ? length : 1;
    for (size_t i = 0; i < snapshots.size(); ++i) {
        double from = i == 0 ? 0 : (snapshots[i - 1].first + snapshots[i].first) / 2;
        double to = i + 1 == snapshots.size() ? period : (snapshots[i].first + snapshots[i + 1].first) / 2;
        char rule[320];
        std::snprintf(rule, sizeof(rule),
            "@keyframes s%zu { 0%% { visibility: hidden } %g%% { visibility: visible } %g%% { visibility: hidden } }\n"
                "#s%zu { visibility: hidden; animation: s%zu %gs step-end infinite }\n",
            i, from / period * 100, to / period * 100, i, i, period);
        style += rule;
        images += "<img id=\"s" + std::to_string(i) + "\" src=\"" + fileUrl(snapshots[i].second) + "\">\n";
    }
    std::string key = "snapshots";
    for (const auto& [time, path] : snapshots)
        key += " " + fs::absolute(path).string();
    return madePage(key, "<!DOCTYPE html>\n<style>" + style + "</style>\n" + images);
}

PrRect fittedIn(PrVec2 extent, PrVec2 viewport) {
    if (!(extent.x > 0 && extent.y > 0))
        return PrRect{0, 0, 0, 0};
    float scale = std::fmin(viewport.x / extent.x, viewport.y / extent.y);
    PrVec2 size{extent.x * scale, extent.y * scale};
    PrVec2 at{(viewport.x - size.x) / 2, (viewport.y - size.y) / 2};
    return PrRect{at.x, at.y, at.x + size.x, at.y + size.y};
}

Reference browserReference(const Source& source) {
    switch (source.type) {
        case SourceType_Page:
            // A browser shows Markdown as plain text, so a Markdown document has no browser's render. A page made for
            // an image has none either, since the image is the reference.
            if (source.path.extension() == ".md" || source.path.extension() == ".markdown" ||
                    source.path.parent_path() == viewerCache() / "pages")
                break;
            return Reference{"Browser", {}, htmlBrowserUrl(source.path, source.query), {}, source.path.extension() != ".svg"};
        case SourceType_Lottie:
            return Reference{"lottie-web", {}, lottiePlayerUrl("player.html", source.path)};
        case SourceType_Rlottie:
        case SourceType_Pdf:
            break;
    }
    return Reference{};
}

bool pdfDrawn() {
    return pdfRasterized();
}

Reference pdfReference(const std::string& name, const fs::path& path) {
    if (!pdfDrawn())
        return Reference{name, {}, fileUrl(path)};
    return Reference{name, Source{.type = SourceType_Pdf, .path = path}};
}

std::shared_ptr<Loaded> loadPrinted(const fs::path& path, PrDrawWriterBackend format) {
    if (format == PrDrawWriterBackend_Svg) {
        Source pages{.path = pagesPage(page_host::PageHost::documentFiles(path))};
        return load(pages, TextBackends{}, page_host::Media_Print, nullptr);
    }
    if (!pdfDrawn())
        return nullptr;
    return load(Source{.type = SourceType_Pdf, .path = path}, TextBackends{}, page_host::Media_Screen, nullptr);
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

bool print(const Reference& reference) {
    // The time of the last change to its source's files that each document was printed after, by the document's path.
    static std::mutex mutex;
    static std::map<fs::path, fs::file_time_type> printed;
    std::lock_guard<std::mutex> lock(mutex);
    const fs::path& out = reference.printed.path;
    if (!prints(reference.source)) {
        std::fprintf(stderr, "%s: only a page prints\n", reference.source.path.string().c_str());
        return false;
    }
    fs::file_time_type changed = lastChange(reference.source.path.parent_path());
    std::map<fs::path, fs::file_time_type>::iterator at = printed.find(out);
    if (at != printed.end() && at->second == changed && !page_host::PageHost::documentFiles(out).empty())
        return true;
    printed.erase(out);
    if (!printHtml(reference.source.path, reference.source.capture, reference.source.scripts, out,
            reference.printed.format))
        return false;
    printed[out] = changed;
    return true;
}

std::shared_ptr<Loaded> load(const Source& source, TextBackends text, page_host::Media media, PrGpuDevice* PR_NULLABLE device) {
    std::shared_ptr<Loaded> loaded = std::make_shared<Loaded>();
    loaded->source = source;
    loaded->shaper.reset(prTextShaperCreate(text.shaper));
    PrTextShaper* textShaper = loaded->shaper.get();
    switch (source.type) {
        case SourceType_Page:
            loaded->html = loadHtml(source.path, source.query, source.settings, source.capture, source.scripts, media,
                textShaper, text.hinting, hinterOf(text.hinting), device);
            break;
        case SourceType_Lottie:
            loaded->lottie = loadLottie(source.path, source.settings, textShaper, text.hinting, hinterOf(text.hinting));
            break;
        case SourceType_Rlottie:
            loaded->rlottie = loadRlottie(source.path);
            break;
        case SourceType_Pdf:
            loaded->pdf = loadPdf(source.path);
            break;
    }
    return loaded;
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
    return loaded.frame;
}

bool pixels(Loaded& loaded, PrVec2 viewport, uint32_t width, uint32_t height, float pixelRatio, std::vector<uint8_t>& bgra) {
    std::lock_guard<std::mutex> lock(loaded.mutex);
    if (loaded.rlottie == nullptr)
        return false;
    loaded.viewport = viewport;
    rlottiePixels(*loaded.rlottie, viewport, width, height, pixelRatio, loaded.frame, bgra);
    return true;
}

bool pixelsOnly(const Loaded& loaded) {
    return loaded.source.type == SourceType_Rlottie;
}

void draw(Loaded& loaded, PrDrawList* list, float width, float height, const PrAppDisplay& display,
        const PrAppUserPreferences& preferences) {
    std::lock_guard<std::mutex> lock(loaded.mutex);
    loaded.viewport = PrVec2{width, height};
    if (loaded.html != nullptr)
        drawHtml(*loaded.html, list, width, height, display, preferences);
    else if (loaded.lottie != nullptr)
        drawLottie(*loaded.lottie, list, PrVec2{width, height}, loaded.frame);
    else if (loaded.pdf != nullptr)
        drawPdf(*loaded.pdf, list, width, height, display.pixelRatio);
}

void input(Loaded& loaded, const PrAppEvent& event) {
    std::lock_guard<std::mutex> lock(loaded.mutex);
    if (loaded.html != nullptr)
        htmlInput(*loaded.html, event);
    else if (loaded.pdf != nullptr && event.kind == PrAppEventKind_Scroll)
        pdfScrollTo(*loaded.pdf, pdfScroll(*loaded.pdf) + prAppEventScrollPixels(&event, kPrAppScrollLineHeight));
}

PrVec2 scroll(Loaded& loaded) {
    std::lock_guard<std::mutex> lock(loaded.mutex);
    if (loaded.html != nullptr)
        return htmlScroll(*loaded.html);
    if (loaded.pdf != nullptr)
        return pdfScroll(*loaded.pdf);
    return PrVec2{0, 0};
}

void scrollTo(Loaded& loaded, PrVec2 offset) {
    std::lock_guard<std::mutex> lock(loaded.mutex);
    if (loaded.html != nullptr)
        htmlScrollTo(*loaded.html, offset);
    else if (loaded.pdf != nullptr)
        pdfScrollTo(*loaded.pdf, offset);
}

float contentWidth(Loaded& loaded) {
    std::lock_guard<std::mutex> lock(loaded.mutex);
    if (loaded.html != nullptr)
        return htmlContentWidth(*loaded.html);
    if (loaded.pdf != nullptr)
        return pdfContentWidth(*loaded.pdf);
    return loaded.viewport.x;
}

PrAppOutput output(Loaded& loaded, PrAppWindow* window, double time) {
    std::lock_guard<std::mutex> lock(loaded.mutex);
    if (loaded.html == nullptr)
        return PrAppOutput{};
    htmlSetWindow(*loaded.html, window);
    return htmlOutput(*loaded.html, time);
}

const std::vector<Document>& Collection::documents() const {
    if (listed_ == nullptr)
        listed_ = std::make_shared<const std::vector<Document>>(list());
    return *listed_;
}

const std::vector<Collection>& allCollections() {
    static const std::vector<Collection> collections = [] {
        std::vector<Collection> all;
        std::vector<Collection> (*const suites[])() = {
            praxCollections, wptCollections, mathmlCollections, svgCollections, lottieCollections, markdownCollections,
            princeCollections,
        };
        for (std::vector<Collection> (*suite)() : suites) {
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
