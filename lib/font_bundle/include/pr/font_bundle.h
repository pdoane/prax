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

// The bundles. Each bundle gives the fonts that the generic families and its other names refer to, and the families
// that text falls back to. A bundle uses only its own files.
enum {
    // Covers every script in modern use, with the same fonts on every platform. Maps serif to Noto Serif, sans-serif
    // and system-ui to Noto Sans, monospace to Noto Sans Mono, and math to New Computer Modern Math. Falls back to Noto
    // for the scripts in modern use, with symbols, math and color emoji, and finally to Last Resort, which draws the
    // missing glyph of a code point that no face serves.
    PrFontBundle_Complete,

    // Maps monospace to Noto Sans Mono, and has no fallback families. It is for an application that takes the rest of
    // its fonts from the system (prFontSetSetSystem).
    PrFontBundle_NotoMono,

    // Substitutes for PDF's standard 14 fonts. Times, Helvetica and Courier map to Tinos, Arimo and Cousine, which have
    // the same metrics, and those fonts also serve as serif, sans-serif and monospace. Symbol and ZapfDingbats map to
    // LeviSymbol and Levibats, and math to New Computer Modern Math. Falls back to its own text faces and math face.
    PrFontBundle_Standard14,

    // Maps serif to IBM Plex Serif, sans-serif and system-ui to IBM Plex Sans, monospace to IBM Plex Mono, and math to
    // IBM Plex Math. Falls back to IBM Plex's families for Arabic, Hebrew, Devanagari, Thai, Chinese, Japanese and
    // Korean.
    PrFontBundle_Plex,

    // Symbols Nerd Font Mono: the Nerd Fonts icons that shell prompts and file listings use, at the private-use code
    // points that Nerd Fonts assigns. Each icon is as wide as it is high. The bundle names no family. Its only fallback
    // family serves private-use code points, so it goes after a bundle of text faces.
    PrFontBundle_NerdFontSymbols,
};
typedef uint8_t PrFontBundle;

// Installs the stacks of `bundles`, in order, into `set`.
//
// The call adds the faces of the bundles' files in `directory` to `set`, each file once. It binds the generic families
// to the bundles' fonts, and adds the bundles' other names as aliases. A name refers to the fonts of the first bundle
// that gives it. The call makes serif the default family. It sets the fallback families in the order that they are
// tried (prFontSetSetFallbackFamilies): the families of each bundle in turn, so that a later bundle serves what the
// earlier ones lack. Each fallback family carries the language that it is made for, if its faces do not say. A file
// that is not in the directory is left out.
//
// Returns the number of files added, out of prFontBundleFileCount(bundles, count).
uint32_t prFontBundleInstall(PrFontSet* set, const PrFontBundle* bundles, uint32_t count, PrStringView directory);

// Returns the number of files that the bundles install together.
uint32_t prFontBundleFileCount(const PrFontBundle* bundles, uint32_t count);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
