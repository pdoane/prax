// resvg's tests (suite_svg.h), selected as their runner, tools/suite/reftest.py, selects them. A test renders as the
// suite's settings specify (suites/resvg/render.txt).

#include "tools/viewer/suite_svg.h"

#include <algorithm>

namespace viewer {

namespace {

namespace fs = std::filesystem;

constexpr const char* kThirdParty = PR_THIRD_PARTY_DIR;
constexpr const char* kSuites = PR_SUITES_DIR;

// Returns the suite's render settings.
fs::path settingsOf(const char* suite) {
    return fs::path(kSuites) / suite / "render.txt";
}

// Reads a PNG's size from its header.
bool pngSize(const fs::path& path, float* width, float* height) {
    std::string header = readFile(path).substr(0, 24);
    if (header.size() < 24)
        return false;
    auto word = [&](size_t at) {
        return uint32_t(uint8_t(header[at])) << 24 | uint32_t(uint8_t(header[at + 1])) << 16
               | uint32_t(uint8_t(header[at + 2])) << 8 | uint32_t(uint8_t(header[at + 3]));
    };
    *width = float(word(16));
    *height = float(word(20));
    return true;
}

// Returns every file under `root` with the extension, in path order, excluding `support` and `resources` directories.
std::vector<fs::path> filesUnder(const fs::path& root, const char* extension) {
    std::vector<fs::path> files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
        if (error)
            break;
        std::string name = it->path().filename().string();
        if (it->is_directory() && (name == "support" || name == "resources")) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file() && it->path().extension() == extension)
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Returns true if the resvg test's title marks its behavior as undefined. The test's PNG then shows that, rather than
// what the test should draw.
bool undefinedBehavior(const std::string& text) {
    size_t title = text.find("<title>");
    size_t end = text.find("</title>", title);
    return title != std::string::npos && end != std::string::npos
           && text.substr(title, end - title).find("(UB)") != std::string::npos;
}

// Returns resvg's tests, each against its PNG, at the PNG's size.
std::vector<Document> resvg() {
    std::vector<Document> documents;
    fs::path root = fs::path(kThirdParty) / "resvg-tests" / "tests";
    for (const fs::path& svg : filesUnder(root, ".svg")) {
        fs::path png = svg;
        png.replace_extension(".png");
        if (!fs::exists(png) || undefinedBehavior(readFile(svg)))
            continue;
        Document d{fs::relative(svg, root).string(), "the resvg test suite, against its PNG",
                   fileSource(svg, settingsOf("resvg"))};
        d.references.push_back(Reference{"corpus", fileSource(png)});
        pngSize(png, &d.width, &d.height);
        documents.push_back(std::move(d));
    }
    return documents;
}

} // namespace

std::vector<Collection> svgCollections() {
    std::vector<Collection> collections;
    // Compared at the density of its PNGs.
    if (fs::exists(fs::path(kThirdParty) / "resvg-tests"))
        collections.push_back(Collection{"resvg", 500, 500, 1, resvg});
    return collections;
}

} // namespace viewer
