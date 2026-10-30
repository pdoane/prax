// Computed Style
//
// css-cascade-5 - https://drafts.csswg.org/css-cascade-5/
// css-values-4 - https://drafts.csswg.org/css-values-4/
// css-variables-1 - https://drafts.csswg.org/css-variables-1/
//
// The computed values (css-cascade-5/#computed) of the properties that the engine renders. A style holds one group of
// each kind, and a group holds the computed values of its properties as value runs, in one allocation. The resolver
// that computes groups interns them, so elements with the same styles share them. A child shares an inherited group
// with its parent if the child declares none of the group's properties. A group's kind is inherited if its properties
// are inherited.
//
// Styles, groups and custom properties are immutable once created, and their reference counts are atomic, so the
// threads of a restyle share them. Each thread computes with a builder of its own, and the builders intern into one
// table.

#pragma once

#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/color.h"
#include "pr/font_set.h"
#include "pr/hash_index.h"
#include "pr/locale.h"
#include "pr/mutex.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/text_break.h"
#include "pr/text_buffer.h"

#include "css/src/css_color_value.h"
#include "css/src/css_property.h"
#include "css/src/css_style_group.h"
#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the slot of `property`, which is a longhand.
CssStyleSlot cssStyleSlot(PrCssProperty property);

// Returns the properties of `group`, in the order of their slots.
CssStyleGroupProperties cssStyleGroupProperties(CssStyleGroup group);

// Returns true if the group's properties inherit. Every loop over a style's groups calls it, so it tests a constant
// mask.
inline bool cssStyleGroupInherited(CssStyleGroup group) {
    return (kCssStyleGroupsInherited >> group & 1u) != 0;
}

struct CssStyleTable;

// The computed values of one group: a value run for each of its properties, which share one pool of values and text.
struct CssStyleValues {
    mutable Atomic<uint32_t> references;
    uint32_t hash;
    CssStyleGroup group;
    uint32_t valueCount;
    uint32_t textSize;
    CssValueSpan* spans;            // by slot
    CssValueNode* values;
    char* PR_NULLABLE text;
    CssStyleTable* PR_NULLABLE owner; // the table that interns the group; the group leaves it when freed
    uint32_t slot;                    // the group's position among the groups of its part of the owner
};

// Returns the computed value of the property at `index` of the group's slots.
inline CssValueRun cssStyleValue(const CssStyleValues* values, uint32_t index) {
    CssValueSpan span = values->spans[index];
    return CssValueRun{values->values + span.start, span.count, values->text};
}

// The computed custom properties of an element (css-variables-1/#defining-variables). A set holds its own entries over
// an optional base set, and a lookup checks the entries first, then the base. A base never has a base of its own, so a
// lookup reads at most two sets.
//
// An element that declares no custom property shares its parent's set. An element that declares some gets a new set
// over the base of its parent's set. The new set holds the parent's own entries merged with the element's declarations.
// Declaring one property therefore copies only the entries declared between the base and the element, not every
// inherited one, so the whole document shares a large set on the root.
//
// Sets are interned by their base and their own entries, like groups. An element whose style is computed again under
// the same parent with the same declarations gets the same set, so comparing the two sets is immediate.

// Marks a guaranteed-invalid value (css-variables-1/#guaranteed-invalid-value), which hides the base's entry of that
// name.
constexpr uint32_t kCssCustomInvalid = 0xffffffffu;

// One entry of a set. Entries are sorted by name hash, then by name.
struct CssCustomEntry {
    uint32_t hash;                  // cssCustomHash of the name
    uint32_t name;                  // offsets into the set's text
    uint32_t nameSize;
    uint32_t value;
    uint32_t valueSize;             // kCssCustomInvalid for a guaranteed-invalid value
};

// A set of custom properties: the entries, and the text that they reference, in one allocation.
struct CssCustomProperties {
    mutable Atomic<uint32_t> references;
    const CssCustomProperties* PR_NULLABLE base;   // retained; has no base of its own

