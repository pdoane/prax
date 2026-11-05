// The engine's own pages (suite_prax.h). A page is an HTML file in a directory under suites/prax, other than the
// `shared` directory, which holds what every page uses. Pages are served from the checkout, so a page's resources,
// including the checkout's fonts, resolve as they do in a browser that opens the file.
//
// The pages come in the order that the index files give (tools/suite/prax/suite.py describes them): the directories as
// suites/prax/index.txt lists them, and in each directory, its own pages and then each section's pages, as its
// index.txt lists them. The tree lists a section's pages under the section's number and title. A directory or page that
// no index lists comes after the listed ones, in the order of its name, so that a page shows before it is indexed.

#include "tools/viewer/suite_prax.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace viewer {

namespace {

namespace fs = std::filesystem;

fs::path root() {
    return fs::path(PR_SUITES_DIR) / "prax";
}

// A line of an index, without its indent, and whether it had one. Blank lines and comments are skipped.
struct IndexLine {
    std::string text;
    bool indented;
};

std::vector<IndexLine> indexLines(const fs::path& path) {
    std::vector<IndexLine> lines;
    std::istringstream in(readFile(path));
    for (std::string line; std::getline(in, line);) {
        size_t first = line.find_first_not_of(" \t");
        size_t last = line.find_last_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#')
            continue;
        lines.push_back(IndexLine{line.substr(first, last - first + 1), first > 0});
    }
    return lines;
}

// A page, and the section that it walks: "10.3.7 Absolutely positioned, non-replaced elements", or empty for a page of
// the directory as a whole.
struct Listed {
    std::string page;
    std::string section;
};

// Returns the pages of a directory in its index's order. A section's line holds its anchor, its number and its title.
std::vector<Listed> listed(const fs::path& directory) {
    std::vector<Listed> pages;
    std::string section;
    for (const IndexLine& line : indexLines(directory / "index.txt")) {
        if (line.text.ends_with(".html")) {
            pages.push_back(Listed{line.text, line.indented ? section : std::string()});
            continue;
        }
        size_t space = line.text.find(' ');
        section = space == std::string::npos ? line.text : line.text.substr(space + 1);
    }
    return pages;
}

// Returns the HTML files of a directory, by name.
std::vector<std::string> htmlFiles(const fs::path& directory) {
    std::vector<std::string> files;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; it != end; it.increment(error)) {
        if (error)
            break;
        if (it->is_regular_file() && it->path().extension() == ".html")
            files.push_back(it->path().filename().string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<Document> pages() {
    std::vector<std::string> directories;
    for (const IndexLine& line : indexLines(root() / "index.txt"))
        directories.push_back(line.text.substr(0, line.text.find(' ')));
    std::vector<std::string> unlisted;
    std::error_code error;
    for (fs::directory_iterator it(root(), error), end; it != end; it.increment(error)) {
        if (error)
            break;
        std::string name = it->path().filename().string();
        if (it->is_directory() && name != "shared" &&
                std::find(directories.begin(), directories.end(), name) == directories.end())
            unlisted.push_back(name);
    }
    std::sort(unlisted.begin(), unlisted.end());
    directories.insert(directories.end(), unlisted.begin(), unlisted.end());

    std::vector<Document> documents;
    auto add = [&](const std::string& directory, const std::string& page, const std::string& section) {
        fs::path file = root() / directory / page;
        Document d{directory + "/" + page, htmlTitle(file), fileSource(file)};
        d.folders = {directory};
        if (!section.empty())
            d.folders.push_back(section);
        documents.push_back(std::move(d));
    };
    for (const std::string& directory : directories) {
        std::set<std::string> seen;
        for (const Listed& l : listed(root() / directory)) {
            if (fs::is_regular_file(root() / directory / l.page, error) && seen.insert(l.page).second)
                add(directory, l.page, l.section);
        }
        for (const std::string& page : htmlFiles(root() / directory)) {
            if (!seen.contains(page))
                add(directory, page, "Not in the index");
        }
    }
    return documents;
}

} // namespace

std::vector<Collection> praxCollections() {
    std::error_code error;
    if (!fs::is_directory(root(), error))
        return {};
    return {Collection{"Prax", 600, 800, pages}};
}

} // namespace viewer
