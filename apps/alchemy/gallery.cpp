// The gallery: a directory of PNG files, newest first.

#include "apps/alchemy/gallery.h"

#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/environment.h"
#include "pr/file.h"
#include "pr/min_max.h"
#include "pr/sort.h"
#include "pr/string.h"
#include "pr/string_ops.h"

namespace alchemy {

using namespace pr;

namespace {

bool endsWithPng(PrStringView name) {
    return name.size > 4 && prAsciiEndsWithFold(name, ".png");
}

String joinPath(PrStringView dir, PrStringView name) {
    String path(dir);
    if (!path.isEmpty() && !prStringEndsWith(path, "/"))
        path.append('/');
    path.append(name);
    return path;
}

bool exists(PrStringView path) {
    PrFileInfo info{};
    return prFileInfo(path, &info);
}

// Returns a name that cannot leave the gallery's directory and that a listing finds: the basename without leading dots,
// ending in .png.
String galleryFilename(PrStringView filename) {
    PrStringView base = prFilePathName(filename);
    while (!base.isEmpty() && base.data[0] == '.')
        base = prStringFrom(base, 1);
    if (base.isEmpty())
        return "image.png";
    String out(base);
    if (!endsWithPng(out))
        out.append(".png");
    return out;
}

// Returns a path in `dir` that no file has: `filename`, or else its stem with a number.
String uniquePath(PrStringView dir, PrStringView filename) {
    String path = joinPath(dir, filename);
    if (!exists(path))
        return path;
    PrStringView stem = prStringSlice(filename, 0, filename.size - 4);
    for (uint32_t i = 1; i < 10000; ++i) {
        path = joinPath(dir, format("{}-{}.png", stem, i));
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
    gallery->entries.push(GalleryEntry{
        .path = joinPath(gallery->dir, name),
        .filename = name,
        .modified = entry->info.modified,
    });
}

} // namespace

String galleryDirectory() {
    String named = String::adopt(prEnvironmentVariable("ALCHEMY_DIR"));
    if (!named.isEmpty())
        return named;
    String base = String::adopt(prFileUserDirectory(PrFileUserDirectory_Data));
    return base.isEmpty() ? String("Alchemy/images") : joinPath(base, "Alchemy/images");
}

void galleryOpen(Gallery& gallery, PrStringView dir) {
    gallery.dir = dir;
    prFileCreateDirectory(gallery.dir);
    galleryRescan(gallery);
}

void galleryRescan(Gallery& gallery) {
    Array<String> kept = gallerySelectionPaths(gallery);
    String lead = gallerySelected(gallery) != nullptr ? gallerySelected(gallery)->filename : String();
    gallery.entries.clear();
    prFileEachEntry(gallery.dir, collect, &gallery);
    sortItems(gallery.entries.span(), [](const GalleryEntry& a, const GalleryEntry& b) {
        if (a.modified.ticks != b.modified.ticks)
            return a.modified.ticks > b.modified.ticks;
        return prStringCompare(a.filename, b.filename) > 0;
    });
    ++gallery.revision;
    // The selection carries over by name. If none of it remains, the newest image is selected.
    gallery.selected.clear();
    gallery.selected.resize(gallery.entries.count());
    uint32_t newLead = 0;
    bool any = false;
    for (uint32_t i = 0; i < gallery.entries.count(); ++i) {
        const GalleryEntry& entry = gallery.entries[i];
        if (kept.contains(entry.path)) {
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

bool gallerySave(Gallery& gallery, PrStringView filename, PrByteView png, String* savedPath) {
    String path = uniquePath(gallery.dir, galleryFilename(filename));
    if (!prFileWrite(path, png))
        return false;
    PrFileInfo info{};
    prFileInfo(path, &info);
    gallery.entries.insert(0, GalleryEntry{.path = path, .filename = prFilePathName(path), .modified = info.modified});
    gallery.selected.insert(0, false);
    gallerySelect(gallery, 0);
    *savedPath = path;
    return true;
}

const GalleryEntry* PR_NULLABLE gallerySelected(const Gallery& gallery) {
    return gallery.lead < gallery.entries.count() ? &gallery.entries[gallery.lead] : nullptr;
}

void gallerySelect(Gallery& gallery, uint32_t index) {
    gallery.selected.clear();
    gallery.selected.resize(gallery.entries.count());
    if (index >= gallery.entries.count()) {
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
    if (gallery.entries.isEmpty())
        return;
    int64_t index = int64_t(gallery.lead) + delta;
    index = clamp<int64_t>(index, 0, int64_t(gallery.entries.count()) - 1);
    gallerySelect(gallery, uint32_t(index));
}

uint32_t galleryIndexOf(const Gallery& gallery, PrStringView path) {
    for (uint32_t i = 0; i < gallery.entries.count(); ++i) {
        if (gallery.entries[i].path == path)
            return i;
    }
    return gallery.entries.count();
}

Array<String> gallerySelectionPaths(const Gallery& gallery) {
    Array<String> paths;
    for (uint32_t i = 0; i < gallery.entries.count() && i < gallery.selected.count(); ++i) {
        if (gallery.selected[i])
            paths.push(gallery.entries[i].path);
    }
    if (paths.isEmpty() && gallerySelected(gallery) != nullptr)
        paths.push(gallerySelected(gallery)->path);
    return paths;
}

uint32_t galleryDelete(Gallery& gallery, const Array<String>& paths) {
    uint32_t removed = 0;
    uint32_t last = gallery.lead;
    for (const String& path : paths) {
        uint32_t index = galleryIndexOf(gallery, path);
        if (index == gallery.entries.count() || !prFileRemove(path))
            continue;
        gallery.entries.erase(index);
        gallery.selected.erase(index);
        last = index;
        ++removed;
    }
    // The neighbor that took the last removed entry's place, or the entry before it at the end of the list.
    uint32_t count = gallery.entries.count();
    if (removed != 0)
        gallerySelect(gallery, count == 0 ? 0 : min(last, count - 1));
    return removed;
}

} // namespace alchemy