    // The sum of a hash of each valid entry of the set merged with its base, with the low bit set. Two sets whose sums
    // differ hold different contents. A set over a base adds its own entries to the base's sum without reading the
    // base's other entries. The sum is computed on first use, because only sets over different bases are compared by
    // it. It is 0 until then.
    mutable Atomic<uint64_t> contents;

    uint32_t key;                   // the hash of the base and the own entries that the table interns the set by
    uint32_t count;
    CssCustomEntry* entries;
    char* PR_NULLABLE text;
    CssStyleTable* PR_NULLABLE owner; // the table that interns the set; the set leaves it when freed
    uint32_t slot;                    // the set's position among the sets of its part of the owner
};

// An entry used to create a set: a name and its value, which the set copies.
struct CssCustomRow {
    uint32_t hash;
    PrStringView name;
    PrStringView value;
    bool invalid;                   // true for a guaranteed-invalid value; `value` is ignored
};

// Returns the hash that orders and finds entries by `name`.
uint32_t cssCustomHash(PrStringView name);

// Returns true if the entry with hash `hashA` and name `a` sorts before the entry with hash `hashB` and name `b`.
bool cssCustomBefore(uint32_t hashA, PrStringView a, uint32_t hashB, PrStringView b);

// Returns the set of `rows` over `base`, interned in `table`. The set is one that the table already holds, or a new
// one, and the caller holds one reference to it. `base` must have no base of its own. The rows are sorted by
// cssCustomBefore and have distinct names. The set retains `base`.
const CssCustomProperties* cssCustomPropertiesCreate(CssStyleTable* table, const CssCustomProperties* PR_NULLABLE base,
    const CssCustomRow* PR_NULLABLE rows, uint32_t count);

// Looks up the value of custom property `name`. Returns false if the set has no valid value for it.
bool cssCustomValue(const CssCustomProperties* PR_NULLABLE custom, PrStringView name, PrStringView* out);

// Returns true if the two sets hold the same names with the same values, however they are layered.
bool cssCustomPropertiesEqual(const CssCustomProperties* PR_NULLABLE a, const CssCustomProperties* PR_NULLABLE b);

// Adds or releases a reference. The last release frees the set and releases its base.
void cssCustomPropertiesRetain(const CssCustomProperties* properties);
void cssCustomPropertiesRelease(const CssCustomProperties* PR_NULLABLE properties);

// Iterates the valid entries of a set merged with its base, in order.
struct CssCustomCursor {
    const CssCustomProperties* PR_NULLABLE own;
    const CssCustomProperties* PR_NULLABLE base;
    uint32_t at;                    // index of the next entry of `own`
    uint32_t baseAt;                // index of the next entry of `base`

    // Writes the next entry to `out`. Returns false once there is none.
    bool next(CssCustomRow* out);
};

// Returns a cursor at the first entry of `custom`.
CssCustomCursor cssCustomCursor(const CssCustomProperties* PR_NULLABLE custom);

// Calls `fn(row)` with each valid entry of the set, merged with its base, in order.
template <typename Fn>
void cssCustomEach(const CssCustomProperties* PR_NULLABLE custom, Fn fn) {
    CssCustomCursor cursor = cssCustomCursor(custom);
    CssCustomRow row{};
    while (cursor.next(&row))
        fn(row);
}

// The ratio of a normal line height to the font size, used for `lh` and `rlh`. It stands in for the normal line height
// of the first available font, because the cascade has no font to measure.
constexpr double kCssNormalLineHeight = 1.2;

struct CssComputedStyle;

// The lengths in px that the font-relative units of one font are relative to (css-values-4/#font-relative-lengths). The
// element's bases apply to em, ex, cap, ch, ic and lh, and the root element's bases to their r-prefixed forms. ex, cap,
// ch and ic read the metrics of the font that `style` sets, measured when a value first reads them (cssFontMetricPx).
struct CssFontBases {
    double em;                      // the computed font size
    double lh;                      // the computed line height

