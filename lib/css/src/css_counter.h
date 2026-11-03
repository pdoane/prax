// Counter representations: the text that counter(), counters() and a list item's marker write for a counter's value.
//
// css-counter-styles-3 - https://drafts.csswg.org/css-counter-styles-3/
// css-lists-3 - https://drafts.csswg.org/css-lists-3/

#pragma once

#include "pr/array.h"
#include "pr/css_descriptors.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/text_buffer.h"

#include "css/src/css_counter_style.h"
#include "css/src/css_value.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct CssRuleData;

// ---------------------------------------------------------------------------------------------------------------------
// A document's counter styles

// What one of a document's counter styles holds that its rule does not: its name, and the descriptors' lists.
struct CssCounterStyleStorage {
    PrString name;
    Array<PrStringView> symbols;
    Array<CssAdditiveTuple> additive;
    Array<CssCounterRange> ranges;
};

// The counter styles that a document's @counter-style rules define (css-counter-styles-3/#the-counter-style-rule). Of
// the rules of a name, the last wins whole. A style of a predefined style's name takes its place, except for the styles
// that no rule may define. A style that extends another takes the algorithm and the descriptors that it does not set
// from that style, a document's or a predefined one, and the styles of a cycle of `extends` extend decimal. The symbols
// view the text of the rules, which outlives the set.
struct CssCounterStyles {
    Array<CssCounterStyle> styles;
    Array<CssCounterStyleStorage> storage;      // by index of `styles`
};

// Resolves the counter styles that the `count` @counter-style rules `rules` define, in cascade order, into `out`, in
// place of what it held. A rule that defines no style, because its symbols are too few for its system, it extends with
// symbols of its own, or it names a symbol that is an image, which the engine does not draw, is ignored. A calculation
// in a descriptor resolves font-relative lengths against an initial font of size `em`, in px.
void cssCounterStylesBuild(CssCounterStyles* out, const CssRuleData* const PR_NONNULL* PR_NULLABLE rules,
    uint32_t count, double em);

// Returns true if a @counter-style rule may define the style `name`, which is neither none nor a style that no rule may
// define (css-counter-styles-3/#the-counter-style-rule).
bool cssCounterStyleNameDefinable(PrStringView name);

// Returns true if `rule`, a @counter-style rule, with `descriptor` set to `value`, a value of its grammar, still
// defines a counter style, and, for its system, keeps its algorithm. A CSSOM setter does nothing otherwise
// (css-counter-styles-3/#the-csscounterstylerule-interface).
bool cssCounterStyleRuleAccepts(const CssRuleData* rule, PrCssDescriptor descriptor, CssValueRun value);

// Frees what the set holds.
void cssCounterStylesReset(CssCounterStyles* styles);

// Returns the counter style that `name` names: a document's, matched exactly, or else a predefined one, whose names
// match ASCII case-insensitively, as their uses are lowercased (css-counter-styles-3/#typedef-counter-style-name).
// Returns null if none has the name. `styles` may be null, for a document with none.
const CssCounterStyle* PR_NULLABLE cssCounterStyleFind(const CssCounterStyles* PR_NULLABLE styles, PrStringView name);

// ---------------------------------------------------------------------------------------------------------------------
// Representations

// A physical side, toward which a symbol points.
enum {
    CssCounterSide_Right,
    CssCounterSide_Left,
    CssCounterSide_Down,
    CssCounterSide_Up,
};
typedef uint8_t CssCounterSide;

// The writing mode and direction that a representation is in, as the physical sides of its inline end and block end.
struct CssCounterFlow {
    CssCounterSide inlineEnd;
    CssCounterSide blockEnd;
};

// Horizontal left-to-right text, which the flow of a representation is unless it is given.
constexpr CssCounterFlow kCssCounterFlowDefault = CssCounterFlow{CssCounterSide_Right, CssCounterSide_Down};

// The greatest number of code points in a representation. A longer representation uses the fallback style instead,
// which css-counter-styles-3/#generate-a-counter allows, so that a large value of a symbolic or additive style, or a
// large pad, costs no more than this.
constexpr uint32_t kCssCounterMaxCodePoints = 60;

// Appends the representation of `value` in `style` (css-counter-styles-3/#generate-a-counter), without the style's
// prefix and suffix. A value that the style cannot represent uses the style's fallback, found among `styles`, and
// decimal in the end. A symbol that points turns to `flow`, as disclosure-closed's does.
void cssCounterRepresent(PrTextBuffer* out, const CssCounterStyles* PR_NULLABLE styles, const CssCounterStyle* style,
    int32_t value, CssCounterFlow flow);

// Appends the representation of `value` in the counter style that a computed <counter-style> names, as counter() and
// counters() write it: a style's name, among `styles` and the predefined styles, or symbols(). A name that no style has
// is decimal.
void cssCounterAppend(PrTextBuffer* out, const CssCounterStyles* PR_NULLABLE styles, CssValueRun counterStyle,
    int32_t value, CssCounterFlow flow);

// Appends the marker string that a computed list-style-type gives a list item whose list-item counter is `value`
// (css-lists-3/#text-markers): the counter style's prefix, the representation and the style's suffix, or the string
// that the type gives. Appends nothing for none. Returns false if the type is none.
bool cssCounterAppendMarker(PrTextBuffer* out, const CssCounterStyles* PR_NULLABLE styles, CssValueRun listStyleType,
    int32_t value, CssCounterFlow flow);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
