// SVG documents as sources (source_svg.h).

#include "tools/viewer/source_svg.h"

#include "pr/dom_xml.h"
#include "pr/svg.h"
#include "pr/svg_render.h"

#include "tools/suite/suite_render.h"
#include "tools/viewer/documents.h"

#include <utility>

using namespace pr;
using namespace suite;

namespace viewer {

namespace fs = std::filesystem;

namespace {

constexpr const char* kThirdParty = PR_THIRD_PARTY_DIR;

PrDomDocument* PR_NULLABLE parseSvg(const fs::path& path) {
    std::string source = readFile(path);
    PrDomDocument* document = prDomParseXml(source, "image/svg+xml", nullptr);
    prSvgEnable(document);
    return document;
}

} // namespace

// A parsed SVG file, with the documents that its references load, and the settings that it renders with.
struct SvgSource {
    fs::path path;
    PrDomDocument* PR_NULLABLE document = nullptr;
    std::vector<std::pair<fs::path, PrDomDocument*>> referenced;
    SuiteRender render{};
    PrTextShaper* shaper;

    SvgSource(fs::path from, const fs::path& settings, PrTextShaper* shaper, PrFontHinting hinting,
            PrFontHinter* PR_NULLABLE hinter) : path(std::move(from)), document(parseSvg(path)), shaper(shaper) {
        if (!settings.empty()) {
            std::string root = fs::path(kThirdParty).parent_path().string();
            std::string file = settings.string();
            suiteRenderRead(&render, file, root);
        }
        if (render.fonts != nullptr)
            prFontSetSetHinting(render.fonts, hinting, hinter);
    }
    SvgSource(const SvgSource&) = delete;
    SvgSource& operator=(const SvgSource&) = delete;
    ~SvgSource() {
        suiteRenderReset(&render);
        prDomDocumentRelease(document);
        for (auto& [loaded, referencedDocument] : referenced)
            prDomDocumentRelease(referencedDocument);
    }

    // Loads a file that a reference names, beside the document that names it.
    static PrDomDocument* PR_NULLABLE load(void* PR_NULLABLE context, PrDomDocument* from, PrStringView url) {
        SvgSource* svg = static_cast<SvgSource*>(context);
        fs::path base = svg->path;
        for (const auto& [path, loaded] : svg->referenced)
            if (loaded == from)
                base = path;
        fs::path path = (base.parent_path() / std::string(url.data, url.size)).lexically_normal();
        for (const auto& [loaded, loadedDocument] : svg->referenced)
            if (loaded == path)
                return loadedDocument;
        if (!fs::exists(path))
            return nullptr;
        PrDomDocument* loadedDocument = parseSvg(path);
        svg->referenced.emplace_back(path, loadedDocument);
        return loadedDocument;
    }
};

std::shared_ptr<SvgSource> loadSvg(const fs::path& path, const fs::path& settings, PrTextShaper* shaper,
        PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter) {
    std::shared_ptr<SvgSource> svg = std::make_shared<SvgSource>(path, settings, shaper, hinting, hinter);
    return svg->document != nullptr ? svg : nullptr;
}

void drawSvg(SvgSource& svg, PrDrawList* list, float width, float height) {
    PrDomElement* root = prDomDocumentElement(svg.document);
    PrSvgElement* element = root != nullptr ? prSvgElementQuery(prDomElementAsEventTarget(root)) : nullptr;
    PrSvgDrawOptions options{};
    options.fonts = svg.render.fonts;
    options.shaper = svg.shaper;
    options.load = SvgSource::load;
    options.loadContext = &svg;
    if (element != nullptr)
        prSvgElementDraw(element, PrVec2{width, height}, &options, list);
}

} // namespace viewer
