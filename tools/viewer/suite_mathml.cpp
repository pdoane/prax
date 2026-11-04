// The visual tests of MathML (suite_mathml.h), each of which sets formulas beside TeX's setting of them.
//
// Mozilla's MathML torture test names its script and its fonts' style sheets from the root of the server, as
// /MathFonts/..., so it loads from the third-party server, once for each math font of the checkout, which its query
// chooses. eyeasme.com's browser test is listed whole, and each of its formulas on its own, written to the viewer's
// cache as a page of the formula with the test's styles, against pages of the images of TeX's and Firefox's settings of
// it. lib/tex's sample is a Markdown document, which the engine loads as text/markdown.

#include "tools/viewer/suite_mathml.h"

#include "tools/viewer/source_html.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <string_view>

namespace viewer {

namespace {

namespace fs = std::filesystem;

constexpr const char* kThirdParty = PR_THIRD_PARTY_DIR;

// The font that the torture test is listed with first: the one closest to TeX's own Computer Modern.
constexpr const char* kFirstFont = "NewComputerModern";

// Writes `text` to `path` unless the file holds it already, so that a listing does not touch files it wrote before.
void writeIfChanged(const fs::path& path, const std::string& text) {
    std::error_code error;
    if (fs::exists(path, error) && readFile(path) == text)
        return;
    fs::create_directories(path.parent_path(), error);
    std::ofstream(path, std::ios::binary) << text;
}

// Returns the text between `open` and `close` that follows `from`, or empty if either is missing. `at` is set past the
// close.
std::string_view between(std::string_view text, std::string_view open, std::string_view close, size_t& at) {
    size_t start = text.find(open, at);
    size_t end = start == std::string_view::npos ? start : text.find(close, start + open.size());
    if (end == std::string_view::npos) {
        at = std::string_view::npos;
        return {};
    }
    at = end + close.size();
    return text.substr(start + open.size(), end - start - open.size());
}

// Returns the name that font-selection.js gives the math font of the directory `font`, or the directory's name if it
// gives none.
std::string fontTitle(const std::string& selection, const std::string& font) {
    size_t at = 0;
    std::string_view title = between(selection, "\"" + font + "\": \"", "\"", at);
    return title.empty() ? font : std::string(title);
}

// Returns the torture test once for each math font of the checkout, a directory with a mathfonts.css, New Computer
// Modern first and then by name.
std::vector<Document> torture(const fs::path& checkout) {
    std::vector<std::string> fonts;
    std::error_code error;
    for (fs::directory_iterator it(checkout, error), end; it != end; it.increment(error)) {
        if (error)
            break;
        if (it->is_directory() && fs::is_regular_file(it->path() / "mathfonts.css", error))
            fonts.push_back(it->path().filename().string());
    }
    std::sort(fonts.begin(), fonts.end(), [](const std::string& a, const std::string& b) {
        if ((a == kFirstFont) != (b == kFirstFont))
            return a == kFirstFont;
        return a < b;
    });
    std::string selection = readFile(checkout / "font-selection.js");
    std::vector<Document> documents;
    for (const std::string& font : fonts) {
        Source source = fileSource(checkout / "mozilla_mathml_test" / "index.html");
        source.query = "fontFamily=" + queryEscaped(font);
        Document d{"mozilla/" + font, "Mozilla's MathML torture test in " + fontTitle(selection, font), source};
        d.folders = {"Mozilla MathML test"};
        documents.push_back(std::move(d));
    }
    return documents;
}

// Returns eyeasme.com's browser test whole, and then each formula of its table: a row whose second cell is the image of
// TeX's setting and whose last cell is the formula in MathML.
std::vector<Document> eyeasme(const fs::path& checkout) {
    fs::path page = checkout / "MathML_browser_test.html";
    std::vector<Document> documents;
    Document whole{"eyeasme/MathML_browser_test", "eyeasme.com's MathML browser test", fileSource(page)};
    whole.folders = {"eyeasme"};
    documents.push_back(std::move(whole));

    std::string text = readFile(page);
    size_t at = 0;
    std::string style(between(text, "<style>", "</style>", at));
    fs::path cache = viewerCache() / "mathml" / "eyeasme";
    for (size_t row = text.find("<tr>"); row != std::string::npos; row = text.find("<tr>", row + 1)) {
        size_t end = text.find("</tr>", row);
        if (end == std::string::npos)
            break;
        std::string_view cells = std::string_view(text).substr(row, end - row);
        size_t cursor = 0;
        std::string_view image = between(cells, "<img src=\"Images/TeX/", "\"", cursor);
        size_t last = cells.rfind("<td");
        size_t open = last == std::string_view::npos ? last : cells.find('>', last);
        size_t close = cells.rfind("</td>");
        if (image.empty() || open == std::string_view::npos || close == std::string_view::npos || close < open)
            continue;
        std::string name = fs::path(std::string(image)).stem().string();
        cursor = 0;
        std::string title(between(cells, "alt=\"Image of TeX rendering of ", "\"", cursor));
        fs::path formula = cache / (name + ".html");
        // The formula sits at the page's top left and shrinks to its width, as the image of TeX's setting is drawn.
        writeIfChanged(formula, "<!DOCTYPE html>\n<html lang=\"en\">\n<meta charset=\"utf-8\">\n<style>" + style +
            "  body { margin: 0; }\n</style>\n<body>\n<div style=\"display: inline-block\">" +
            std::string(cells.substr(open + 1, close - open - 1)) + "</div>\n");
        if (!title.empty() && title.starts_with("the "))
            title = "The " + title.substr(4);
        Document d{
            "eyeasme/" + name,
            (title.empty() ? name : title) + ", from eyeasme.com's MathML browser test",
            fileSource(formula),
        };
        // Each image shows at the page's top left at one pixel to the px, where the formula sits at its own size,
        // rather than fitted to the viewport as an image's own page shows it (imagePage).
        auto setting = [&](const char* engine, const char* directory) {
            fs::path image = checkout / "Images" / directory / (name + ".png");
            fs::path shown = cache / (name + "." + directory + ".html");
            writeIfChanged(shown, "<!DOCTYPE html>\n<style>html, body { margin: 0 } img { display: block }</style>\n"
                "<img src=\"" + fileUrl(image) + "\">\n");
            return Reference{engine, fileSource(shown)};
        };
        d.references.push_back(setting("TeX", "TeX"));
        d.references.push_back(setting("Firefox", "MathML"));
        d.folders = {"eyeasme", "Formulas"};
        documents.push_back(std::move(d));
    }
    return documents;
}

std::vector<Document> mathml() {
    std::vector<Document> documents;
    std::error_code error;
    fs::path mathfonts = fs::path(kThirdParty) / "MathFonts";
    if (thirdPartyServed() && fs::is_regular_file(mathfonts / "mozilla_mathml_test" / "index.html", error)) {
        for (Document& d : torture(mathfonts))
            documents.push_back(std::move(d));
    }
    fs::path eyeasmeCheckout = fs::path(kThirdParty) / "eyeasme";
    if (fs::is_regular_file(eyeasmeCheckout / "MathML_browser_test.html", error)) {
        for (Document& d : eyeasme(eyeasmeCheckout))
            documents.push_back(std::move(d));
    }
    fs::path sample = fs::path(PR_TEX_TESTS_DIR) / "mathematics.md";
    if (fs::is_regular_file(sample, error)) {
        Document d{"tex/mathematics.md", "Real mathematics in TeX, through lib/tex and Markdown", fileSource(sample)};
        d.folders = {"TeX"};
        documents.push_back(std::move(d));
    }
    return documents;
}

} // namespace

std::vector<Collection> mathmlCollections() {
    return {Collection{"MathML", 1000, 800, mathml}};
}

} // namespace viewer