    // The style whose font group and locale choose the font that is measured, or null for no font. With no font, each
    // metric takes its fallback. The bases hold no reference to the style. The bases that a style holds for itself name
    // no style (cssStyleFontBases).
    const CssComputedStyle* PR_NULLABLE style;
};

// Returns the bases of a font of size `em` on lines `lh` tall, measured in the font of `style`.
constexpr CssFontBases cssFontBases(double em, double lh, const CssComputedStyle* PR_NULLABLE style = nullptr) {
    return CssFontBases{em, lh, style};
}

// The metrics of a font that ex, cap, ch and ic read (css-values-4/#font-relative-lengths).
enum {
    CssFontMetric_Ex,               // the x-height of the first available font
    CssFontMetric_Cap,              // the cap height of the first available font
    CssFontMetric_Ch,               // the advance measure of "0" in the font that sets it
    CssFontMetric_Ic,               // the advance measure of U+6C34, the CJK water ideograph, in the font that sets it
    CssFontMetric_Count,
};
typedef uint8_t CssFontMetric;

// How the producer of styles measures fonts. `measure` writes the metric of the font that the font group and locale of
// `style` set, in ems, and returns false if it cannot measure it. An advance is measured down the line if `upright`,
// and along a horizontal line otherwise. `measure` is called on any thread that computes styles.
struct CssFontMeasure {
    bool (*measure)(void* PR_NULLABLE data, const CssComputedStyle* style, CssFontMetric metric, bool upright, double* ems);
    void* PR_NULLABLE data;
};

// The metrics that the values of one style read, and whether a value read them. The first of each pair is for the
// style's own font or its parent's, and the second for the root's font. Each metric is measured once. A style that
// reads no metric measures nothing.
struct CssFontMetrics {
    const CssFontMeasure* PR_NULLABLE measure;
    const CssComputedStyle* PR_NULLABLE sources[2]; // the styles whose fonts are measured
    double ems[2][CssFontMetric_Count];
    uint8_t measured[2];            // by CssFontMetric, the metrics in `ems`
    bool readsFont;                 // a value read a metric of the element's font, or of its parent's
    bool readsRoot;                 // a value read a metric of the root's font
    bool readsGroup;                // a value of the group being computed read either
};

// The longhands whose winning declarations are important, in increasing order. Important declarations override
// animations (css-cascade-5/#cascade-origin), which do not apply to them.
struct CssImportantProperties {
    mutable Atomic<uint32_t> references;
    uint32_t count;
    PrCssProperty properties[];
};

// How an element is rendered under its computed appearance (css-ui-4/#appearance-switching). An element that is not a
// widget is None, and renders as for `appearance: none`.
enum {
    CssAppearance_None,
    CssAppearance_Native,           // a widget's native appearance
    CssAppearance_Primitive,        // a widget's primitive appearance: `appearance: none`, or a devolved widget
    CssAppearance_Base,             // a widget's base appearance (css-forms-1/#appearance)
};
typedef uint8_t CssAppearance;

// An element's computed style. It holds a reference to a group of each kind, its custom properties, the longhands that
// important declarations won, and the locale that its language resolves to. The locale inherits in the same way as the
// properties of an inherited group.
struct CssComputedStyle {
    mutable Atomic<uint32_t> references;
    const CssStyleValues* PR_NONNULL groups[CssStyleGroup_Count];
    const CssCustomProperties* PR_NULLABLE custom;
    const CssImportantProperties* PR_NULLABLE important; // null if no important declaration won
    PrLocale locale;                                     // zero if the language is unknown or does not resolve

    // The used appearance, which the cascade decides from the computed appearance, the kind of widget, and whether the
    // author origin devolves it (css-ui-4/#appearance-disabling-properties). A pseudo-element takes the used appearance
    // of the element or pseudo-element that originates it.
    CssAppearance appearance;

