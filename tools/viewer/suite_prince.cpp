// Prince's samples (suite_prince.h), from the checkout that tools/fetch/checkouts.py pins. A sample is a directory with
// a README that says how Prince builds it: the HTML or XHTML file that Prince prints, whether it runs the file's
// scripts (`prince -j`), and so which PDF beside the file is Prince's. A directory may hold several samples.
//
// A sample loads from the checkout, so its style sheets, images, fonts and scripts resolve as they do in a browser that
// opens the file. What it loads from other origins is answered by its capture, which `./dev.sh fetch prince_samples`
// makes, so that it prints the same on every run and without the network. Each sample is compared with Prince's PDF,
// with the engine's PDF and SVG, which the engine prints into the viewer's cache when the sample opens (print), and
// with the system's browser showing the page.

#include "tools/viewer/suite_prince.h"

#include <sstream>

namespace viewer {

namespace {

namespace fs = std::filesystem;

constexpr const char* kThirdParty = PR_THIRD_PARTY_DIR;

// A sample, as its README builds it: the page that Prince prints, under the sample's directory, and the PDF that it
// makes there.
struct Sample {
    const char* name;
    const char* directory;
    const char* page;
    const char* pdf;
    bool scripts;               // Prince runs the page's scripts (`prince -j`)
};

constexpr Sample kSamples[] = {
    {"campaign", "campaign", "letter.html", "letter.pdf", true},
    {"catalogue", "catalogue", "PrinceCatalogue.html", "PrinceCatalogue.pdf", false},
    {"catalogue-cover", "catalogue", "Cover.html", "Cover.pdf", false},
    {"digte", "digte", "digte.html", "digte.pdf", false},
    {"flyer", "flyer", "flyer.html", "flyer.pdf", false},
    {"invoice-colorful", "invoice-colorful", "invoicesample.html", "invoicesample.pdf", false},
    {"invoice-plain", "invoice-plain", "index.html", "index.pdf", false},
    {"letter", "letter", "letter.html", "letter.pdf", false},
    {"magic6", "magic6", "magic.xhtml", "magic.pdf", false},
    {"magic8", "magic8", "index.html", "index.pdf", true},
    {"malthus", "malthus", "essay.html", "essay.pdf", true},
    {"textbook", "textbook", "somatosensory.html", "somatosensory.pdf", false},
    {"twist", "twist", "oliver-twist.html", "oliver-twist.pdf", true},
    {"usenix", "usenix", "example.html", "example.pdf", true},
    {"webarch", "webarch", "webarch.html", "webarch.pdf", false},
    {"winelist", "winelist", "winelist.html", "winelist.pdf", false},
};

// A document that the engine prints of each sample: its reference's name, its writer and its file's extension.
struct Written {
    const char* name;
    PrDrawWriterBackend format;
    const char* extension;
};

constexpr Written kWritten[] = {
    {"Prax PDF", PrDrawWriterBackend_Pdf, ".pdf"},
    {"Prax SVG", PrDrawWriterBackend_Svg, ".svg"},
};

fs::path root() {
    return fs::path(kThirdParty) / "prince-samples";
}

// Returns the capture of the sample's page, which holds what the page loads from other origins.
fs::path captureOf(const Sample& sample) {
    fs::path page = fs::path(sample.directory) / sample.page;
    return fs::path(kThirdParty) / "prince-samples-capture" / page.replace_extension(".json");
}

// Returns the title that the README in `directory` gives its sample, the text of its first heading, or empty.
std::string readmeTitle(const fs::path& directory) {
    std::istringstream in(readFile(directory / "README.md"));
    for (std::string line; std::getline(in, line);) {
        if (line.starts_with("#")) {
            size_t start = line.find_first_not_of("# ");
            return start == std::string::npos ? std::string() : line.substr(start);
        }
    }
    return {};
}

std::vector<Document> samples() {
    std::vector<Document> documents;
    for (const Sample& sample : kSamples) {
        fs::path directory = root() / sample.directory;
        Document d{sample.name, readmeTitle(directory), fileSource(directory / sample.page)};
        // Most samples load their fonts from the web, as Prince did when it printed them, so what they load from there
        // is answered by their capture. A sample runs its scripts only if Prince ran them.
        d.source.capture = captureOf(sample);
        d.source.scripts = sample.scripts;
        d.references.push_back(pdfReference("Prince PDF", directory / sample.pdf));
        // The engine prints the sample by each of its writers, into the viewer's cache.
        for (const Written& written : kWritten) {
            Reference prax{written.name, d.source};
            prax.printed = Printed{
                .path = viewerCache() / "prince-samples" / (std::string(sample.name) + written.extension),
                .format = written.format,
            };
            d.references.push_back(std::move(prax));
        }
        documents.push_back(std::move(d));
    }
    return documents;
}

} // namespace

std::vector<Collection> princeCollections() {
    std::error_code error;
    if (!fs::exists(root() / "README.md", error))
        return {};
    // A US letter page in CSS pixels, which most of the samples print on. The samples are written for print, and every
    // render that they are compared with is printed except the browser's, so the engine prints them by default.
    Collection collection{"Prince samples", 816, 1056, samples};
    collection.media = page_host::Media_Print;
    return {collection};
}

} // namespace viewer
