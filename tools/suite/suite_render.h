// The render settings of a suite, which its harness and the viewer render its documents with.

#pragma once

#include "pr/color.h"
#include "pr/font_set.h"
#include "pr/string_ops.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace suite {

// The settings from a suite's render.txt.
struct SuiteRender {
    PrFontSet* PR_NULLABLE fonts;   // the fonts that the suite's text is set in, owned
    PrColor background;             // what a Lottie animation is drawn over, transparent unless the settings name it
};

// Reads the settings file at `path` into `out`, with font paths relative to `root`. `out` is zeroed or was read before.
// Its fonts start as a new, empty set. Returns false if a line is not a setting or the file cannot be read. The file is
// suites/<suite>/render.txt, with one setting per line, and `#` starts a comment:
//
//   font <path>                 a font file that text is set in, relative to `root`
//   system                      the system's fonts, which text is also set in (prFontSystemInstall)
//   alias <name> = <family>     a family name, or a generic family's keyword, that matches another family's faces
//                               (prFontSetAlias, prFontSetSetGeneric)
//   default <family>            the family that text falls back to after the families it names
//                               (prFontSetSetDefaultFamily)
//   background white            a Lottie animation drawn over white, as a player page shows it
bool suiteRenderRead(SuiteRender* out, PrStringView path, PrStringView root);

// Frees the font set, and zeroes the settings.
void suiteRenderReset(SuiteRender* render);

} // namespace suite

PR_ASSUME_NONNULL_END