    bool inheritsReset;                                  // a property that does not inherit takes the parent's value,
                                                         // as with `inherit`
    bool staticInline;                                   // absolutely positioned, with a display blockified from an
                                                         // inline-level one

    // The boxes of the element's children are a flex or grid container's items, which blockifies their display
    // (css-display-4/#transformations). This holds if the element's display is flex, inline-flex, grid or inline-grid.
    // It also holds for contents, if the parent's children's boxes are items, because contents puts the element's
    // children among its parent's children (css-display-4/#box-generation).
    bool childrenAreItems;

    // A value read a metric of the element's or its parent's font, or of the root's (CssFontMetrics). A change of the
    // fonts that the style's font may come from restyles such an element.
    bool readsFontMetrics;
    bool readsRootMetrics;

    CssFontBases font;              // the bases that the font group gives (cssComputedStyleSetFontBases)
};

// Sets a style's font bases from its font group. The code that creates a style calls this once the style's groups are
// set.
void cssComputedStyleSetFontBases(CssComputedStyle* style);

// Returns the computed value of `property`, which the engine renders. A flow-relative property has the computed value
// of its physical counterpart, chosen by the style's own writing mode and direction (cssPhysicalProperty).
CssValueRun cssComputedValue(const CssComputedStyle* style, PrCssProperty property);

// Returns the keyword that a computed value begins with, or PrCssKeyword_Count if its first value is no keyword.
inline PrCssKeyword cssComputedFirstKeyword(const CssComputedStyle* style, PrCssProperty property) {
    return cssValueRunFirstKeyword(cssComputedValue(style, property));
}

// Returns the physical property that the flow-relative longhand `property` maps to in an element whose computed
// writing-mode and direction are `writingMode` and `direction` (css-logical-1/#box,
// css-writing-modes-4/#logical-to-physical), or `property` itself if it is not flow-relative. The two properties share
// the physical property's computed value, and inherit in the same way as the physical property (csswg-drafts#3029).
PrCssProperty cssPhysicalProperty(PrCssProperty property, PrCssKeyword writingMode, PrCssKeyword direction);

// Returns true if `property` is a flow-relative longhand that cssPhysicalProperty maps.
bool cssIsFlowRelative(PrCssProperty property);

// Adds or releases a reference. The last release frees the style, and releases its groups, custom properties and
// important properties.
void cssComputedStyleRetain(const CssComputedStyle* style);
void cssComputedStyleRelease(const CssComputedStyle* PR_NULLABLE style);

// Returns true if an important declaration won `property` in the style's cascade.
bool cssStyleIsImportant(const CssComputedStyle* style, PrCssProperty property);

// Creates the list of `count` important longhands in increasing order, with one reference, or retains or releases one.
const CssImportantProperties* cssImportantPropertiesCreate(const PrCssProperty* properties, uint32_t count);
void cssImportantPropertiesRetain(const CssImportantProperties* important);
void cssImportantPropertiesRelease(const CssImportantProperties* PR_NULLABLE important);

// ---------------------------------------------------------------------------------------------------------------------
// Computing values

// The font size of `medium`, which is the initial font size (css-fonts-4/#absolute-size-mapping).
constexpr double kCssMediumFontSize = 16;

// The viewport that a style is computed in. Its extent in px is the base of viewport-relative lengths.
struct CssViewport {
    float width;
    float height;
};

// The bases of the initial font size and line height, which the root's own font-relative lengths are relative to.
constexpr CssFontBases kCssInitialFontBases =
    cssFontBases(kCssMediumFontSize, kCssMediumFontSize * kCssNormalLineHeight);

