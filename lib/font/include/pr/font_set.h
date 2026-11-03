// Font sets: faces described by a document's font rules or by the system, matched against a requested family and style.
//
// A set may have a parent, whose faces it matches after its own, as the set of a document is layered over the set of
// its host. Any thread may match in a set, add to it, and use the fonts that it returns. A set and its parents lock
// while they do. A parent must outlive the sets over it.

#pragma once

#include "pr/base.h"
#include "pr/font.h"
#include "pr/locale.h"
#include "pr/raw_string.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A collection of described faces that requests are matched against.
typedef struct PrFontSet PrFontSet;

// A generic font family: a kind of type that stands for families that the host installs, possibly a different family
// for each script or language. A generic is never matched as the name of a family.
enum {
    PrFontGeneric_None,         // a family named by its name
    PrFontGeneric_Serif,
    PrFontGeneric_SansSerif,
    PrFontGeneric_Monospace,
    PrFontGeneric_Cursive,
    PrFontGeneric_Fantasy,
    PrFontGeneric_SystemUi,
    PrFontGeneric_Math,
    PrFontGeneric_Emoji,
    PrFontGeneric_UiSerif,
    PrFontGeneric_UiSansSerif,
    PrFontGeneric_UiMonospace,
    PrFontGeneric_UiRounded,
    PrFontGeneric_Fangsong,     // generic(fangsong)
    PrFontGeneric_Kai,          // generic(kai)
    PrFontGeneric_KhmerMul,     // generic(khmer-mul)
    PrFontGeneric_Nastaliq,     // generic(nastaliq)
    PrFontGeneric_Count,
};
typedef uint8_t PrFontGeneric;

// Returns the generic family that a CSS keyword names, matched ASCII case-insensitively, such as `sans-serif` or
// `fangsong` (whose keyword is written generic(fangsong) in a family list), or None for any other name. Also returns
// the keyword of a generic.
PrFontGeneric prFontGenericFromName(PrStringView keyword);
PrStringView prFontGenericName(PrFontGeneric generic);

// An entry of a family list, as font-family gives it: a family by its name, or a generic family.
typedef struct PrFontFamily {
    PrStringView name;          // empty for a generic
    PrFontGeneric generic;
} PrFontFamily;

// Returns the family named `name`, or the generic family `generic`.
static inline PrFontFamily prFontFamilyFromName(PrStringView name) {
    PrFontFamily family;
    family.name = name;
    family.generic = PrFontGeneric_None;
    return family;
}
static inline PrFontFamily prFontFamilyGeneric(PrFontGeneric generic) {
    PrFontFamily family;
    family.name.data = NULL;
    family.name.size = 0;
    family.generic = generic;
    return family;
}

// An inclusive range of code points.
typedef struct PrFontCodePointRange {
    uint32_t first;
    uint32_t last;
} PrFontCodePointRange;

// What a face is for, beyond the code points that it serves.
enum {
    PrFontFaceFlag_Emoji = 1u << 0,             // text with emoji presentation
};
typedef uint8_t PrFontFaceFlags;

// A font and the code points that it serves. Those are the code points in `ranges` that the font has glyphs for, or
// every code point that it has a glyph for if `ranges` is empty. A face added without its font (prFontSetAddPending)
// has no font until its font loads, and serves nothing. `pending` is the key that it was added with.
typedef struct PrFontFace {
    const PrFont* PR_NULLABLE font;
    const PrFontCodePointRange* PR_NULLABLE ranges;
    uint32_t rangeCount;
    PrFontFaceFlags flags;
    uint64_t pending;
} PrFontFace;

// Returns true if the face serves `codePoint`: it has its font, the code point is in its ranges, and the font has a
// glyph.
bool prFontFaceServes(const PrFontFace* face, uint32_t codePoint);

// Returns true if `codePoint` is in the ranges of the face, whether or not its font has a glyph for it.
bool prFontFaceInRanges(const PrFontFace* face, uint32_t codePoint);

// The descriptors read from the face instead of taken from the PrFontFaceDescriptor.
enum {
    PrFontFaceAutoFlag_Family = 1u << 0,
    PrFontFaceAutoFlag_Weight = 1u << 1,
    PrFontFaceAutoFlag_Width = 1u << 2,
    PrFontFaceAutoFlag_Slope = 1u << 3,

    // PrFontFaceFlag_Emoji, set if the font has color glyphs for the Emoticons block. That block defaults to emoji
    // presentation.
    PrFontFaceAutoFlag_Emoji = 1u << 4,

