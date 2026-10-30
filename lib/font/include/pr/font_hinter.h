// Font hinters: the backends that fit glyph outlines to the pixel grid of the size at which they are drawn.
//
// A font reads its design outlines itself, and hints them through the hinter that its derivation names
// (PrFontDerivation::hinter). A font without a hinter draws its design outlines at every size, whatever its hinting.

#pragma once

#include "pr/base.h"
#include "pr/font.h"
#include "pr/path.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The function table of a hinter, placed at the start of the backend's state. Each member implements the public
// function of the same name. A null `destroy` means there is nothing to free, and a null `forget` that the hinter keeps
// nothing for a font. Callers use the functions below, never these members.
//
// A hinter is called from any thread, so it locks whatever it shares between calls. It reads the fonts that it hints
// through their public functions: their bytes, index and axis values.
struct PrFontHinter {
    void (*PR_NULLABLE destroy)(PrFontHinter* hinter);
    bool (*outline)(PrFontHinter* hinter, const PrFont* font, uint16_t glyph, float pixelsPerEm, PrFontHinting hinting,
        float embolden, PrPath* out);
    bool (*advance)(PrFontHinter* hinter, const PrFont* font, uint16_t glyph, float pixelsPerEm, PrFontHinting hinting,
        float* out);
    void (*PR_NULLABLE forget)(PrFontHinter* hinter, uint64_t fontId);
};

// The library's backends of hinters, which the build turns on (README.md), and Default, the first that the build has.
enum {
    PrFontHinterBackend_Default,
    PrFontHinterBackend_FreeType,
    PrFontHinterBackend_Count,
};
typedef uint8_t PrFontHinterBackend;

// Returns the backend that `name` names, as prFontHinterBackendName spells it, ignoring case, or Count if it names
// none.
PrFontHinterBackend prFontHinterBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prFontHinterBackendName(PrFontHinterBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prFontHinterBackendIsBuilt(PrFontHinterBackend backend);

// Creates a hinter by the backend. A backend's own header documents how it hints. Returns null if the build does not
// have the backend.
PrFontHinter* PR_NULLABLE prFontHinterCreate(PrFontHinterBackend backend);

// Frees the hinter. No font that names it may remain.
void prFontHinterDestroy(PrFontHinter* PR_NULLABLE hinter);

// Appends to `out` the outline of the glyph of the font's design at its axis values, fitted by `hinting` to a size of
// `pixelsPerEm` pixels per em of the design, in ems of the design with y down. `embolden` grows the outline outward by
// that many ems on each side, after it is fitted. Returns false if the hinter fits no outline for the glyph, and
// appends nothing; the font then draws its design outline. `hinting` is not None, and `pixelsPerEm` is above zero.
bool prFontHinterOutline(PrFontHinter* hinter, const PrFont* font, uint16_t glyph, float pixelsPerEm,
    PrFontHinting hinting, float embolden, PrPath* out);

// Gets the advance of the glyph fitted by `hinting` to `pixelsPerEm` pixels per em of the design, in pixels. Returns
// false if the hinter fits no advance for the glyph.
bool prFontHinterAdvance(PrFontHinter* hinter, const PrFont* font, uint16_t glyph, float pixelsPerEm,
    PrFontHinting hinting, float* out);

// Drops what the hinter keeps for the font of `fontId` (prFontId), which is being freed.
void prFontHinterForget(PrFontHinter* hinter, uint64_t fontId);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