// The values that a computed value is relative to (css-values-4/#relative-lengths). The font group computes `font-size`
// first, then `line-height`, then the rest. `font` changes as it goes. It holds the parent's bases while font-size
// computes, takes the element's font size before line-height computes, and takes the element's line height after.
struct CssStyleContext {
    CssFontBases font;              // the bases of font-relative lengths
    CssFontBases root;              // the bases of root-relative lengths
    float parentWeight;             // the parent's computed font weight, which bolder and lighter step from
    PrColor parentColor;            // the parent's computed color, which `color: currentcolor` takes
    CssViewport viewport;
    bool* PR_NULLABLE readsViewport;   // set if a value resolves a length relative to the viewport's extent
    bool* PR_NULLABLE readsFont;       // set if a value resolves a length relative to the element's font

    // Measures and records the metrics that ex, cap, ch and ic read, or null to take each metric's fallback.
    CssFontMetrics* PR_NULLABLE metrics;
};

// The parts that a table divides its groups among, chosen by the top bits of each group's hash. Each part has a lock of
// its own, so that threads that intern at the same time seldom wait for each other.
constexpr uint32_t kCssStyleTablePartBits = 5;
constexpr uint32_t kCssStyleTableParts = 1u << kCssStyleTablePartBits;

// One part of a table: its groups, indexed by `index`. Interning or freeing a group of the part holds the part's lock,
// which guards the fields after `mutex`.
struct CssStyleTablePart {
    PrMutex mutex;
    Array<CssStyleValues*> groups;
    HashIndex index;
    Array<uint32_t> free;           // indices of `groups` whose group was freed
};

// One part of a table's custom property sets. It has the same structure as CssStyleTablePart.
struct CssCustomTablePart {
    PrMutex mutex;
    Array<CssCustomProperties*> sets;
    HashIndex index;
    Array<uint32_t> free;
};

// The interned groups and custom property sets, which the builders of several threads share.
struct CssStyleTable {
    const CssStyleValues* PR_NULLABLE initial[CssStyleGroup_Count]; // the group of initial values for each kind
    CssStyleTablePart parts[kCssStyleTableParts];                   // indexed by the top bits of a group's hash
    CssCustomTablePart customParts[kCssStyleTableParts];            // indexed by the top bits of a set's key
};

// Initializes a table that holds the initial groups, or frees what the table holds. The groups and sets that styles
// still hold outlive the table, and belong to no table afterwards. No builder may use a table that is reset.
void cssStyleTableInit(CssStyleTable* table);
void cssStyleTableReset(CssStyleTable* table);

// The number of recently interned groups of each kind that a builder keeps by hash, for the elements of a subtree to
// share.
constexpr uint32_t kCssStyleBuilderRecent = 16;

// The state in which one thread computes groups and interns them in `table`.
struct CssStyleBuilder {
    CssStyleTable* PR_NULLABLE table;
    CssValuePool pool;              // the values of the group being built
    Array<CssValueSpan> spans;      // the runs of those values, by slot

    // Groups that the builder interned recently, by kind, each at the position that its hash names. The builder holds a
    // reference to each. A group that is built again is found here without taking the table's lock. Each kind has slots
    // of its own, so that the groups of one element never evict each other.
    const CssStyleValues* PR_NULLABLE recent[CssStyleGroup_Count][kCssStyleBuilderRecent];

    // How the styles that the builder computes measure fonts, or null to take each metric's fallback.
    const CssFontMeasure* PR_NULLABLE measure;
};

// Frees what the builder holds.
void cssStyleBuilderReset(CssStyleBuilder* builder);

// Returns the group of initial values of the properties of `group`. The caller holds one reference to it.
const CssStyleValues* cssStyleInitial(CssStyleBuilder* builder, CssStyleGroup group);

// Starts a group, whose values the caller adds in slot order.
void cssStyleBegin(CssStyleBuilder* builder);

// Computes `specified` and adds it as the computed value of the group's next property. `specified` is a value of
// `property` as declared, with no CSS-wide keyword or substitution left in it.
void cssStyleAddComputed(CssStyleBuilder* builder, PrCssProperty property, CssValueRun specified, const CssStyleContext& context);