    // Every descriptor.
    PrFontFaceAutoFlag_All = PrFontFaceAutoFlag_Family | PrFontFaceAutoFlag_Weight | PrFontFaceAutoFlag_Width |
        PrFontFaceAutoFlag_Slope | PrFontFaceAutoFlag_Emoji,
};
typedef uint8_t PrFontFaceAutoFlags;

// The descriptors of a face, as an @font-face rule gives them.
typedef struct PrFontFaceDescriptor {
    PrStringView family;
    float weightMin;
    float weightMax;
    float widthMin;                     // percent
    float widthMax;
    PrFontSlope slope;
    float obliqueMin;                   // Oblique: degrees, clockwise positive
    float obliqueMax;
    const PrFontCodePointRange* PR_NULLABLE ranges;  // the code points that it serves, or empty for all
    uint32_t rangeCount;
    PrFontDerivation derivation;        // applied over the axes that matching sets
    PrFontFaceAutoFlags automatic;      // the descriptors read from the face instead

    // The weight, width and slope whose values match the face but do not limit its variation axes. Only the ranges of
    // the font itself limit those axes. An unclamped slope also lets an oblique request slant the face along its slant
    // axis.
    PrFontFaceAutoFlags unclamped;

    PrFontFaceFlags flags;              // the flags of the faces that prFontSetMatch returns for it
} PrFontFaceDescriptor;

// Returns the descriptor that reads every descriptor from the face, with the default derivation.
PrFontFaceDescriptor prFontFaceDescriptorDefault(void);

// Creates an empty set over `parent`, or with no parent if it is null, or frees a set.
PrFontSet* prFontSetCreate(PrFontSet* PR_NULLABLE parent);
void prFontSetDestroy(PrFontSet* PR_NULLABLE set);

// Returns a number that changes whenever the result of a match in the set or its parents may change: if a face is
// added, its font loads or fails, or a generic, alias, default or fallback family changes. Versions and stamps are
// taken from one counter for the process, so a later change has a greater number.
uint64_t prFontSetVersion(const PrFontSet* set);

// Returns the number of the last change, in the set or a parent, that may change the result of a match of `family`.
// That is a change of the faces of the family that it names or resolves to by a generic binding or an alias, or a
// change of a binding. A family that the set matches nothing of has a stamp too, which its first face moves. Faces
// change the stamp of a family as they are added, removed, loaded, failed or ordered.
uint64_t prFontSetFamilyStamp(const PrFontSet* set, PrFontFamily family);

// Returns the number of the last change of the set that the fallback families are matched in (prFontSetMatchFallback),
// which is the outermost of the set and its parents.
uint64_t prFontSetFallbackStamp(const PrFontSet* set);

// Adds the face of `font`, described by `descriptor`. The set holds a reference to the font and copies the descriptor.
void prFontSetAdd(PrFontSet* set, const PrFontFaceDescriptor* descriptor, const PrFont* font);

// Adds every face of the font file at `path`, described by `descriptor`, whose automatic descriptors read each face,
// and returns how many it added. The file is read for the faces' tables, and a face's font is created when a match
// first needs it. Returns zero if the file cannot be read.
uint32_t prFontSetAddFile(PrFontSet* set, const PrFontFaceDescriptor* descriptor, PrStringView path);

// Adds the faces of `family` in the font file at `path` without reading the file. A match first asking for the family
// reads the file, and its faces whose family is `family`, or every face if none is, are added then, described by the
// default descriptor. A host adds the fonts that its platform lists this way, by the families that the platform knows
// them by, so that a file is read only for text that asks for its family. Until then, the file counts as one face of
// the set. Adding the same family of the same file again adds nothing.
void prFontSetAddFileFamily(PrFontSet* set, PrStringView family, PrStringView path);

// The library's backends of the system's fonts, which the build turns on (README.md), and Default, the first that the
// build has.
enum {
    PrFontSystemBackend_Default,
    PrFontSystemBackend_CoreText,
    PrFontSystemBackend_DirectWrite,
    PrFontSystemBackend_Count,
};
typedef uint8_t PrFontSystemBackend;

