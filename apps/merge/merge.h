// Prax Merge's model: what its command line asks for, as Git's difftool and mergetool and Perforce's P4DIFF and P4MERGE
// invoke it, and the text that a merge of three files starts its result with.

#pragma once

#include "pr/array.h"
#include "pr/span.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace merge {

using namespace pr;

// What Prax Merge does: compare two files, or merge two files that each changed a base into a result.
enum {
    Task_Compare,
    Task_Merge,
};
typedef uint8_t Task;

// What the command line asks for.
struct Invocation {
    Task task = Task_Compare;
    bool trace = false;

    // A comparison's old and new files, or a merge's base, sides and result.
    String old;
    String young;
    String base;
    String left;
    String right;
    String result;
};

// The usage of the command line.
constexpr const char* kUsage = "usage: merge [--trace] old new\n"
    "       merge [--trace] base left right result\n";

// Reads `arguments`, those after the program's name. Returns false if they are not a comparison's or a merge's.
bool parseArguments(Span<const PrStringView> arguments, Invocation* out);

// The text that a merge's result starts with, and its conflicts.
struct MergedText {
    String text;
    uint32_t conflicts = 0;
};

// Merges the files at `left` and `right`, which each changed the file at `base`: the lines that agree, and each
// conflict between diff3 markers labeled with the files' names. The text ends its lines as the left file does, with a
// line feed or CR LF. Returns false if a file cannot be read.
bool mergeFiles(PrStringView base, PrStringView left, PrStringView right, MergedText* out);

// Returns the number of conflicts that the file at `path` marks, or -1 if it cannot be read.
int64_t fileConflicts(PrStringView path);

} // namespace merge

PR_ASSUME_NONNULL_END
