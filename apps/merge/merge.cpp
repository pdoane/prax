// Prax Merge's model (merge.h): its command line, and the files of a merge read as lines and merged by lib/text_diff.

#include "apps/merge/merge.h"

#include "pr/file.h"
#include "pr/text_diff.h"
#include "pr/text_diff_merge.h"

namespace merge {

namespace {

// A file's text as lines, without their line breaks, as a merge reads them.
struct FileLines {
    PrByteView mapping{};
    Array<PrStringView> lines;
    bool crlf = false;              // the first line ends with CR LF

    ~FileLines() { prFileUnmap(&mapping); }

    PrTextDiffText text() const {
        return PrTextDiffText{
            .lineCount = uint32_t(lines.count()),
            .line = [](void* context, uint32_t index) { return static_cast<const FileLines*>(context)->lines[index]; },
            .context = const_cast<FileLines*>(this),
        };
    }
};

// Reads the file at `path` into `out`, its lines split at line feeds, each without a CR before its feed. A file that
// ends with a line break has an empty last line, as an editor's buffer does. Returns false if it cannot be read.
bool readLines(PrStringView path, FileLines* out) {
    if (!prFileMap(path, &out->mapping))
        return false;
    PrStringView all{reinterpret_cast<const char*>(out->mapping.data), out->mapping.size};
    size_t start = 0;
    for (size_t i = 0; i <= all.size; ++i) {
        if (i != all.size && all.data[i] != '\n')
            continue;
        size_t end = i;
        if (i != all.size && end > start && all.data[end - 1] == '\r') {
            out->crlf = out->crlf || out->lines.isEmpty();
            --end;
        }
        out->lines.push(PrStringView{all.data + start, end - start});
        start = i + 1;
    }
    return true;
}

// Returns the name of the file at `path`: what follows its last separator.
PrStringView fileName(PrStringView path) {
    size_t at = path.size;
    while (at > 0 && path.data[at - 1] != '/' && path.data[at - 1] != '\\')
        --at;
    return PrStringView{path.data + at, path.size - at};
}

} // namespace

bool parseArguments(Span<const PrStringView> arguments, Invocation* out) {
    Array<PrStringView> paths;
    for (PrStringView argument : arguments) {
        if (argument == "--trace")
            out->trace = true;
        else if (argument.size != 0 && argument.data[0] == '-')
            return false;
        else
            paths.push(argument);
    }
    if (paths.count() == 2) {
        out->task = Task_Compare;
        out->old = paths[0];
        out->young = paths[1];
        return true;
    }
    if (paths.count() == 4) {
        out->task = Task_Merge;
        out->base = paths[0];
        out->left = paths[1];
        out->right = paths[2];
        out->result = paths[3];
        return true;
    }
    return false;
}

bool mergeFiles(PrStringView base, PrStringView left, PrStringView right, MergedText* out) {
    FileLines files[3];
    if (!readLines(base, &files[0]) || !readLines(left, &files[1]) || !readLines(right, &files[2]))
        return false;
    PrTextDiffText texts[] = {files[0].text(), files[1].text(), files[2].text()};
    PrTextDiffMerge* merged = prTextDiffMergeCreate(nullptr);
    prTextDiffMergeCompare(merged, &texts[0], &texts[1], &texts[2], nullptr, 0);
    PrTextDiffMarkerOptions options = prTextDiffMarkerOptionsDefault();
    options.leftLabel = fileName(left);
    options.baseLabel = fileName(base);
    options.rightLabel = fileName(right);
    PrRawString text = prTextDiffMergeText(merged, &texts[0], &texts[1], &texts[2], &options);
    out->conflicts = prTextDiffMergeConflictCount(merged);
    prTextDiffMergeDestroy(merged);
    PrStringView lines = prRawStringView(&text);
    out->text.clear();
    for (size_t i = 0; i < lines.size; ++i) {
        if (lines.data[i] == '\n' && files[1].crlf)
            out->text.append('\r');
        out->text.append(lines.data[i]);
    }
    prRawStringReset(&text);
    return true;
}

int64_t fileConflicts(PrStringView path) {
    FileLines file;
    if (!readLines(path, &file))
        return -1;
    PrTextDiffText text = file.text();
    return prTextDiffConflicts(&text, nullptr, 0);
}

} // namespace merge