// Returns the backend that `name` names, as prFontSystemBackendName spells it, ignoring case, or Count if it names
// none.
PrFontSystemBackend prFontSystemBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prFontSystemBackendName(PrFontSystemBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prFontSystemBackendIsBuilt(PrFontSystemBackend backend);

// Adds the families of the system's fonts that the backend lists to `set`, each by the file that holds it
// (prFontSetAddFileFamily), and returns how many it added. A backend that the build does not have adds none.
uint32_t prFontSystemInstall(PrFontSystemBackend backend, PrFontSet* set);

// Adds a face whose font is not loaded yet, as the font rule of a document does. The face is described by `descriptor`,
// whose automatic descriptors read the defaults, and is known to the caller by `key`. A match keeps it like any other
// face, and gives it with no font. A family whose faces are all pending or failed is missing. Text that needs the face
// requests it (prFontSetRequestLoad), and the caller loads it and gives the set its font, or its failure.
void prFontSetAddPending(PrFontSet* set, const PrFontFaceDescriptor* descriptor, uint64_t key);
void prFontSetLoaded(PrFontSet* set, uint64_t key, const PrFont* font);
void prFontSetFailed(PrFontSet* set, uint64_t key);

// Removes the faces added pending with `key`, and what the set keeps for them. A load of the key that completes later
// finds no face.
void prFontSetRemove(PrFontSet* set, uint64_t key);

// Ranks the faces added pending with `keys` in that order, the first lowest. The faces of a family are checked from the
// highest rank down, so the face of the last key is checked first, as the last @font-face rule of a document is. A face
// ranks by when it was added until it is ordered, and keeps its rank if it is left out. Changes the set only if the
// faces do not already rank in that order.
void prFontSetOrder(PrFontSet* set, const uint64_t* PR_NULLABLE keys, uint32_t count);

// Requests that the pending face `key` of the set or its parents load, because text needs it. The set that holds the
// face keeps the request, once, until it is taken.
void prFontSetRequestLoad(PrFontSet* set, uint64_t key);

// Takes the keys of the loads requested since the last take. Writes up to `capacity` keys to `out`, keeps the rest, and
// returns how many it wrote.
uint32_t prFontSetTakeLoads(PrFontSet* set, uint64_t* PR_NULLABLE out, uint32_t capacity);

// Returns the number of faces in the set.
uint32_t prFontSetFaceCount(const PrFontSet* set);

// Makes a request for the family `name` match the faces of `family`, as a host maps a well-known name to a family that
// it installs. A later alias of the name replaces an earlier one. A family that the set has faces of is matched as
// itself, whatever it is aliased to.
void prFontSetAlias(PrFontSet* set, PrStringView name, PrStringView family);

// Makes the generic family `generic` match the faces of `family`, or nothing if `family` is empty. A set without its
// own binding uses the binding of its parent. A name read stays valid until the generic is set again.
void prFontSetSetGeneric(PrFontSet* set, PrFontGeneric generic, PrStringView family);
PrStringView prFontSetGenericFamily(const PrFontSet* set, PrFontGeneric generic);

// The family that text falls back to after the families that it names, like the default font-family of a user agent. It
// is serif until it is set, or the default family of the parent in a set over one. A name read stays valid until the
// default is set again.
void prFontSetSetDefaultFamily(PrFontSet* set, PrFontFamily family);
PrFontFamily prFontSetDefaultFamily(const PrFontSet* set);

// Makes the faces that the set matches, and that the sets over it match unless they set their own, hinted by `hinter`
// in the mode `hinting` (pr/font_hinter.h). A null hinter, or a hinting of None, draws them as designed, which is how a
// set without a setting of its own or a parent's draws them. A face whose descriptor's derivation sets its hinting
// keeps that. The hinter must outlive the fonts that the set returns. The hinting applies to the matches made from then
// on, so a host sets it before text uses the set.
void prFontSetSetHinting(PrFontSet* set, PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter);

// Returns how the set hints the faces that it matches, and writes its hinter to `hinter`, if `hinter` is not null.
PrFontHinting prFontSetHinting(const PrFontSet* set, PrFontHinter* PR_NULLABLE* PR_NULLABLE hinter);

// A family that text falls back to, the locales that it is made for, and the generic family whose style it is drawn in,
// if any. The locales it is made for are those whose parts match each part that `locale` sets. A locale of only a
// script, such as `und-Hant`, names Traditional Chinese whatever language it writes, and `und-Hant-HK` names the forms
// of Hong Kong, which text of that locale prefers to the first. A `locale` of zero is made for no locale.
typedef struct PrFontFallbackFamily {
    PrStringView family;
    PrLocale locale;
    PrFontGeneric generic;
} PrFontFallbackFamily;

// The families that text falls back to, in the order in which they are tried, if no family that its style names serves
// it. These are the fonts that a host installs. A set over a parent falls back to the fallback families of the parent,
// and matches them among the faces of the parent, never among its own. Matching among its own faces would let the fonts
// of one document serve the text of another (prFontSetMatchFallback). The caller chooses among the families by the
// script and locale of the text, the locales that each family is made for, and the scripts that its faces declare
// (prFontScriptTags). The families read stay valid until they are set again, so a host sets them before text uses the
// set.
void prFontSetSetFallbackFamilies(PrFontSet* set, const PrFontFallbackFamily* PR_NULLABLE families, uint32_t count);
uint32_t prFontSetFallbackCount(const PrFontSet* set);
PrFontFallbackFamily prFontSetFallbackFamily(const PrFontSet* set, uint32_t index);

// Returns the families of the set's own faces, each listed once, in the order in which they were first added. The list
// only grows, and each name stays valid for the life of the set.
uint32_t prFontSetFamilyCount(const PrFontSet* set);
PrStringView prFontSetFamily(const PrFontSet* set, uint32_t index);

// What a match may do beyond choosing faces and setting their axes, as the CSS properties font-optical-sizing and
// font-synthesis allow.
enum {
    PrFontStyleFlag_OpticalSizing = 1u << 0,        // sets an opsz axis to the size of the style
    PrFontStyleFlag_SynthesizeWeight = 1u << 1,     // synthesizes bold
    PrFontStyleFlag_SynthesizeStyle = 1u << 2,      // synthesizes oblique
};
typedef uint8_t PrFontStyleFlags;

// The heaviest weight that matching treats as normal. A style from kPrFontWeightNormal to this weight tries faces from
// its weight up to this one first, then lighter faces, then heavier ones.
static const float kPrFontSetWeightNormalMax = 500;

// The angle in degrees of an oblique that stands in for a requested italic, and the default angle of an oblique style.
static const float kPrFontSetObliqueAngle = 14;

// A style requested of the faces of a family, as the CSS font properties give it.
typedef struct PrFontStyle {
    float weight;
    float width;                        // percent
    PrFontSlope slope;
    float obliqueAngle;                 // Oblique: degrees, clockwise positive
    float size;                         // CSS px, for optical sizing
    PrFontStyleFlags flags;
} PrFontStyle;

// Returns a style with weight kPrFontWeightNormal, width kPrFontWidthNormal, a normal slope, an oblique angle of
// kPrFontSetObliqueAngle, a size of 16, and every flag.
PrFontStyle prFontStyleDefault(void);

// The faces that a match gives, each holding a reference to its font that the match owns. One thread uses a match at a
// time, and a match is reused across matches.
typedef struct PrFontMatch PrFontMatch;

// Creates an empty match, or frees one and the references it holds.
PrFontMatch* prFontMatchCreate(void);
void prFontMatchDestroy(PrFontMatch* PR_NULLABLE match);

// Releases the faces that the match holds and empties it, keeping its storage.
void prFontMatchClear(PrFontMatch* match);

// Returns the faces, in the order in which matching gives them. A face, including its ranges, stays valid until the
// match changes, whatever the set does meanwhile.
uint32_t prFontMatchFaceCount(const PrFontMatch* match);
PrFontFace prFontMatchFace(const PrFontMatch* match, uint32_t index);

// Appends to `out` the faces of `family` that best match `style`, in the order in which a code point tries their
// ranges. Appends none if the family has no face, or no face that the set can read.
//
// A generic family matches the faces of the family bound to it (prFontSetSetGeneric). The faces of a family are the
// set's own faces if it has any, including pending or failed ones, and otherwise those of its parent. A family that a
// document defines therefore hides the family of that name in the host, even if none of its faces loads.
//
// Matching narrows the faces of the family by width, then slope, then weight. Each step keeps the faces nearest the
// style. A value within the range of a face is nearest. If the value is outside every range, the search first goes away
// from the normal value, and then toward it: narrower below kPrFontWidthNormal, lighter below kPrFontWeightNormal,
// heavier above kPrFontSetWeightNormalMax. Several faces remain if faces of one style divide the code points between
// them. The face of the highest rank comes first (prFontSetOrder). A failed face takes part in narrowing like any other
// face, and is then left out. Its failure leaves no face of its style, instead of moving the match to another style.
//
// Each face is an instance. Its variation axes are set to the style if they vary a property that the style asks for,
// and the derivation of its descriptor is applied over them. Its font synthesizes the bold and oblique that the face
// cannot provide (prFontEmbolden, prFontOblique). Equal instances of faces with the same font are the same font
// (prFontCreateDerived). A pending face is given with no font, and a failed one is not given at all. The ranges and
// flags of a face are those of its descriptor, copied into the match.
void prFontSetMatch(PrFontSet* set, PrFontFamily family, const PrFontStyle* style, PrFontMatch* out);

// Appends to `out` the faces of the fallback family `index` that best match `style`, among the faces of the outermost
// of the set and its parents. Matching works the same way as in prFontSetMatch.
void prFontSetMatchFallback(PrFontSet* set, uint32_t index, const PrFontStyle* style, PrFontMatch* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
