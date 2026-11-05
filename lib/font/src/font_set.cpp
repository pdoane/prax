// Font sets
//
// A face added from a file is described from the tables of the file when it is added. The file is opened as a font when
// a match first keeps the face. Every instance that a match makes is kept for the life of the set, keyed by its face
// and the axis values that it sets. Matching the same style again therefore returns the same font.
//
// Each set has a lock, which guards its faces, instances and requests. A match holds the lock of a set and then the
// lock of its parent, never the other way around. The stamp of a set comes from one counter for the process, taken at
// each change. The version of a set is the latest of its own stamp and the stamps of its parents.

#include "pr/font_set.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/atomic.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/string_array.h"

#include "font/src/sfnt.h"

#if defined(PR_FONT_HAS_CORETEXT)
#include "pr/font_coretext.h"
#endif
#if defined(PR_FONT_HAS_DIRECTWRITE)
#include "pr/font_directwrite.h"
#endif

namespace pr_impl {

namespace {

constexpr uint32_t kWght = PR_FONT_TAG('w', 'g', 'h', 't');
constexpr uint32_t kWdth = PR_FONT_TAG('w', 'd', 't', 'h');
constexpr uint32_t kSlnt = PR_FONT_TAG('s', 'l', 'n', 't');
constexpr uint32_t kItal = PR_FONT_TAG('i', 't', 'a', 'l');
constexpr uint32_t kOpsz = PR_FONT_TAG('o', 'p', 's', 'z');

// The angle below which an oblique style searches smaller angles first.
constexpr float kObliqueThreshold = 11;

// Synthetic bold applies to a style of at least this weight for a face lighter than it. It grows the outline by a
// forty-eighth of an em.
constexpr float kBoldWeight = 600;
constexpr float kEmbolden = 1.0f / 48;

// The axes that matching sets, in the order in which the key of an instance stores them.
constexpr uint32_t kAutoAxes[] = {kWght, kWdth, kSlnt, kItal, kOpsz};
constexpr uint32_t kAutoAxisCount = uint32_t(countOf(kAutoAxes));

// Whether a face can be matched: its font is at hand or can be opened, its font is still loading, or its font failed to
// load. An unread face stands for the faces of its family in a file that is read when a match first needs the family.
enum { FaceState_Ready, FaceState_Pending, FaceState_Failed, FaceState_Unread };
typedef uint8_t FaceState;

struct Face {
    PrRawString family;
    float weightMin, weightMax;
    float widthMin, widthMax;
    PrFontSlope slope;
    float obliqueMin, obliqueMax;
    PrFontInfo info;                            // the info of the face itself, for its axes
    RawArray<PrFontCodePointRange> ranges;
    RawArray<PrFontVariation> variations;
    RawArray<PrFontFeature> features;
    PrFontDerivation derivation;                // the derivation's arrays are those above
    PrFontFaceFlags flags;

    // The automatic flags of the descriptor, for the axes that the ranges of the face do not limit.
    PrFontFaceAutoFlags unclamped;

    PrRawString path;                              // the path of a file face; null for a face added as a font
    uint32_t index;
    const PrFont* PR_NULLABLE font;             // null until a file face is first opened, or a pending face loads
    bool unreadable;                            // true for a file face that failed to open
    uint64_t key;                               // the load key of a pending face, which names its load to the host
    uint64_t id;                                // the id of the face, which its instances name
    uint64_t rank;                              // the order among the faces of its family; the highest is first
    FaceState state;
    bool autoEmoji;                             // true if the font of a pending face, once loaded, decides Emoji
};

// The values that an instance sets the automatic axes to, which axes it sets, and what it synthesizes. Keys are
// compared by their bytes. An axis not in the mask has the value zero.
PR_NO_PADDING_BEGIN
struct AxisKey {
    uint32_t mask;                              // one bit for each axis of kAutoAxes
    float values[kAutoAxisCount];
    float embolden;
    float oblique;
};
PR_NO_PADDING_END

// A font that a match made from a face, which the set keeps while the face is in it, so that matching the style again
// opens nothing. Equal fonts of other faces are the same font (prFontCreateDerived).
struct Instance {
    uint64_t face;                              // the id of the face
    AxisKey axes;
    PrFontHinting hinting;                      // the hinting of the set that the match was asked of, and its hinter
    PrFontHinter* PR_NULLABLE hinter;
    const PrFont* font;
};

// How a set hints the faces that it matches (prFontSetSetHinting).
struct SetHinting {
    PrFontHinting hinting;
    PrFontHinter* PR_NULLABLE hinter;
};

// The stamp of the last change to the faces of a family, by its name, folded as matching folds names.
struct FamilyStamp {
    PrRawString name;
    uint64_t stamp;
};

// A name that matches the faces of another family.
struct Alias {
    PrRawString name;
    PrRawString family;
};

// A face that a match gives. Its ranges start at `rangeStart` in the ranges of the match.
struct MatchedFace {
    PrFontFace face;                            // with null ranges; the match gives them
    uint32_t rangeStart;
};

// The faces of matches, each holding a reference to its font, and copies of their ranges.
struct FontMatch {
    RawArray<MatchedFace> faces;
    RawArray<PrFontCodePointRange> ranges;
};

struct FontSet {
    FontSet* PR_NULLABLE parent;
    mutable PrMutex lock;           // taken by what reads the set too, so a const set is locked as well
    Atomic<uint64_t> stamp;         // the stamp of the last change of the set
    RawArray<Face> faces;
    RawArray<PrRawString> families;
    RawArray<Instance> instances;
    RawArray<Alias> aliases;
    PrRawString generics[PrFontGeneric_Count]; // the family each generic family matches, or null for the parent's
    PrRawString defaultName;                   // the default family if it is a name, with defaultSet
    PrFontGeneric defaultGeneric;              // the default family if it is a generic
    bool defaultSet;
    RawArray<PrRawString> fallback;           // the families that text falls back to, in order
    RawArray<PrLocale> fallbackLocales;       // for each fallback family
    RawArray<PrFontGeneric> fallbackGenerics; // for each fallback family
    RawArray<uint64_t> requests;              // the pending faces requested since the last take
    uint64_t nextRank;                        // the rank of the next face added, or the next face ordered
    uint64_t nextFaceId;                      // the id of the next face added
    RawArray<FamilyStamp> familyStamps;
    uint64_t bindingStamp;          // the last change to an alias, a generic binding, the default, the fallback or the
                                    // hinting

    // How the set hints the faces that it matches, if `hintingSet`, and otherwise as its parent does.
    SetHinting hinting;
    bool hintingSet;

