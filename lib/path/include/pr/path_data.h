// Path data: paths written in the SVG path syntax (SVG2/paths.html#PathData), which SVG documents, fonts and icon sets
// use.

#pragma once

#include "pr/base.h"
#include "pr/path.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Appends the path that `data` describes. The first command of the data must be a moveto. Data in error is appended up
// to the first error (SVG2/paths.html#PathDataErrorHandling), and the call returns Syntax.
PrStatus prPathAppendData(PrPath* path, PrStringView data);

// The maximum number of arguments of a command, which an arc has.
enum { kPrPathDataCommandMaxArguments = 7 };

// A command of path data, with its arguments as written. The letter is uppercase for absolute coordinates and lowercase
// for relative ones. A command that the data leaves implicit is explicit here: each pair of coordinates after the first
// pair of a moveto is a lineto, and each further set of arguments after a command is another command of its kind.
typedef struct PrPathDataCommand {
    char letter;
    float arguments[kPrPathDataCommandMaxArguments]; // in the order of the syntax, with an arc's flags as 0 or 1
} PrPathDataCommand;

// Returns the number of arguments of the command `letter`, which must be a command's letter.
uint32_t prPathDataArgumentCount(char letter);

// Parses `data` into the commands before its first error. Writes up to `capacity` of them to `out` and returns their
// number.
uint32_t prPathDataParse(PrStringView data, PrPathDataCommand* PR_NULLABLE out, uint32_t capacity);

// Appends the path that `commands` describe. The first command must be a moveto. An arc's flag is set if it is above
// zero.
void prPathAppendCommands(PrPath* path, const PrPathDataCommand* PR_NULLABLE commands, uint32_t count);

// The point where a command of path data leaves the current point, and the directions of the path through the command's
// segment (SVG2/paths.html#PathDirectionality).
typedef struct PrPathDataVertex {
    PrVec2 point;               // the end of the segment: a moveto's point, or the start of the subpath for a closepath
    PrVec2 startDirection;      // the direction at the start of the segment, a unit vector
    PrVec2 endDirection;        // the direction at the end of the segment, a unit vector
} PrPathDataVertex;

// Writes the vertex of each of `count` commands to `out`, which holds `count` vertices. A moveto is a segment of zero
// length. A segment of zero length takes its directions from the closest segments of the path that have length, and
// every direction of a path of no length is the positive x-axis.
void prPathDataVertices(const PrPathDataCommand* PR_NULLABLE commands, uint32_t count,
                        PrPathDataVertex* PR_NULLABLE out);

// Rewrites `commands` in absolute coordinates, so that each letter is uppercase and the commands describe the same
// path.
void prPathDataMakeAbsolute(PrPathDataCommand* PR_NULLABLE commands, uint32_t count);

// Appends `commands` to `out` as path data: each letter followed by its arguments, separated by spaces. A closepath is
// written as Z. Each number is written with the fewest digits that parse back to it, and an arc's flag is written as 1
// if it is above zero.
void prPathDataWrite(const PrPathDataCommand* PR_NULLABLE commands, uint32_t count, PrString* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