// Appends the computed value of `property` to `pool`, computed in the same way as cssStyleAddComputed.
void cssStyleComputeValue(CssValuePool* pool, PrCssProperty property, CssValueRun specified, const CssStyleContext& context);

// Appends the computed value of `property`'s initial value to `pool` (css-cascade-5/#initial-values). The initial font
// family is the user agent's (css-fonts-4/#font-family-prop).
void cssStyleComputeInitial(CssValuePool* pool, PrCssProperty property, const CssStyleContext& context);

// Adds a value that is already computed, such as a parent's inherited value.
void cssStyleAddValue(CssStyleBuilder* builder, CssValueRun computed);

// Returns the value that the builder added last.
CssValueRun cssStyleLastValue(const CssStyleBuilder* builder);

// Ends the group and returns it interned in the builder's table. The caller holds one reference to it.
const CssStyleValues* cssStyleEnd(CssStyleBuilder* builder, CssStyleGroup group);

// Adds or releases a reference. The last release frees the group, which leaves its table.
void cssStyleValuesRetain(const CssStyleValues* values);
void cssStyleValuesRelease(const CssStyleValues* PR_NULLABLE values);

// Replaces `out` with the families of a computed font-family (css-fonts-4/#font-family-prop), whose names are views of
// `names`, which the call also replaces. A keyword, alone or in generic(), is a generic family. A string or a sequence
// of identifiers names a family, never a generic one.
void cssFontFamilies(CssValueRun run, TextBuffer* names, Array<PrFontFamily>* out);

// Returns what the style's font-variant longhands, font-kerning, font-feature-settings and font-synthesis longhands ask
// of the shaping of its text (css-fonts-4/#feature-variation-precedence), or null if they ask for shaping's defaults.
// The caller owns a reference to the result. The features of the font-variant longhands come before those of
// font-feature-settings. The functions of font-variant-alternates, which name @font-feature-values, ask for nothing.
PrTextFeatureSet* PR_NULLABLE cssFontFeatureSet(const CssComputedStyle* style);

// Returns how the style's text breaks within words: its word-break and overflow-wrap (css-text-4/#word-break-property,
// css-text-4/#overflow-wrap-property).
PrTextBreakStyle cssTextBreakStyle(const CssComputedStyle* style);

// Returns the used font size and weight of the computed values. Other values are computed relative to these and to the
// color (cssStyleColor).
double cssStyleFontSize(const CssComputedStyle* style);
float cssStyleFontWeight(const CssComputedStyle* style);

// Returns the percentage that a font-width keyword names (css-fonts-4/#font-width-prop), or 100 for normal and any
// other keyword.
double cssFontWidthOfKeyword(PrCssKeyword keyword);

// The color schemes that a page supports or that an element's color-scheme names
// (css-color-adjust-1/#color-scheme-prop).
enum {
    CssColorSchemeFlag_Light = 1u << 0,
    CssColorSchemeFlag_Dark = 1u << 1,
};
typedef uint8_t CssColorSchemeFlags;

// What colors resolve against outside an element's style: the display's headroom in stops, which color-hdr() reads
// (css-color-hdr-1/#hdr-color-function); the user's preferred scheme (mediaqueries-5/#prefers-color-scheme); and the
// schemes that the page supports (html/#meta-color-scheme), which an element whose color-scheme is normal uses. A
// resolution sets `readsOutside` if it read the headroom or the preference. A change to the page's schemes is rare, and
// reaches every color. The default environment is SDR, with a light preference and no supported schemes.
struct CssColorEnvironment {
    float headroom;
    bool prefersDark;
    CssColorSchemeFlags pageSchemes;
    bool* PR_NULLABLE readsOutside;
};
constexpr CssColorEnvironment kCssDefaultColorEnvironment = {};

// What a color resolves against besides its environment: the element's color, which currentcolor names; its computed
// style, whose used color scheme light-dark() and the system colors read (css-color-adjust-1/#color-scheme-prop), and
// whose accent color AccentColor follows unless accent-color is auto (css-color-4/#css-system-colors), or null for a
// light scheme and the default accent. The scheme and the accent are read only if a color needs them.
struct CssColorContext {
    PrColor current;
    const CssComputedStyle* PR_NULLABLE style;
    CssColorEnvironment environment;
};