    // The number of requests, which a take reads without the lock to return at once if there are none.
    Atomic<uint32_t> requestCount;
};

// The counter that the stamp of every set is taken from.
Atomic<uint64_t> gFontSetStamp;

// Records a change of the set, which its lock guards.
void changed(FontSet* set) {
    set->stamp.store(gFontSetStamp.fetchAdd(1) + 1);
}

// Records a change of the faces of `family`, along with the change of the set.
void familyChanged(FontSet* set, PrStringView family) {
    changed(set);
    uint64_t stamp = set->stamp.load();
    for (uint32_t i = 0; i < set->familyStamps.count; ++i) {
        if (prAsciiEqualsFold(prRawStringView(&set->familyStamps.data[i].name), family)) {
            set->familyStamps.data[i].stamp = stamp;
            return;
        }
    }
    set->familyStamps.push(FamilyStamp{prRawStringCopy(family), stamp});
}

// Records a change of what a family resolves to: an alias, a generic binding, the default or the fallback families.
void bindingChanged(FontSet* set) {
    changed(set);
    set->bindingStamp = set->stamp.load();
}

// Returns the stamp of the last change of the set alone that may change a match of `family` in it: a change of its
// faces of the family, or of a binding. Called with the lock of the set held.
uint64_t ownFamilyStamp(const FontSet* set, PrStringView family) {
    uint64_t stamp = set->bindingStamp;
    for (uint32_t i = 0; i < set->familyStamps.count; ++i) {
        const FamilyStamp& at = set->familyStamps.data[i];
        if (at.stamp > stamp && prAsciiEqualsFold(prRawStringView(&at.name), family))
            stamp = at.stamp;
    }
    return stamp;
}

// Releases what the face holds, as it leaves the set.
void faceReset(Face& face) {
    prRawStringReset(&face.family);
    prRawStringReset(&face.path);
    face.ranges.reset();
    face.variations.reset();
    face.features.reset();
    prFontRelease(face.font);
}

void setAxis(AxisKey& key, uint32_t axis, float value) {
    key.mask |= 1u << axis;
    key.values[axis] = value;
}

// The Emoticons block, whose code points all default to emoji presentation.
constexpr uint32_t kEmoticonsFirst = 0x1f600;
constexpr uint32_t kEmoticonsLast = 0x1f64f;

// Returns true if the font draws emoji: the first emoticon that it has a glyph for is a color glyph, of layers or a
// bitmap.
bool drawsEmoji(const PrFont* font) {
    for (uint32_t codePoint = kEmoticonsFirst; codePoint <= kEmoticonsLast; ++codePoint) {
        uint16_t glyph = prFontCodePointGlyph(font, codePoint);
        PrFontGlyphImage image;
        if (glyph != 0)
            return prFontGlyphHasColor(font, glyph) || prFontGlyphImage(font, glyph, 64, &image);
    }
    return false;
}

// Returns true if the face has a table of color glyphs. Every font for which drawsEmoji returns true has one.
bool hasColorGlyphs(const Sfnt& sfnt) {
    const uint32_t tags[] = {
        PR_FONT_TAG('C', 'O', 'L', 'R'), PR_FONT_TAG('s', 'b', 'i', 'x'),
        PR_FONT_TAG('C', 'B', 'D', 'T')
    };
    for (uint32_t tag : tags) {
        if (sfntTable(sfnt, tag).size != 0)
            return true;
    }
    return false;
}

// Records that the face at `index` was added: its family is one of the set's, and the family changed.
void faceAdded(FontSet* set, uint32_t index) {
    PrStringView name = prRawStringView(&set->faces.data[index].family);
    bool known = false;
    for (uint32_t i = 0; i < set->families.count && !known; ++i)
        known = prAsciiEqualsFold(prRawStringView(&set->families.data[i]), name);
    if (!known)
        set->families.push(prRawStringCopy(name));
    familyChanged(set, name);
}

// Returns a face described by the descriptor, whose automatic descriptors come from the info of the face, the family,
// and whether it draws emoji, ranked after the faces of the set.
Face describedFace(FontSet* set, const PrFontFaceDescriptor* descriptor, const PrFontInfo& info, PrStringView family,
        bool emoji) {
    Face face{};
    PrFontFaceAutoFlags flags = descriptor->automatic;
    face.family = prRawStringCopy((flags & PrFontFaceAutoFlag_Family) != 0 ? family : descriptor->family);
    face.flags = descriptor->flags;
    face.unclamped = descriptor->unclamped;
    if ((flags & PrFontFaceAutoFlag_Emoji) != 0)
        face.flags = emoji ? face.flags | PrFontFaceFlag_Emoji : face.flags & ~PrFontFaceFlag_Emoji;
    bool autoWeight = (flags & PrFontFaceAutoFlag_Weight) != 0;
    face.weightMin = autoWeight ? info.weightMin : descriptor->weightMin;
    face.weightMax = autoWeight ? info.weightMax : descriptor->weightMax;
    bool autoWidth = (flags & PrFontFaceAutoFlag_Width) != 0;
    face.widthMin = autoWidth ? info.widthMin : descriptor->widthMin;
    face.widthMax = autoWidth ? info.widthMax : descriptor->widthMax;
    bool autoSlope = (flags & PrFontFaceAutoFlag_Slope) != 0;
    face.slope = autoSlope ? info.slope : descriptor->slope;
    face.obliqueMin = autoSlope ? info.obliqueMin : descriptor->obliqueMin;
    face.obliqueMax = autoSlope ? info.obliqueMax : descriptor->obliqueMax;
    face.info = info;
    if (descriptor->rangeCount != 0)
        face.ranges.append(descriptor->ranges, descriptor->rangeCount);
    const PrFontDerivation& derivation = descriptor->derivation;
    if (derivation.variationCount != 0)
        face.variations.append(derivation.variations, derivation.variationCount);
    if (derivation.featureCount != 0)
        face.features.append(derivation.features, derivation.featureCount);
    face.derivation = derivation;
    face.derivation.variations = nullptr;
    face.derivation.features = nullptr;
    face.path = prRawStringNull();
    face.rank = set->nextRank++;
    face.id = ++set->nextFaceId;
    return face;
}

// Adds a face described by the descriptor, as describedFace describes it.
uint32_t addFace(FontSet* set, const PrFontFaceDescriptor* descriptor, const PrFontInfo& info, PrStringView family, bool emoji) {
    uint32_t index = set->faces.count;
    set->faces.push(describedFace(set, descriptor, info, family, emoji));
    faceAdded(set, index);
    return index;
}

// Returns the face `index` of a file at the path, read from its tables in `sfnt` and described by `descriptor` with the
// family `family`. A face that may draw emoji is opened to find out, and keeps the font for matching. The font of any
// other face is created when a match first needs it.
Face fileFace(FontSet* set, const PrFontFaceDescriptor* descriptor, const Sfnt& sfnt, PrStringView path, uint32_t index,
        PrStringView family) {
    PrFontInfo info;
    sfntDescribe(sfnt, &info);
    const PrFont* font = nullptr;
    if ((descriptor->automatic & PrFontFaceAutoFlag_Emoji) != 0 && hasColorGlyphs(sfnt) &&
            prFontCreateFromFile(path, index, nullptr, &font) != PrStatus_Ok)
        font = nullptr;
    Face face = describedFace(set, descriptor, info, family, font != nullptr && drawsEmoji(font));
    face.path = prRawStringCopy(path);
    face.index = index;
    face.font = font;
    return face;
}

// Adds every face of the file at the path as `descriptor` describes it, and returns the number added.
uint32_t addFile(FontSet* set, const PrFontFaceDescriptor* descriptor, PrStringView path) {
    PrByteView mapping{};
    if (!prFileMap(path, &mapping))
        return 0;
    uint32_t faces = sfntFaceCount(mapping.data, mapping.size);
    uint32_t added = 0;
    for (uint32_t i = 0; i < faces; ++i) {
        Sfnt sfnt;
        if (!sfntOpen(mapping.data, mapping.size, i, &sfnt))
            continue;
        PrRawString family = sfntFamily(sfnt);
        if (prRawStringIsNull(&family))
            continue;
        set->faces.push(fileFace(set, descriptor, sfnt, path, i, prRawStringView(&family)));
        faceAdded(set, set->faces.count - 1);
        prRawStringReset(&family);
        ++added;
    }
    prFileUnmap(&mapping);
    return added;
}

// Reads the file of the unread face at `at`. Its faces of the family take the unread face's place and rank, described
// by the default descriptor. The faces that read as another family are left out, unless none reads as the family: the
// platform that listed the file may take the family from another of its names than a face reads, so then every face of
// the file takes the family. If no face can be read, the family stays defined and matches no face.
void readFile(FontSet* set, uint32_t at) {
    Face unread = set->faces.data[at];
    set->faces.erase(at);
    PrStringView family = prRawStringView(&unread.family);
    PrStringView path = prRawStringView(&unread.path);
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    PrByteView mapping{};
    if (prFileMap(path, &mapping)) {
        uint32_t faces = sfntFaceCount(mapping.data, mapping.size);
        uint32_t added = 0;
        for (uint32_t pass = 0; pass < 2 && added == 0; ++pass) {
            for (uint32_t i = 0; i < faces; ++i) {
                Sfnt sfnt;
                if (!sfntOpen(mapping.data, mapping.size, i, &sfnt))
                    continue;
                PrRawString own = sfntFamily(sfnt);
                bool same = !prRawStringIsNull(&own) && prAsciiEqualsFold(prRawStringView(&own), family);
                prRawStringReset(&own);
                if (pass == 0 && !same)
                    continue;
                Face face = fileFace(set, &descriptor, sfnt, path, i, family);
                face.rank = unread.rank;
                set->faces.push(face);
                ++added;
            }
        }
        prFileUnmap(&mapping);
    }
    faceReset(unread);
}

// Returns the font of the face, opening a file face the first time. Returns null if it cannot be read.
const PrFont* PR_NULLABLE fontOfFace(Face& face) {
    if (face.font == nullptr && !face.unreadable) {
        const PrFont* font = nullptr;
        PrStatus status = prFontCreateFromFile(prRawStringView(&face.path), face.index, nullptr, &font);
        if (status == PrStatus_Ok)
            face.font = font;
        else
            face.unreadable = true;
    }
    return face.font;
}

// The candidate value of a face on a matching axis: the value in its range nearest the value of the style.
struct Candidate {
    uint32_t face;
    float value;
};

// The order in which matching ranks a value against the value of a style, lowest first: a tier, then the distance.
struct Rank {
    uint32_t tier;
    float distance;
};

bool before(Rank a, Rank b) {
    return a.tier != b.tier ? a.tier < b.tier : a.distance < b.distance;
}

// For widths at or below kPrFontWidthNormal, the search tries narrower widths first. For wider widths, it tries wider
// first.
Rank widthRank(float value, float asked) {
    bool nearer = asked <= kPrFontWidthNormal ? value <= asked : value >= asked;
    return Rank{nearer ? 0u : 1u, prF32Abs(value - asked)};
}

// For weights from kPrFontWeightNormal to kPrFontSetWeightNormalMax, the search goes up to kPrFontSetWeightNormalMax,
// then lighter, then heavier. For lighter weights it tries lighter first, and for heavier weights heavier first.
Rank weightRank(float value, float asked) {
    float distance = prF32Abs(value - asked);
    if (asked >= kPrFontWeightNormal && asked <= kPrFontSetWeightNormalMax) {
        if (value >= asked && value <= kPrFontSetWeightNormalMax)
            return Rank{0, distance};
        return Rank{value < asked ? 1u : 2u, distance};
    }
    bool nearer = asked < kPrFontWeightNormal ? value <= asked : value >= asked;
    return Rank{nearer ? 0u : 1u, distance};
}

// For oblique angles at or above the threshold, the search tries larger angles first, and for smaller angles smaller
// first. Each direction goes toward zero before trying the other sign. Negative angles mirror this.
Rank obliqueRank(float value, float asked) {
    float distance = prF32Abs(value - asked);
    bool negative = asked < 0;
    float a = negative ? -asked : asked;
    float v = negative ? -value : value;
    if (v < 0)
        return Rank{2, distance};
    if (a >= kObliqueThreshold)
        return Rank{v >= a ? 0u : 1u, distance};
    return Rank{v <= a ? 0u : 1u, distance};
}

// Keeps the candidates whose values rank first.
void keepBest(RawArray<Candidate>& candidates, float asked, Rank (*rank)(float, float)) {
    if (candidates.count == 0)
        return;
    Rank best = rank(candidates.data[0].value, asked);
    for (uint32_t i = 1; i < candidates.count; ++i) {
        Rank r = rank(candidates.data[i].value, asked);
        if (before(r, best))
            best = r;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < candidates.count; ++i) {
        Rank r = rank(candidates.data[i].value, asked);
        if (!before(best, r))
            candidates.data[kept++] = candidates.data[i];
    }
    candidates.count = kept;
}

bool obliqueCapable(const Face& face) {
    return face.slope == PrFontSlope_Oblique || face.obliqueMin != face.obliqueMax;
}

bool italicCapable(const Face& face) {
    return face.slope == PrFontSlope_Italic || face.info.italicAxis;
}

bool normalCapable(const Face& face) {
    if (face.slope == PrFontSlope_Normal)
        return true;
    return face.slope == PrFontSlope_Oblique && face.obliqueMin <= 0 && face.obliqueMax >= 0;
}

// How a face provides the slope that a style asks for, which determines the order in which its kind is searched and the
// oblique angle at which it is drawn.
enum { SlopeKind_Normal, SlopeKind_Italic, SlopeKind_Oblique, SlopeKind_None };
typedef uint8_t SlopeKind;

// Writes the slope kinds that a style searches, in order.
void slopeOrder(const PrFontStyle* style, SlopeKind* order) {
    if (style->slope == PrFontSlope_Italic) {
        order[0] = SlopeKind_Italic;
        order[1] = SlopeKind_Oblique;
        order[2] = SlopeKind_Normal;
    } else if (style->slope == PrFontSlope_Oblique) {
        order[0] = SlopeKind_Oblique;
        order[1] = SlopeKind_Italic;
        order[2] = SlopeKind_Normal;
    } else {
        order[0] = SlopeKind_Normal;
        order[1] = SlopeKind_Oblique;
        order[2] = SlopeKind_Italic;
    }
}

bool givesKind(const Face& face, SlopeKind kind) {
    if (kind == SlopeKind_Italic)
        return italicCapable(face);
    if (kind == SlopeKind_Oblique)
        return obliqueCapable(face);
    return normalCapable(face);
}

// Narrows the candidates by slope, and returns the kind they provide.
SlopeKind keepSlope(const FontSet* set, RawArray<Candidate>& candidates, const PrFontStyle* style) {
    SlopeKind order[3];
    slopeOrder(style, order);
    for (SlopeKind kind : order) {
        uint32_t kept = 0;
        for (uint32_t i = 0; i < candidates.count; ++i) {
            if (givesKind(set->faces.data[candidates.data[i].face], kind))
                candidates.data[kept++] = candidates.data[i];
        }
        if (kept == 0)
            continue;
        candidates.count = kept;
        if (kind == SlopeKind_Oblique) {
            float asked = 0;
            if (style->slope == PrFontSlope_Oblique)
                asked = style->obliqueAngle;
            else if (style->slope == PrFontSlope_Italic)
                asked = kPrFontSetObliqueAngle;
            for (uint32_t i = 0; i < candidates.count; ++i) {
                const Face& face = set->faces.data[candidates.data[i].face];
                candidates.data[i].value = prF32Clamp(asked, face.obliqueMin, face.obliqueMax);
            }
            keepBest(candidates, asked, obliqueRank);
        }
        return kind;
    }
    return SlopeKind_None;
}

// Returns the automatic axis values of an instance of the face for the style.
AxisKey instanceAxes(const Face& face, const PrFontStyle* style, SlopeKind kind) {
    AxisKey axes{};
    const PrFontInfo& d = face.info;
    bool weightFree = (face.unclamped & PrFontFaceAutoFlag_Weight) != 0;
    bool widthFree = (face.unclamped & PrFontFaceAutoFlag_Width) != 0;
    bool slopeFree = (face.unclamped & PrFontFaceAutoFlag_Slope) != 0;
    if (d.weightMin != d.weightMax) {
        float weight = prF32Clamp(style->weight, weightFree ? d.weightMin : face.weightMin,
            weightFree ? d.weightMax : face.weightMax);
        setAxis(axes, 0, weight);
    }
    if (d.widthMin != d.widthMax) {
        float width =
            prF32Clamp(style->width, widthFree ? d.widthMin : face.widthMin, widthFree ? d.widthMax : face.widthMax);
        setAxis(axes, 1, width);
    }
    if (d.obliqueMin != d.obliqueMax) {
        float angle = 0;
        if (kind == SlopeKind_Oblique || (slopeFree && style->slope == PrFontSlope_Oblique))
            angle = style->slope == PrFontSlope_Oblique ? style->obliqueAngle : kPrFontSetObliqueAngle;
        setAxis(axes, 2, -prF32Clamp(angle, slopeFree ? d.obliqueMin : face.obliqueMin,
            slopeFree ? d.obliqueMax : face.obliqueMax));
    }
    if (d.italicAxis)
        setAxis(axes, 3, kind == SlopeKind_Italic ? 1 : 0);
    if (d.opticalSizeMin != d.opticalSizeMax && (style->flags & PrFontStyleFlag_OpticalSizing) != 0)
        setAxis(axes, 4, style->size);
    return axes;
}

// Returns the instance of the face with the axes, hinted as `hinting` says unless the face's own derivation sets its
// hinting, made the first time it is requested. The set holds the instance while the face is in the set. Returns null
// if the face cannot be read.
const PrFont* PR_NULLABLE instanceOf(FontSet* set, uint32_t faceIndex, const AxisKey& axes, SetHinting hinting) {
    Face& face = set->faces.data[faceIndex];
    if ((face.derivation.overrides & PrFontOverrideFlag_Hinting) != 0)
        hinting = SetHinting{PrFontHinting_None, nullptr};
    for (uint32_t i = 0; i < set->instances.count; ++i) {
        const Instance& instance = set->instances.data[i];
        if (instance.face == face.id && prMemEquals(&instance.axes, &axes, sizeof(AxisKey)) &&
                instance.hinting == hinting.hinting && instance.hinter == hinting.hinter)
            return instance.font;
    }
    const PrFont* base = fontOfFace(face);
    if (base == nullptr)
        return nullptr;
    RawArray<PrFontVariation> variations;
    for (uint32_t i = 0; i < kAutoAxisCount; ++i) {
        if ((axes.mask & (1u << i)) != 0)
            variations.push(PrFontVariation{kAutoAxes[i], axes.values[i]});
    }
    if (face.variations.count != 0)
        variations.append(face.variations.span());
    PrFontDerivation derivation = face.derivation;
    derivation.variations = variations.data;
    derivation.variationCount = variations.count;
    derivation.features = face.features.data;
    derivation.featureCount = face.features.count;
    derivation.embolden += axes.embolden;
    derivation.oblique += axes.oblique;
    if (hinting.hinting != PrFontHinting_None && hinting.hinter != nullptr) {
        derivation.hinting = hinting.hinting;
        derivation.hinter = hinting.hinter;
        derivation.overrides |= PrFontOverrideFlag_Hinting;
    }
    bool plain = variations.count == 0 && face.features.count == 0 &&
        derivation.sizeAdjust == 1 && derivation.overrides == 0 && derivation.embolden == 0 && derivation.oblique == 0;
    const PrFont* font = base;
    if (plain)
        prFontRetain(base);
    else
        font = prFontCreateDerived(base, &derivation);
    variations.reset();
    set->instances.push(Instance{face.id, axes, hinting.hinting, hinting.hinter, font});
    return font;
}

// Returns true if the set has its own faces of `family`, in any state other than removed. A family that it defines
// hides the family of that name in its parent.
bool definesFamily(const FontSet* set, PrStringView family) {
    for (uint32_t i = 0; i < set->faces.count; ++i) {
        const Face& face = set->faces.data[i];
        if (prAsciiEqualsFold(prRawStringView(&face.family), family))
            return true;
    }
    return false;
}

// Returns the set, among `set` and its parents, whose faces are those of `family`, or null if none has any.
FontSet* PR_NULLABLE definingSet(FontSet* set, PrStringView family) {
    for (FontSet* at = set; at != nullptr; at = at->parent) {
        prMutexLock(&at->lock);
        bool defines = definesFamily(at, family);
        prMutexUnlock(&at->lock);
        if (defines)
            return at;
    }
    return nullptr;
}

// Returns the family that a name stands for by an alias of the set or its parents, the nearest first, or the empty
// view.
PrStringView aliasOf(const FontSet* set, PrStringView name) {
    for (const FontSet* at = set; at != nullptr; at = at->parent) {
        PrStringView family = {};
        prMutexLock(&at->lock);
        for (uint32_t a = 0; a < at->aliases.count && family.data == nullptr; ++a) {
            const Alias& alias = at->aliases.data[a];
            if (prAsciiEqualsFold(prRawStringView(&alias.name), name))
                family = prRawStringView(&alias.family);
        }
        prMutexUnlock(&at->lock);
        if (family.data != nullptr)
            return family;
    }
    return PrStringView{};
}

// Returns the family that the generic family stands for in the set or its parents, the nearest binding first, or the
// empty view.
PrStringView genericOf(const FontSet* set, PrFontGeneric generic) {
    for (const FontSet* at = set; at != nullptr; at = at->parent) {
        prMutexLock(&at->lock);
        bool bound = !prRawStringIsNull(&at->generics[generic]);
        PrStringView family = prRawStringView(&at->generics[generic]);
        prMutexUnlock(&at->lock);
        if (bound)
            return family;
    }
    return PrStringView{};
}

// Returns how `set` hints the faces that it matches: its own hinting, or the nearest parent's.
SetHinting hintingOf(const FontSet* set) {
    for (const FontSet* at = set; at != nullptr; at = at->parent) {
        prMutexLock(&at->lock);
        bool own = at->hintingSet;
        SetHinting hinting = at->hinting;
        prMutexUnlock(&at->lock);
        if (own)
            return hinting;
    }
    return SetHinting{PrFontHinting_None, nullptr};
}

// Matches the style among the faces of `family` that the set holds, under its lock, hinted as `hinting` says.
// prFontSetMatch describes the matching.
void matchIn(FontSet* set, const PrFontStyle* style, PrStringView family, SetHinting hinting, FontMatch& out) {
    prMutexLock(&set->lock);
    for (uint32_t i = 0; i < set->faces.count;) {
        const Face& face = set->faces.data[i];
        if (face.state == FaceState_Unread && prAsciiEqualsFold(prRawStringView(&face.family), family))
            readFile(set, i);
        else
            ++i;
    }
    // Every face of the family takes part in narrowing, including a failed one, so that its failure leaves the style
    // that it matched without a face instead of moving the match to another style.
    RawArray<Candidate> candidates;
    for (uint32_t i = 0; i < set->faces.count; ++i) {
        const Face& face = set->faces.data[i];
        if (prAsciiEqualsFold(prRawStringView(&face.family), family))
            candidates.push(Candidate{i, prF32Clamp(style->width, face.widthMin, face.widthMax)});
    }
    // Sorts the highest rank first, an order that keepBest keeps.
    for (uint32_t i = 1; i < candidates.count; ++i) {
        Candidate moving = candidates.data[i];
        uint32_t at = i;
        for (; at > 0 && set->faces.data[candidates.data[at - 1].face].rank < set->faces.data[moving.face].rank; --at)
            candidates.data[at] = candidates.data[at - 1];
        candidates.data[at] = moving;
    }
    keepBest(candidates, style->width, widthRank);
    SlopeKind kind = keepSlope(set, candidates, style);
    for (uint32_t i = 0; i < candidates.count; ++i) {
        const Face& face = set->faces.data[candidates.data[i].face];
        candidates.data[i].value = prF32Clamp(style->weight, face.weightMin, face.weightMax);
    }
    keepBest(candidates, style->weight, weightRank);
    for (uint32_t i = 0; i < candidates.count; ++i) {
        uint32_t faceIndex = candidates.data[i].face;
        const Face& face = set->faces.data[faceIndex];
        if (face.state == FaceState_Failed)
            continue;
        const PrFont* PR_NULLABLE font = nullptr;
        if (face.state == FaceState_Ready) {
            float weight = candidates.data[i].value;
            AxisKey axes = instanceAxes(face, style, kind);
            if ((style->flags & PrFontStyleFlag_SynthesizeWeight) != 0 && style->weight >= kBoldWeight &&
                    weight < kBoldWeight)
                axes.embolden = kEmbolden;
            bool slanted = style->slope != PrFontSlope_Normal;
            // A face whose slope leaves its slant axis free slants along that axis, and is not sheared as well.
            bool slants = (face.unclamped & PrFontFaceAutoFlag_Slope) != 0 &&
                style->slope == PrFontSlope_Oblique && face.info.obliqueMin != face.info.obliqueMax;
            if ((style->flags & PrFontStyleFlag_SynthesizeStyle) != 0 && slanted && kind == SlopeKind_Normal && !slants) {
                float angle = style->slope == PrFontSlope_Oblique ? style->obliqueAngle : kPrFontSetObliqueAngle;
                axes.oblique = prF32Tan(angle * (kPrF32Pi / 180));
            }
            font = instanceOf(set, faceIndex, axes, hinting);
            if (font == nullptr)
                continue;
        }
        const Face& kept = set->faces.data[faceIndex];
        if (font != nullptr)
            prFontRetain(font);
        out.faces.push(MatchedFace{
            PrFontFace{font, nullptr, kept.ranges.count, kept.flags, kept.state == FaceState_Pending ? kept.key : 0},
            out.ranges.count,
        });
        out.ranges.append(kept.ranges.span());
    }
    prMutexUnlock(&set->lock);
    candidates.reset();
}

// The keyword of each generic family, by PrFontGeneric.
constexpr PrStringView kGenericNames[PrFontGeneric_Count] = {
    {nullptr, 0},         "serif",        "sans-serif",  "monospace",
    "cursive",     "fantasy",      "system-ui",   "math",
    "emoji",       "ui-serif",     "ui-sans-serif", "ui-monospace",
    "ui-rounded",  "fangsong",     "kai",         "khmer-mul",
    "nastaliq",
};

// Returns the set whose fallback families a set falls back to: the outermost of it and its parents.
const FontSet* fallbackSet(const FontSet* set) {
    while (set->parent != nullptr)
        set = set->parent;
    return set;
}

constexpr PrStringView kSystemBackendNames[PrFontSystemBackend_Count] = {"default", "coretext", "directwrite"};

// The backends of the system's fonts that the build has, which Default installs the first of, and then Default, so that
// a build without backends installs none.
constexpr PrFontSystemBackend kSystemBuilt[] = {
#if defined(PR_FONT_HAS_CORETEXT)
    PrFontSystemBackend_CoreText,
#endif
#if defined(PR_FONT_HAS_DIRECTWRITE)
    PrFontSystemBackend_DirectWrite,
#endif
    PrFontSystemBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrFontMatch : FontMatch {};
struct PrFontSet : FontSet {};

PrFontGeneric prFontGenericFromName(PrStringView keyword) {
    for (uint32_t i = 1; i < PrFontGeneric_Count; ++i) {
        if (prAsciiEqualsFold(kGenericNames[i], keyword))
            return PrFontGeneric(i);
    }
    return PrFontGeneric_None;
}

PrStringView prFontGenericName(PrFontGeneric generic) {
    PR_ASSERT(generic < PrFontGeneric_Count);
    return kGenericNames[generic];
}

bool prFontFaceServes(const PrFontFace* face, uint32_t codePoint) {
    return face->font != nullptr && prFontFaceInRanges(face, codePoint) && prFontCodePointGlyph(face->font, codePoint) != 0;
}

bool prFontFaceInRanges(const PrFontFace* face, uint32_t codePoint) {
    bool inRange = face->rangeCount == 0;
    for (uint32_t i = 0; i < face->rangeCount && !inRange; ++i)
        inRange = codePoint >= face->ranges[i].first && codePoint <= face->ranges[i].last;
    return inRange;
}

PrFontFaceDescriptor prFontFaceDescriptorDefault(void) {
    PrFontFaceDescriptor d{};
    d.weightMin = d.weightMax = kPrFontWeightNormal;
    d.widthMin = d.widthMax = kPrFontWidthNormal;
    d.slope = PrFontSlope_Normal;
    d.derivation = prFontDerivationDefault();
    d.automatic = PrFontFaceAutoFlag_All;
    return d;
}

PrFontSet* prFontSetCreate(PrFontSet* PR_NULLABLE parent) {
    PrFontSet* set = allocZeroed<PrFontSet>();
    set->parent = parent;
    prMutexInit(&set->lock);
    changed(set);
    return set;
}

void prFontSetDestroy(PrFontSet* PR_NULLABLE set) {
    if (set == nullptr)
        return;
    for (uint32_t i = 0; i < set->instances.count; ++i)
        prFontRelease(set->instances.data[i].font);
    for (uint32_t i = 0; i < set->faces.count; ++i)
        faceReset(set->faces.data[i]);
    stringsReset(&set->families);
    for (uint32_t i = 0; i < set->familyStamps.count; ++i)
        prRawStringReset(&set->familyStamps.data[i].name);
    set->familyStamps.reset();
    for (uint32_t i = 0; i < set->aliases.count; ++i) {
        prRawStringReset(&set->aliases.data[i].name);
        prRawStringReset(&set->aliases.data[i].family);
    }
    set->aliases.reset();
    for (PrRawString& generic : set->generics)
        prRawStringReset(&generic);
    prRawStringReset(&set->defaultName);
    stringsReset(&set->fallback);
    set->fallbackLocales.reset();
    set->fallbackGenerics.reset();
    set->requests.reset();
    set->instances.reset();
    set->faces.reset();
    prMutexReset(&set->lock);
    prMemoryFree(set);
}

uint64_t prFontSetVersion(const PrFontSet* set) {
    uint64_t version = 0;
    for (const FontSet* at = set; at != nullptr; at = at->parent) {
        uint64_t stamp = at->stamp.load();
        version = stamp > version ? stamp : version;
    }
    return version;
}

uint64_t prFontSetFamilyStamp(const PrFontSet* set, PrFontFamily family) {
    // Resolves the family in the same way as prFontSetMatch: the binding of the generic, then the alias of that name.
    // Both the name and the target of the alias are watched, because a family defined under the name shadows the alias.
    PrStringView name = family.name;
    if (family.generic != PrFontGeneric_None)
        name = genericOf(set, family.generic);
    PrStringView aliased = name.size != 0 ? aliasOf(set, name) : PrStringView{};
    uint64_t stamp = 0;
    for (const FontSet* at = set; at != nullptr; at = at->parent) {
        prMutexLock(&at->lock);
        uint64_t own = ownFamilyStamp(at, name);
        if (aliased.size != 0) {
            uint64_t target = ownFamilyStamp(at, aliased);
            own = target > own ? target : own;
        }
        prMutexUnlock(&at->lock);
        stamp = own > stamp ? own : stamp;
    }
    return stamp;
}

uint64_t prFontSetFallbackStamp(const PrFontSet* set) {
    return fallbackSet(set)->stamp.load();
}

void prFontSetAdd(PrFontSet* set, const PrFontFaceDescriptor* descriptor, const PrFont* font) {
    PrRawString family = prFontFamilyName(font);
    prMutexLock(&set->lock);
    uint32_t index = addFace(set, descriptor, prFontInfo(font), prRawStringView(&family), drawsEmoji(font));
    prRawStringReset(&family);
    Face& face = set->faces.data[index];
    face.index = prFontIndex(font);
    prFontRetain(font);
    face.font = font;
    prMutexUnlock(&set->lock);
}

uint32_t prFontSetAddFile(PrFontSet* set, const PrFontFaceDescriptor* descriptor, PrStringView path) {
    prMutexLock(&set->lock);
    uint32_t added = addFile(set, descriptor, path);
    prMutexUnlock(&set->lock);
    return added;
}

void prFontSetAddFileFamily(PrFontSet* set, PrStringView family, PrStringView path) {
    prMutexLock(&set->lock);
    bool known = false;
    for (uint32_t i = 0; i < set->faces.count && !known; ++i) {
        const Face& face = set->faces.data[i];
        PrStringView added = prRawStringView(&face.path);
        known = face.state == FaceState_Unread && added.size == path.size &&
            prMemEquals(added.data, path.data, path.size) && prAsciiEqualsFold(prRawStringView(&face.family), family);
    }
    if (!known) {
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        descriptor.automatic = 0;
        descriptor.family = family;
        uint32_t index = addFace(set, &descriptor, PrFontInfo{}, family, false);
        Face& face = set->faces.data[index];
        face.path = prRawStringCopy(path);
        face.state = FaceState_Unread;
    }
    prMutexUnlock(&set->lock);
}

PrFontSystemBackend prFontSystemBackendFromName(PrStringView name) {
    for (PrFontSystemBackend backend = 0; backend < PrFontSystemBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kSystemBackendNames[backend], name))
            return backend;
    }
    return PrFontSystemBackend_Count;
}

PrStringView prFontSystemBackendName(PrFontSystemBackend backend) {
    return backend < PrFontSystemBackend_Count ? kSystemBackendNames[backend] : PrStringView{};
}

bool prFontSystemBackendIsBuilt(PrFontSystemBackend backend) {
    for (PrFontSystemBackend built : kSystemBuilt) {
        if (built != PrFontSystemBackend_Default && (built == backend || backend == PrFontSystemBackend_Default))
            return true;
    }
    return false;
}

uint32_t prFontSystemInstall(PrFontSystemBackend backend, PrFontSet* set) {
    switch (backend == PrFontSystemBackend_Default ? kSystemBuilt[0] : backend) {
#if defined(PR_FONT_HAS_CORETEXT)
        case PrFontSystemBackend_CoreText: return prFontCoreTextInstall(set);
#endif
#if defined(PR_FONT_HAS_DIRECTWRITE)
        case PrFontSystemBackend_DirectWrite: return prFontDirectWriteInstall(set);
#endif
        default: (void)set; return 0;
    }
}

void prFontSetAddPending(PrFontSet* set, const PrFontFaceDescriptor* descriptor, uint64_t key) {
    PR_ASSERT(key != 0);
    PrFontFaceDescriptor given = *descriptor;
    given.automatic = 0;
    PrFontInfo info{};
    info.weightMin = info.weightMax = given.weightMin;
    info.widthMin = info.widthMax = given.widthMin;
    info.slope = given.slope;
    prMutexLock(&set->lock);
    uint32_t index = addFace(set, &given, info, given.family, false);
    Face& face = set->faces.data[index];
    face.key = key;
    face.state = FaceState_Pending;
    face.autoEmoji = (descriptor->automatic & PrFontFaceAutoFlag_Emoji) != 0;
    prMutexUnlock(&set->lock);
}

void prFontSetLoaded(PrFontSet* set, uint64_t key, const PrFont* font) {
    prMutexLock(&set->lock);
    for (uint32_t i = 0; i < set->faces.count; ++i) {
        Face& face = set->faces.data[i];
        if (face.state != FaceState_Pending || face.key != key)
            continue;
        prFontRetain(font);
        face.font = font;
        face.index = prFontIndex(font);
        face.info = prFontInfo(font);
        if (face.autoEmoji && drawsEmoji(font))
            face.flags |= PrFontFaceFlag_Emoji;
        face.state = FaceState_Ready;
        familyChanged(set, prRawStringView(&face.family));
    }
    prMutexUnlock(&set->lock);
}

void prFontSetFailed(PrFontSet* set, uint64_t key) {
    prMutexLock(&set->lock);
    for (uint32_t i = 0; i < set->faces.count; ++i) {
        Face& face = set->faces.data[i];
        if (face.state == FaceState_Pending && face.key == key) {
            face.state = FaceState_Failed;
            familyChanged(set, prRawStringView(&face.family));
        }
    }
    prMutexUnlock(&set->lock);
}

void prFontSetRemove(PrFontSet* set, uint64_t key) {
    prMutexLock(&set->lock);
    for (uint32_t i = 0; i < set->faces.count;) {
        Face& face = set->faces.data[i];
        if (face.key != key) {
            ++i;
            continue;
        }
        familyChanged(set, prRawStringView(&face.family));
        for (uint32_t k = 0; k < set->instances.count;) {
            if (set->instances.data[k].face == face.id) {
                prFontRelease(set->instances.data[k].font);
                set->instances.data[k] = set->instances.last();
                set->instances.pop();
            } else {
                ++k;
            }
        }
        faceReset(face);
        set->faces.erase(i);
    }
    set->requests.removeFirst(key);
    set->requestCount.store(set->requests.count);
    prMutexUnlock(&set->lock);
}

void prFontSetOrder(PrFontSet* set, const uint64_t* PR_NULLABLE keys, uint32_t count) {
    prMutexLock(&set->lock);
    // The faces of each key in turn, as they rank now. The order changes nothing if they already rank in it.
    RawArray<uint32_t> ordered;
    bool inOrder = true;
    for (uint32_t k = 0; k < count; ++k) {
        for (uint32_t i = 0; i < set->faces.count; ++i) {
            const Face& face = set->faces.data[i];
            if (face.key != keys[k])
                continue;
            if (ordered.count != 0 && set->faces.data[ordered.last()].rank > face.rank)
                inOrder = false;
            ordered.push(i);
        }
    }
    if (!inOrder) {
        for (uint32_t i = 0; i < ordered.count; ++i) {
            Face& face = set->faces.data[ordered.data[i]];
            face.rank = set->nextRank++;
            familyChanged(set, prRawStringView(&face.family));
        }
    }
    prMutexUnlock(&set->lock);
    ordered.reset();
}

void prFontSetRequestLoad(PrFontSet* set, uint64_t key) {
    for (FontSet* at = set; at != nullptr; at = at->parent) {
        prMutexLock(&at->lock);
        bool pending = false;
        for (uint32_t i = 0; i < at->faces.count && !pending; ++i) {
            const Face& face = at->faces.data[i];
            pending = face.state == FaceState_Pending && face.key == key;
        }
        if (pending && at->requests.find(key) == kArrayMissing) {
            at->requests.push(key);
            at->requestCount.store(at->requests.count);
        }
        prMutexUnlock(&at->lock);
        if (pending)
            return;
    }
}

uint32_t prFontSetTakeLoads(PrFontSet* set, uint64_t* PR_NULLABLE out, uint32_t capacity) {
    // A request made while this reads none is taken by the next take, as a request made after it would be.
    if (set->requestCount.load() == 0)
        return 0;
    prMutexLock(&set->lock);
    uint32_t taken = out != nullptr && capacity < set->requests.count ? capacity : set->requests.count;
    if (out == nullptr)
        taken = 0;
    for (uint32_t i = 0; i < taken; ++i)
        out[i] = set->requests.data[i];
    uint32_t left = set->requests.count - taken;
    prMemMove(set->requests.data, set->requests.data + taken, left * sizeof(uint64_t));
    set->requests.count = left;
    set->requestCount.store(left);
    prMutexUnlock(&set->lock);
    return taken;
}

uint32_t prFontSetFaceCount(const PrFontSet* set) {
    prMutexLock(&set->lock);
    uint32_t count = set->faces.count;
    prMutexUnlock(&set->lock);
    return count;
}

void prFontSetAlias(PrFontSet* set, PrStringView name, PrStringView family) {
    prMutexLock(&set->lock);
    bindingChanged(set);
    bool replaced = false;
    for (uint32_t i = 0; i < set->aliases.count && !replaced; ++i) {
        Alias& alias = set->aliases.data[i];
        if (prAsciiEqualsFold(prRawStringView(&alias.name), name)) {
            prRawStringReset(&alias.family);
            alias.family = prRawStringCopy(family);
            replaced = true;
        }
    }
    if (!replaced)
        set->aliases.push(Alias{prRawStringCopy(name), prRawStringCopy(family)});
    prMutexUnlock(&set->lock);
}

void prFontSetSetGeneric(PrFontSet* set, PrFontGeneric generic, PrStringView family) {
    PR_ASSERT(generic != PrFontGeneric_None && generic < PrFontGeneric_Count);
    prMutexLock(&set->lock);
    prRawStringReset(&set->generics[generic]);
    set->generics[generic] = family.size != 0 ? prRawStringCopy(family) : prRawStringEmpty();
    bindingChanged(set);
    prMutexUnlock(&set->lock);
}

PrStringView prFontSetGenericFamily(const PrFontSet* set, PrFontGeneric generic) {
    PR_ASSERT(generic < PrFontGeneric_Count);
    return generic == PrFontGeneric_None ? PrStringView{} : genericOf(set, generic);
}

void prFontSetSetDefaultFamily(PrFontSet* set, PrFontFamily family) {
    prMutexLock(&set->lock);
    prRawStringReset(&set->defaultName);
    set->defaultName = prRawStringCopy(family.name);
    set->defaultGeneric = family.generic;
    set->defaultSet = true;
    bindingChanged(set);
    prMutexUnlock(&set->lock);
}

PrFontFamily prFontSetDefaultFamily(const PrFontSet* set) {
    for (const FontSet* at = set; at != nullptr; at = at->parent) {
        prMutexLock(&at->lock);
        bool own = at->defaultSet;
        PrFontFamily family = PrFontFamily{prRawStringView(&at->defaultName), at->defaultGeneric};
        prMutexUnlock(&at->lock);
        if (own)
            return family;
    }
    return prFontFamilyGeneric(PrFontGeneric_Serif);
}

void prFontSetSetHinting(PrFontSet* set, PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter) {
    prMutexLock(&set->lock);
    set->hinting = SetHinting{hinting, hinter};
    if (hinter == nullptr)
        set->hinting.hinting = PrFontHinting_None;
    set->hintingSet = true;
    bindingChanged(set);
    prMutexUnlock(&set->lock);
}

PrFontHinting prFontSetHinting(const PrFontSet* set, PrFontHinter* PR_NULLABLE* PR_NULLABLE hinter) {
    SetHinting hinting = hintingOf(set);
    if (hinter != nullptr)
        *hinter = hinting.hinter;
    return hinting.hinting;
}

void prFontSetSetFallbackFamilies(PrFontSet* set, const PrFontFallbackFamily* PR_NULLABLE families, uint32_t count) {
    prMutexLock(&set->lock);
    stringsClear(&set->fallback);
    set->fallbackLocales.clear();
    set->fallbackGenerics.clear();
    for (uint32_t i = 0; i < count; ++i) {
        set->fallback.push(prRawStringCopy(families[i].family));
        set->fallbackLocales.push(families[i].locale);
        set->fallbackGenerics.push(families[i].generic);
    }
    bindingChanged(set);
    prMutexUnlock(&set->lock);
}

uint32_t prFontSetFallbackCount(const PrFontSet* set) {
    const FontSet* root = fallbackSet(set);
    prMutexLock(&root->lock);
    uint32_t count = root->fallback.count;
    prMutexUnlock(&root->lock);
    return count;
}

PrFontFallbackFamily prFontSetFallbackFamily(const PrFontSet* set, uint32_t index) {
    const FontSet* root = fallbackSet(set);
    prMutexLock(&root->lock);
    PR_ASSERT(index < root->fallback.count);
    PrFontFallbackFamily family = {
        prRawStringView(&root->fallback.data[index]), root->fallbackLocales.data[index],
        root->fallbackGenerics.data[index],
    };
    prMutexUnlock(&root->lock);
    return family;
}

uint32_t prFontSetFamilyCount(const PrFontSet* set) {
    prMutexLock(&set->lock);
    uint32_t count = set->families.count;
    prMutexUnlock(&set->lock);
    return count;
}

PrStringView prFontSetFamily(const PrFontSet* set, uint32_t index) {
    prMutexLock(&set->lock);
    PR_ASSERT(index < set->families.count);
    PrStringView family = prRawStringView(&set->families.data[index]);
    prMutexUnlock(&set->lock);
    return family;
}

PrFontStyle prFontStyleDefault(void) {
    PrFontStyle style{};
    style.weight = kPrFontWeightNormal;
    style.width = kPrFontWidthNormal;
    style.slope = PrFontSlope_Normal;
    style.obliqueAngle = kPrFontSetObliqueAngle;
    style.size = 16;
    style.flags = PrFontStyleFlag_OpticalSizing | PrFontStyleFlag_SynthesizeWeight | PrFontStyleFlag_SynthesizeStyle;
    return style;
}

PrFontMatch* prFontMatchCreate(void) {
    return allocZeroed<PrFontMatch>();
}

void prFontMatchDestroy(PrFontMatch* PR_NULLABLE match) {
    if (match == nullptr)
        return;
    prFontMatchClear(match);
    match->faces.reset();
    match->ranges.reset();
    prMemoryFree(match);
}

void prFontMatchClear(PrFontMatch* match) {
    for (uint32_t i = 0; i < match->faces.count; ++i)
        prFontRelease(match->faces.data[i].face.font);
    match->faces.clear();
    match->ranges.clear();
}

uint32_t prFontMatchFaceCount(const PrFontMatch* match) {
    return match->faces.count;
}

PrFontFace prFontMatchFace(const PrFontMatch* match, uint32_t index) {
    PR_ASSERT(index < match->faces.count);
    const MatchedFace& matched = match->faces.data[index];
    PrFontFace face = matched.face;
    face.ranges = face.rangeCount != 0 ? match->ranges.data + matched.rangeStart : nullptr;
    return face;
}

void prFontSetMatch(PrFontSet* set, PrFontFamily family, const PrFontStyle* style, PrFontMatch* out) {
    PrStringView name = family.name;
    if (family.generic != PrFontGeneric_None) {
        name = genericOf(set, family.generic);
        if (name.size == 0)
            return;
    }
    FontSet* defining = definingSet(set, name);
    if (defining == nullptr) {
        PrStringView aliased = aliasOf(set, name);
        if (aliased.size == 0)
            return;
        name = aliased;
        defining = definingSet(set, name);
        if (defining == nullptr)
            return;
    }
    matchIn(defining, style, name, hintingOf(set), *out);
}

void prFontSetMatchFallback(PrFontSet* set, uint32_t index, const PrFontStyle* style, PrFontMatch* out) {
    FontSet* root = const_cast<FontSet*>(fallbackSet(set));
    prMutexLock(&root->lock);
    PR_ASSERT(index < root->fallback.count);
    PrRawString name = prRawStringClone(&root->fallback.data[index]);
    prMutexUnlock(&root->lock);
    if (definingSet(root, prRawStringView(&name)) == root)
        matchIn(root, style, prRawStringView(&name), hintingOf(set), *out);
    prRawStringReset(&name);
}

