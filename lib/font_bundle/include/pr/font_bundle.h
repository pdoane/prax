// Font bundles: the generic families and the families that text falls back to, bound to fonts that a build fetches
// (lib/font_bundle/font_bundle.cmake), for an application to install into its font set.
//
// The library knows the files of each bundle by name, but not where they are. The application passes the directory that
// holds them, either as pr_font_bundle_fetch creates it or as the application ships it.

#pragma once

#include "pr/base.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The bundles. A bundle decides which fonts the generic families, and the other names that the bundle gives, refer to.
// Every bundle falls back to the same families: Noto for the scripts in modern use, with symbols, math and color emoji,
// and finally Last Resort, which draws the missing glyph of a code point that no face serves.
enum {
    // Maps serif, sans-serif and monospace to Noto Serif, Noto Sans and Noto Sans Mono, and math to New Computer Modern
    // Math.
    PrFontBundle_Noto,

    // Substitutes for PDF's standard 14 fonts. Times, Helvetica and Courier map to Tinos, Arimo and Cousine, which have
    // the same metrics, and those fonts also serve as serif, sans-serif and monospace. Symbol and ZapfDingbats map to
    // LeviSymbol and Levibats.
    PrFontBundle_Standard14,
};
typedef uint8_t PrFontBundle;

// Installs the bundle into `set`.
//
// The call adds the faces of the bundle's files in `directory` to `set`. It names the bundle's fonts by the generic
// families, and by the other names that the bundle gives them as aliases. It makes serif the default family, and sets
// the fallback families in the order that they are tried (prFontSetSetFallbackFamilies). Each fallback family carries
// the language that it is made for, if its faces do not say. A file that is not in the directory is left out.
//
// Returns the number of files added, out of prFontBundleFileCount(bundle).
uint32_t prFontBundleInstall(PrFontSet* set, PrFontBundle bundle, PrStringView directory);

// Returns the number of files that the bundle installs.
uint32_t prFontBundleFileCount(PrFontBundle bundle);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