// Returns the computed font-width (css-fonts-4/#font-width-prop), as a percentage of the normal width.
float cssStyleFontWidth(const CssComputedStyle* style);

// Returns true if a property's value is a <color>, which computes to an absolute color where it can.
bool cssIsColorProperty(PrCssProperty property);

// Returns the context that a computed style's colors resolve in, in `environment`, or the used color of the style.
CssColorContext cssStyleColorContext(const CssComputedStyle* style, const CssColorEnvironment& environment);
PrColor cssStyleColor(const CssComputedStyle* style, const CssColorEnvironment& environment);

// Returns true if the element whose style is `style` uses a dark color scheme in `environment`
// (css-color-adjust-1/#color-scheme-prop): its color-scheme names dark and not light, or names both and the user
// prefers dark. An element whose color-scheme is normal uses the page's supported schemes in the same way. A null style
// uses the page's.
bool cssUsesDarkScheme(const CssComputedStyle* PR_NULLABLE style, const CssColorEnvironment& environment);

// Returns the line height in px of a computed `line-height` at `fontSize`. That is a number times the font size, the
// length, or for normal, kCssNormalLineHeight times the font size.
double cssLineHeightPx(CssValueRun computed, double fontSize);

// Returns the line height in px of a computed style, as cssLineHeightPx computes it. `lh` is relative to this height.
double cssStyleLineHeight(const CssComputedStyle* style);

// Returns the bases that a computed style gives font-relative lengths. The style's own bases apply to its children's
// font-size and line-height, and the root's bases apply to every root-relative length.
CssFontBases cssStyleFontBases(const CssComputedStyle* style);

// Returns the length in px of one unit of `metric` in the bases of `context`: the root's if `root`, and the font's
// otherwise. The metric is measured in the bases' font if the context measures fonts. If it cannot be measured, it
// takes its fallback (css-values-4/#font-relative-lengths): 0.5em for ex, 0.7em for cap without a font, 0.5em for ch,
// or 1em for ch set upright, and 1em for ic.
double cssFontMetricPx(const CssStyleContext& context, bool root, CssFontMetric metric);

// Writes the color of the value at `index` of `run` in `context`, in its own space with its missing components, as
// cssResolveColor resolves it before converting it. Returns false for a value that is not a <color>.
bool cssResolveColorComponents(CssValueRun run, uint32_t index, const CssColorContext& context, CssColor* out);

// Returns `color` converted to `space` for interpolation (css-color-4/#interpolation-missing): its missing analogous
// components are carried forward as missing, and a conversion to another space makes a powerless hue missing.
CssColor cssColorConvert(const CssColor& color, PrColorSpace space);

// Returns the color from `a` to `b` by `t` in `space`, with hues taking `hueMethod` (css-color-4/#interpolation). A
// component missing in one color takes the other's, and one missing in both stays missing.
CssColor cssColorInterpolate(const CssColor& a, const CssColor& b, double t, PrColorSpace space,
    PrColorHueMethod hueMethod);

// Returns the extended sRGB of a color, with straight alpha in [0, 1]. A missing component is zero.
PrColor cssColorToSrgb(const CssColor& color);

// Writes the extended sRGB color of the value at `index` of `run` in `context`. The value is a <color>: a color written
// with its components, the keyword of a named or system color, currentcolor, or a function that resolves to a color.
// Returns false for any other value.
bool cssResolveColor(CssValueRun run, uint32_t index, const CssColorContext& context, PrColor* out);

// Writes the length in px of the value at `index` of `run`. The value is a length, or a calculation of lengths and
// percentages of `base`. Returns false for any other value.
bool cssResolveLength(CssValueRun run, uint32_t index, double base, double* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
