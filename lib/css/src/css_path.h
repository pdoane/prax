// Path data in CSS values: the commands of a path() (css-shapes-1/#funcdef-basic-shape-path).
//
// css-shapes-1 - https://drafts.csswg.org/css-shapes-1/
// SVG2 - https://svgwg.org/svg2-draft/paths.html
//
// A path() holds its path data as values, parsed once when the value is matched: each command's letter as a literal,
// followed by its arguments as numbers, after the fill rule and its comma if the fill rule is given. Two paths of the
// same commands then interpolate and add argument by argument, as any values of the same shape do, and paths of other
// commands do not (SVG2/paths.html#PathDataAnimation).

#pragma once

#include "pr/array.h"
#include "pr/path_data.h"
#include "pr/text_buffer.h"

#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Adds the values of `commands`: each command's letter as a literal, followed by its arguments as numbers.
void cssValuePoolAddPathData(CssValuePool* pool, const PrPathDataCommand* PR_NULLABLE commands, uint32_t count);

// Returns the index in `run` of the first value of the path data of the path() at `index`, after its fill rule.
uint32_t cssPathDataStart(CssValueRun run, uint32_t index);

// Appends to `out` the commands of the path() at `index` of `run`.
void cssPathCommands(CssValueRun run, uint32_t index, Array<PrPathDataCommand>* out);

// Adds the path() at `index` of `run` with its commands in absolute coordinates.
void cssValuePoolAddAbsolutePath(CssValuePool* pool, CssValueRun run, uint32_t index);

// Appends the path() at `index` of `run` as CSS text: its fill rule if it has one, and its path data as a string, as
// prPathDataWrite writes it.
void cssSerializePath(PrTextBuffer* out, CssValueRun run, uint32_t index);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
