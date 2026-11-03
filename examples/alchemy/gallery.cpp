// The gallery: a directory of PNG files, newest first.

#include "examples/alchemy/gallery.h"

#include "pr/ascii.h"
#include "pr/file.h"
#include "pr/string_ops.h"

#include "examples/alchemy/text.h"

#include <algorithm>
#include <stdlib.h>

namespace alchemy {

using namespace pr;

namespace {

bool endsWithPng(PrStringView name) {
    return name.size > 4 && prAsciiEndsWithFold(name, ".png");
}

std::string joinPath(PrStringView dir, PrStringView name) {
    std::string path = stringOf(dir);
    if (!path.empty() && path.back() != '/')
        path += '/';
    path.append(name.begin(), name.end());
    return path;
}

bool exists(const std::string& path) {
    PrFileInfo info{};
    return prFileInfo(path, &info);
}

// Returns a name that cannot leave the gallery's directory and that a listing finds: the basename without leading dots,
// ending in .png.
std::string galleryFilename(PrStringView filename) {
    PrStringView base = pathFilename(filename);
    while (base.size != 0 && base.data[0] == '.')
        base = prStringSlice(base, 1, base.size - 1);
    if (base.size == 0)
        return "image.png";
    std::string out = stringOf(base);
    if (!endsWithPng(out))
        out += ".png";
    return out;
}

// Returns a path in `dir` that no file has: `filename`, or else its stem with a number.
std::string uniquePath(PrStringView dir, const std::string& filename) {
    std::string path = joinPath(dir, filename);
    if (!exists(path))
        return path;
    std::string stem = filename.substr(0, filename.size() - 4);
    for (uint32_t i = 1; i < 10000; ++i) {
        path = joinPath(dir, stem + "-" + std::to_string(i) + ".png");
        if (!exists(path))
            return path;
    }
    return path;
}

void collect(void* PR_NULLABLE context, const PrFileEntry* entry) {
    Gallery* gallery = static_cast<Gallery*>(context);
    PrStringView name = entry->name;
    if (entry->info.kind != PrFileKind_Regular || !endsWithPng(name) || name.data[0] == '.')
        return;
    gallery->entries.push_back({joinPath(gallery->dir, name), stringOf(name), entry->info.modified});
}

} // namespace

std::string galleryDirectory() {
    const char* named = getenv("ALCHEMY_DIR");
    if (named != nullptr && named[0] != 0)
        return named;
    PrString data = prFileDataDirectory();
    std::string base = takeString(&data);
    return base.empty() ? std::string("Alchemy/images") : joinPath(base, "Alchemy/images");
}

void galleryOpen(Gallery& gallery, PrStringView dir) {
    gallery.dir = stringOf(dir);
    prFileCreateDirectory(gallery.dir);
    galleryRescan(gallery);
}

void galleryRescan(Gallery& gallery) {
    std::vector<std::string> kept = gallerySelectionPaths(gallery);
    std::string lead = gallerySelected(gallery) != nullptr ? gallerySelected(gallery)->filename : std::string();
    gallery.entries.clear();
    prFileEachEntry(gallery.dir, collect, &gallery);
    std::sort(gallery.entries.begin(), gallery.entries.end(), [](const GalleryEntry& a, const GalleryEntry& b) {
        return a.modified != b.modified ? a.modified > b.modified : a.filename > b.filename;
    });
    ++gallery.revision;
    // The selection carries over by name. If none of it remains, the newest image is selected.
    gallery.selected.assign(gallery.entries.size(), false);
    uint32_t newLead = 0;
    bool any = false;
    for (uint32_t i = 0; i < gallery.entries.size(); ++i) {
        const GalleryEntry& entry = gallery.entries[i];
        if (std::find(kept.begin(), kept.end(), entry.path) != kept.end()) {
            gallery.selected[i] = true;
            if (!any)
                newLead = i;
            any = true;
        }
        if (entry.filename == lead)
            newLead = i;
    }
    if (!any)
        gallerySelect(gallery, 0);
    else
        gallery.lead = gallery.selection.lead = gallery.selection.anchor = newLead;
}

bool gallerySave(Gallery& gallery, PrStringView filename, const std::string& png, std::string* savedPath) {
    std::string path = uniquePath(gallery.dir, galleryFilename(filename));
    if (!prFileWrite(path, PrByteView{reinterpret_cast<const uint8_t*>(png.data()), png.size()}))
        return false;
    PrFileInfo info{};
    prFileInfo(path, &info);
    GalleryEntry entry{path, stringOf(pathFilename(path)), info.modified};
    gallery.entries.insert(gallery.entries.begin(), entry);
    gallery.selected.insert(gallery.selected.begin(), false);
    gallerySelect(gallery, 0);
    *savedPath = path;
    return true;
}

const GalleryEntry* PR_NULLABLE gallerySelected(const Gallery& gallery) {
    return gallery.lead < gallery.entries.size() ? &gallery.entries[gallery.lead] : nullptr;
}

void gallerySelect(Gallery& gallery, uint32_t index) {
    gallery.selected.assign(gallery.entries.size(), false);
    if (index >= gallery.entries.size()) {
        gallery.lead = 0;
        gallery.selection = PrKitSelection{};
        return;
    }
    gallery.selected[index] = true;
    gallery.lead = index;
    gallery.selection.anchor = index;
    gallery.selection.lead = index;
}

void galleryStep(Gallery& gallery, int32_t delta) {
    if (gallery.entries.empty())
        return;
    int64_t index = int64_t(gallery.lead) + delta;
    index = std::clamp<int64_t>(index, 0, int64_t(gallery.entries.size()) - 1);
    gallerySelect(gallery, uint32_t(index));
}

uint32_t galleryIndexOf(const Gallery& gallery, PrStringView path) {
    for (uint32_t i = 0; i < gallery.entries.size(); ++i) {
        if (gallery.entries[i].path == path)
            return i;
    }
    return uint32_t(gallery.entries.size());
}

std::vector<std::string> gallerySelectionPaths(const Gallery& gallery) {
    std::vector<std::string> paths;
    for (uint32_t i = 0; i < gallery.entries.size() && i < gallery.selected.size(); ++i) {
        if (gallery.selected[i])
            paths.push_back(gallery.entries[i].path);
    }
    if (paths.empty() && gallerySelected(gallery) != nullptr)
        paths.push_back(gallerySelected(gallery)->path);
    return paths;
}

uint32_t galleryDelete(Gallery& gallery, const std::vector<std::string>& paths) {
    uint32_t removed = 0;
    uint32_t last = gallery.lead;
    for (const std::string& path : paths) {
        uint32_t index = galleryIndexOf(gallery, path);
        if (index == gallery.entries.size() || !prFileRemove(path))
            continue;
        gallery.entries.erase(gallery.entries.begin() + index);
        gallery.selected.erase(gallery.selected.begin() + index);
        last = index;
        ++removed;
    }
    // The neighbor that took the last removed entry's place, or the entry before it at the end of the list.
    uint32_t count = uint32_t(gallery.entries.size());
    if (removed != 0)
        gallerySelect(gallery, count == 0 ? 0 : std::min(last, count - 1));
    return removed;
}

PrStringView pathFilename(PrStringView path) {
    size_t start = path.size;
    while (start > 0 && path.data[start - 1] != '/' && path.data[start - 1] != '\\')
        --start;
    return prStringSlice(path, start, path.size - start);
}

} // namespace alchemy
