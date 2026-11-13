// The gallery: a directory of PNG files, newest first, which is the only store of alchemy's images.
//
// An image's file is the whole entry: its pixels, and the record of how it was made (provenance.h). A generated image
// is written before it joins the list. The browser's selection is held by filename, so that an image written at the
// head of the list does not move it.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/kit_collection.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/time.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

using namespace pr;

// An image of the gallery.
struct GalleryEntry {
    String path;
    String filename;
    PrTimeSystem modified{};
};

// The images of a directory, newest first, and the browser's selection of them. `selected` holds a flag for each entry,
// and `lead` is the entry that the viewport shows.
struct Gallery {
    String dir;
    Array<GalleryEntry> entries;
    Array<bool> selected;
    PrKitSelection selection{};
    uint32_t lead = 0;

    // The count of rescans, so that what was read from a file under the previous listing is read again.
    uint32_t revision = 0;
};

// Returns the directory of the gallery: $ALCHEMY_DIR if it is set, and otherwise Alchemy/images under the platform's
// directory for application data.
String galleryDirectory();

// Points the gallery at `dir`, creating it if it is missing, and lists its images.
void galleryOpen(Gallery& gallery, PrStringView dir);

// Lists the directory again, keeping the selection on the images that remain.
void galleryRescan(Gallery& gallery);

// Writes `png` into the gallery's directory under a name made from `filename`, and selects it at the head of the list.
// Only the basename of `filename` is used, it ends in .png, and a number is added to it if a file has the name already.
// Writes the path to `savedPath`. Returns false if the file could not be written.
bool gallerySave(Gallery& gallery, PrStringView filename, PrByteView png, String* savedPath);

// Returns the entry that the viewport shows, or null if the gallery is empty.
const GalleryEntry* PR_NULLABLE gallerySelected(const Gallery& gallery);

// Selects the entry at `index` alone, or moves the selection `delta` entries along the list.
void gallerySelect(Gallery& gallery, uint32_t index);
void galleryStep(Gallery& gallery, int32_t delta);

// Returns the index of the entry whose file is `path`, or the number of entries if none is.
uint32_t galleryIndexOf(const Gallery& gallery, PrStringView path);

// Returns the paths of the selected entries, in the list's order, or of the lead alone if none is selected.
Array<String> gallerySelectionPaths(const Gallery& gallery);

// Removes the files of `paths` and their entries, and leaves the selection on the neighbor of the last that was
// removed. Returns the number of files removed.
uint32_t galleryDelete(Gallery& gallery, const Array<String>& paths);

} // namespace alchemy

PR_ASSUME_NONNULL_END
