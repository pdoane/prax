// SVG text (SVG11/text.html, which SVG Tiny 1.2 keeps). The characters of a text element are positioned as its elements
// specify, set in the fonts that their families name, and painted as the styles of the elements specify.
//
// A family name refers to the document's SVG fonts before the host's families. Characters are set one run at a time. A
// run is the characters of one element in one font. A character with an absolute position starts a new run and a new
// text chunk, which text-anchor aligns. A run in an SVG font is set glyph by glyph, as SVG fonts require. A run in a
// host font is shaped.
//
// The layout runs when the tree is built, and records what draws the text: runs of glyphs, outlines and decorations,
// each with the paints of the element that it comes from.

#include "svg/src/svg_text.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/locale_common.h"
#include "pr/mem.h"
#include "pr/number_list.h"
#include "pr/path_data.h"
#include "pr/path_measure.h"
#include "pr/string_ops.h"
#include "pr/text_break.h"
#include "pr/text_buffer.h"
#include "pr/text_font.h"
#include "pr/text_itemize.h"
#include "pr/unicode.h"
#include "pr/unicode_bidi.h"
#include "pr/unicode_grapheme.h"
#include "pr/unicode_line_break.h"
#include "pr/utf8.h"
#include "pr/vec.h"

#include "dom/src/dom_element_state.h"
#include "dom/src/dom_node.h"
#include "svg/src/svg_document.h"
#include "svg/src/svg_font.h"

namespace pr_impl {

// A host text font, keyed by the families, the style and the locale that it was made for.
struct SvgHostFont {
    RawArray<char> key;
    PrTextFont* font;
};

// What one build found of a face whose font its document does not keep: a font in a document that the host loads, or
// none, which another build's host may yet load.
struct SvgLoadedFace {
    const SvgFontFace* face;
    const SvgFont* PR_NULLABLE font;
};

struct SvgTextFonts {
    RawArray<SvgLoadedFace> loaded;
    RawArray<SvgHostFont> hosts;
    PrTextGlyphBuffer* PR_NULLABLE glyphs;  // the buffer that shaping fills, created when text first shapes
};

// What draws one run of glyphs or one outline: a run in a host font, or the outlines of a run in an SVG font or of a
// decoration.
struct SvgTextPiece {
    SvgPaints paints;
    PrPathFillRule rule;
    const PrFont* PR_NULLABLE font;     // a run in a host font, which the piece holds a reference to
    float size;
    uint32_t firstGlyph;                // a range of SvgText::glyphs and SvgText::transforms
    uint32_t glyphCount;
    bool transformed;                   // true if the glyphs have transforms about their origins
    PrPath* PR_NULLABLE path;           // outlines, which the piece owns
};

struct SvgText {
    RawArray<SvgPaints> paints;            // the paints of the text's elements, whose strokes the text owns
    RawArray<SvgTextPiece> pieces;         // each paints with a copy of one of `paints`
    RawArray<PrFontGlyph> glyphs;
    RawArray<PrAffine2> transforms;
    PrRect bounds;
    bool clipped;                       // true if the pieces draw inside `clip`, as text-overflow: clip asks
    PrRect clip;
    RawArray<SvgTextCharacter> characters;
    RawArray<SvgTextElement> elements;     // the text element first, then its descendants in tree order
};

namespace {

// How far a text path's flattened curves may lie from the curves, in user units.
constexpr float kTextPathTolerance = 0.05f;

// The most decorations that a span holds. An element that would add one more adds none.
constexpr uint32_t kMaxDecorations = 4;

// Where text aligns along a line or a block: text-anchor, and SVG Tiny 1.2's text-align and display-align.
enum {
    Anchor_Start,
    Anchor_Middle,
    Anchor_End,
};
typedef uint8_t Anchor;

// A baseline of a font's baseline table (css-inline-3/#baseline-types). Auto is the baseline that the script's text
// sits on.
enum {
    Baseline_Auto,
    Baseline_Alphabetic,
    Baseline_Ideographic,
    Baseline_Hanging,
    Baseline_Mathematical,
    Baseline_Central,
    Baseline_Middle,
    Baseline_TextTop,
    Baseline_TextBottom,
};
typedef uint8_t Baseline;

// How baseline-shift moves an element's baseline: by a length, or to the font's subscript or superscript position.
enum {
    Shift_Length,
    Shift_Sub,
    Shift_Super,
};
typedef uint8_t Shift;

// How an element's white space is processed (css-text-4/#white-space-collapsing). The values of white-space-collapse
// that SVG text distinguishes: break-spaces is Preserve, since text that does not wrap has no spaces to break.
enum {
    Collapse_Collapse,          // a run of spaces and tabs is one space, and a segment break is a space among them
    Collapse_Discard,           // white space is removed
    Collapse_Preserve,          // kept, with tabs as spaces and segment breaks as forced line breaks
    Collapse_PreserveBreaks,    // spaces collapse, and segment breaks are forced line breaks
    Collapse_PreserveSpaces,    // kept, with tabs and segment breaks as spaces, as xml:space="preserve" draws them
};
typedef uint8_t Collapse;

// A text decoration that an element declares (css-text-decor-3/#line-decoration). It is painted across the text that
// the element contains, with the paints of the element.
struct Decoration {
    SvgLineFlags lines;
    SvgPaints paints;
};

// A family of a span's font-family list. A named family's name is a range of Layout::familyNames.
struct Family {
    PrFontGeneric generic;
    uint32_t nameStart;
    uint32_t nameSize;
};

// The computed values that text reads from an element.
struct TextStyle {
    SvgPaints paints;
    PrPathFillRule fillRule;
    bool visible;
    SvgPointerEvents pointerEvents;
    float fontSize;
    uint32_t firstFamily;               // a range of Layout::families
    uint32_t familyCount;
    float fontWeight;
    float fontWidth;
    PrFontSlope fontSlope;
    Anchor textAnchor;
    SvgLengthPercentage letterSpacing;            // normal is zero
    SvgLengthPercentage wordSpacing;
    Baseline dominantBaseline;
    Baseline alignmentBaseline;
    bool rightToLeft;
    PrTextBidi unicodeBidi;
    Shift shiftKind;
    SvgLengthPercentage shift;          // for Length: up is positive, and a percentage is of the font size
    Anchor textAlign;                   // SVG Tiny 1.2: where a textArea aligns each line
    Anchor displayAlign;                // SVG Tiny 1.2: where a textArea places its lines; Start for before and auto
    float lineIncrement;                // SVG Tiny 1.2: line-increment, 0 for auto
    Collapse collapse;
    bool wraps;                         // text-wrap-mode lets a line wrap at soft wrap opportunities
    PrTextBreakStyle breaks;            // how a line may break inside its words: word-break and overflow-wrap
    PrTextOrientation orientation;      // text-orientation, which vertical text sets its characters by
    float lineHeight;                   // the used line-height in user units

    // What its font properties ask of shaping (cssFontFeatureSet), which Layout::featureSets holds, or null.
    const PrTextFeatureSet* PR_NULLABLE features;

    // The decorations that the element and its ancestors declare. They apply to the text that the element contains.
    Decoration decorations[kMaxDecorations];
    uint32_t decorationCount;
};

// An element whose characters share a style, or a part of them that a pseudo-element styles: the part on the first line
// of the text, under ::first-line, or the first letter, under ::first-letter (css-pseudo-4/#first-line-inheritance).
struct Span {
    TextStyle style;
    const PrDomElement* element;
    const CssComputedStyle* computed;   // the element's style, or the part's
    bool held;                          // the layout holds a reference to `computed`

    // For a part, the span of the element whose characters it styles; kArrayMissing for an element's own span.
    uint32_t of;

    PrStringView language;              // the language tag, which the glyphs of an SVG font match
    PrLocale locale;                    // the locale that the tag resolves to, which shaping reads
    uint32_t parent;                    // the enclosing span; kArrayMissing for the text element's span
    uint32_t start;                     // the range of the span's characters
    uint32_t end;
};

struct Char {
    uint32_t offset;        // bytes into the text
    uint32_t length;
    uint32_t span;
    float x;                // NaN if no element gives one
    float y;
    float dx;
    float dy;
    float rotate;           // degrees, NaN if none
    uint8_t level;          // the bidirectional embedding level
    PrScript script;        // with Common and Inherited resolved from their neighbors
    SvgArabicForm form;     // the joining form, which selects the glyphs of an SVG font
    bool hidden;            // takes no space and draws nothing, like a line break's character
    bool collapsible;       // a space that collapses with those around it, and is trimmed at the end of a line
    bool forced;            // a forced line break, which ends its line and takes no space
    bool removed;           // trimmed from the end of a line, and dropped once the characters are collected
    bool unaddressable;     // a soft hyphen or a bidi control, which layout discards
    bool upright;           // set upright in vertical text, rather than turned on its side
};

// The characters of an element that has position lists. The lists address the characters starting from the first.
struct Positioned {
    const PrDomElement* element;
    uint32_t span;
    uint32_t start;
    uint32_t end;
};

// The font that a character is set in: one of the document's SVG fonts or a host font.
struct Font {
    const SvgFont* PR_NULLABLE svg;
    const PrFont* PR_NULLABLE host;

    bool operator==(const Font& other) const { return svg == other.svg && host == other.host; }
};

struct Glyph {
    uint32_t id;
    PrVec2 position;

    // Where the glyph starts on its line: the pen before it, on the baseline that its run is shifted to. Text is
    // measured along the line from here, which differs from the glyph's position by the shaper's offset, and for an
    // upright glyph by its vertical origin.
    PrVec2 pen;

    float rotate;           // degrees
    float advance;          // user units
    uint32_t chunk;
    uint32_t character;     // the first character that the glyph draws
    float scale;            // its em relative to the font size; one unless the shaper synthesized it
    float stretch;          // the horizontal stretch that lengthAdjust applies
    bool hidden;            // off the end of its text path
    bool upright;           // turned back a quarter turn against the line, so that it stands upright on a vertical one
    bool onPath;            // placed on a text path, in the space of the page rather than of its line
    float align;            // the part of the pen's shift that aligns the run's baseline (Run::align)

    // The text path whose method="stretch" bends the glyph's outline onto it, or kArrayMissing. Such a glyph keeps its
    // position on the line, which recordRun bends with its outline.
    uint32_t warp;
};

// Characters of one span in one font, which are set together.
struct Run {
    uint32_t firstChar;
    uint32_t endChar;
    Font font;
    bool upright;

    // How far down the run's glyphs move to align a baseline of their font with the line's, which the positions of its
    // characters leave out (SVG2/text.html#__svg__SVGTextContentElement__getStartPositionOfChar).
    float align;

    uint32_t firstGlyph;
    uint32_t endGlyph;
};

// A glyph that a run in an SVG font sets, before it is placed.
struct SvgRunGlyph {
    uint32_t id;
    uint32_t character;     // the first and last characters that the glyph draws
    uint32_t last;
};

// A textPath: the path that its characters follow, and which characters they are (SVG2/text.html#TextPathElement).
struct TextPath {
    const PrDomElement* element;
    PrPathMeasure* PR_NULLABLE measure; // measures the path, in the user space of the text, or null if it has none
    float length;                       // the length of every contour, end to end
    float offset;                       // startOffset, a distance along the path
    bool closed;                        // the path is one closed subpath, which the text goes round once
    bool right;                         // side="right", which reverses the path
    bool stretch;                       // method="stretch"
    uint32_t start;                     // the range of the textPath's characters
    uint32_t end;
};

struct Layout {
    SvgBuilder* b;
    SvgDocumentFonts* documentFonts;    // the SVG fonts of the text's document
    bool instance;                      // the text is in a use element's shadow tree
    RawArray<char> text;
    RawArray<TextPath> paths;
    RawArray<Span> spans;
    RawArray<Family> families;
    PrTextBuffer familyNames = {};
    RawArray<Char> chars;
    RawArray<Positioned> positioned;
    RawArray<Run> runs;
    RawArray<Glyph> glyphs;
    RawArray<float> values;
    RawArray<SvgRunGlyph> svgGlyphs;
    RawArray<PrTextFeatureSet*> featureSets; // what the spans' font properties ask of shaping (TextStyle::features)
    RawArray<PrTextPiece> pieces;            // the characters in the form that itemization reads
    PrTextItems* PR_NULLABLE items;
    RawArray<uint32_t> codePoints;
    RawArray<uint8_t> levels;
    RawArray<uint32_t> order;
    RawArray<uint32_t> spanStack;
    bool area;                          // a textArea's layout, whose tbreak elements break lines

    // Vertical text is laid out as horizontal text in a frame turned a quarter turn, which `toPage` turns back once the
    // text is recorded: the line runs along the frame's x-axis, down the page, or up it for sideways-lr, whose frame
    // turns the other way.
    bool vertical;
    float turn;                         // the frame's turn onto the page: 1 a quarter turn clockwise, -1 counter
    bool stacksRight;                   // vertical-lr: lines stack to the right, toward -y in the frame

    // The SVG font and glyph that were set last. The next glyph set in the same font kerns against that glyph, even in
    // another element.
    const SvgFont* PR_NULLABLE kernFont;
    uint32_t kernGlyph;

    SvgText* out;
    SvgNodeIndex node;                  // the text's node, which glyphs' content goes under
};

bool isSpace(uint32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Returns the quarter turn from the frame of vertical text to the page, and back
// (css-writing-modes-4/#logical-to-physical).
PrAffine2 toPageOf(const Layout& t) {
    return PrAffine2{0, t.turn, -t.turn, 0, 0, 0};
}

PrAffine2 fromPageOf(const Layout& t) {
    return PrAffine2{0, -t.turn, t.turn, 0, 0, 0};
}

const SvgBuild& buildOf(const Layout& t) {
    return *t.b->build;
}

// Returns the style of a child of the text, with the parent's computed style `parent`. The caller holds a reference if
// the text is an instance.
const CssComputedStyle* PR_NULLABLE styleOf(Layout& t, const PrDomElement* e, const CssComputedStyle* parent) {
    return t.instance ? svgBuildStyleUnder(*t.b, e, parent) : svgBuildStyle(*t.b, e);
}

Anchor anchorOf(PrCssKeyword keyword) {
    return keyword == PrCssKeyword_Middle ? Anchor_Middle : keyword == PrCssKeyword_End ? Anchor_End : Anchor_Start;
}

Baseline baselineOf(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_Alphabetic: return Baseline_Alphabetic;
        case PrCssKeyword_Ideographic: return Baseline_Ideographic;
        case PrCssKeyword_Hanging: return Baseline_Hanging;
        case PrCssKeyword_Mathematical: return Baseline_Mathematical;
        case PrCssKeyword_Central: return Baseline_Central;
        case PrCssKeyword_Middle: return Baseline_Middle;
        case PrCssKeyword_TextTop:
        case PrCssKeyword_TextBeforeEdge:
        case PrCssKeyword_BeforeEdge: return Baseline_TextTop;
        case PrCssKeyword_TextBottom:
        case PrCssKeyword_TextAfterEdge:
        case PrCssKeyword_AfterEdge: return Baseline_TextBottom;
        default: return Baseline_Auto;
    }
}

PrTextBidi bidiOf(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_Embed: return PrTextBidi_Embed;
        case PrCssKeyword_Isolate: return PrTextBidi_Isolate;
        case PrCssKeyword_BidiOverride: return PrTextBidi_BidiOverride;
        case PrCssKeyword_IsolateOverride: return PrTextBidi_IsolateOverride;
        case PrCssKeyword_Plaintext: return PrTextBidi_Plaintext;
        default: return PrTextBidi_Normal;
    }
}

// Returns SVG Tiny 1.2's text-align or display-align that the element gives, or `inherited` if it gives neither of its
// values or `inherit` (SVGTiny12/text.html#TextAlignProperty). These are attributes, which no CSS property has.
Anchor alignOf(const PrDomElement* e, PrStringView name, Anchor inherited) {
    PrStringView text = svgAttribute(e, name);
    if (text == "start" || text == "before" || text == "auto")
        return Anchor_Start;
    if (text == "center")
        return Anchor_Middle;
    if (text == "end" || text == "after")
        return Anchor_End;
    return inherited;
}

// Returns SVG Tiny 1.2's line-increment that the element gives (SVGTiny12/text.html#LineIncrementProperty), 0 for auto,
// or `inherited`.
float lineIncrementOf(const PrDomElement* e, float inherited) {
    PrStringView text = svgAttribute(e, "line-increment");
    if (text == "auto")
        return 0;
    size_t at = 0;
    float increment = 0;
    if (text.data != nullptr && numberListRead(text, &at, &increment) && at == text.size && increment >= 0)
        return increment;
    return inherited;
}

// Adds a decoration of `lines` with `paints` to the style, unless it holds as many as it can.
void addDecoration(TextStyle& style, SvgLineFlags lines, const SvgPaints& paints) {
    if (lines != 0 && style.decorationCount < kMaxDecorations)
        style.decorations[style.decorationCount++] = Decoration{lines, paints};
}

// Returns the text style of an element, whose parent span's style is `parent`, or which starts the text if `parent` is
// null. The families are appended to the layout's.
TextStyle textStyleOf(Layout& t, const PrDomElement* e, const CssComputedStyle* style, const TextStyle* PR_NULLABLE parent) {
    TextStyle s{};
    if (parent != nullptr) {
        s.textAlign = parent->textAlign;
        s.displayAlign = parent->displayAlign;
        s.lineIncrement = parent->lineIncrement;
        s.decorationCount = parent->decorationCount;
        for (uint32_t i = 0; i < parent->decorationCount; ++i)
            s.decorations[i] = parent->decorations[i];
    }
    s.paints = svgBuildPaints(*t.b, style, e);
    t.out->paints.push(s.paints);
    s.fillRule = svgStyleFillRule(style, PrCssProperty_FillRule);
    PrCssKeyword visibility = svgStyleKeyword(style, PrCssProperty_Visibility);
    s.visible = visibility != PrCssKeyword_Hidden && visibility != PrCssKeyword_Collapse;
    s.pointerEvents = svgStylePointerEvents(style);
    s.fontSize = float(cssStyleFontSize(style));
    s.fontWeight = cssStyleFontWeight(style);
    s.fontWidth = cssStyleFontWidth(style);
    PrCssKeyword slope = svgStyleKeyword(style, PrCssProperty_FontStyle);
    if (slope == PrCssKeyword_Italic)
        s.fontSlope = PrFontSlope_Italic;
    else if (slope == PrCssKeyword_Oblique)
        s.fontSlope = PrFontSlope_Oblique;
    else
        s.fontSlope = PrFontSlope_Normal;
    s.textAnchor = anchorOf(svgStyleKeyword(style, PrCssProperty_TextAnchor));
    s.letterSpacing = svgStyleLength(style, PrCssProperty_LetterSpacing);
    s.wordSpacing = svgStyleLength(style, PrCssProperty_WordSpacing);
    s.dominantBaseline = baselineOf(svgStyleKeyword(style, PrCssProperty_DominantBaseline));
    s.alignmentBaseline = baselineOf(svgStyleKeyword(style, PrCssProperty_AlignmentBaseline));
    s.rightToLeft = svgStyleIs(style, PrCssProperty_Direction, PrCssKeyword_Rtl);
    s.unicodeBidi = bidiOf(svgStyleKeyword(style, PrCssProperty_UnicodeBidi));
    PrCssKeyword shift = svgStyleKeyword(style, PrCssProperty_BaselineShift);
    s.shiftKind = shift == PrCssKeyword_Sub ? Shift_Sub : shift == PrCssKeyword_Super ? Shift_Super : Shift_Length;
    s.shift = svgStyleLength(style, PrCssProperty_BaselineShift);
    s.textAlign = alignOf(e, "text-align", s.textAlign);
    s.displayAlign = alignOf(e, "display-align", s.displayAlign);
    s.lineIncrement = lineIncrementOf(e, s.lineIncrement);
    switch (svgStyleKeyword(style, PrCssProperty_WhiteSpaceCollapse)) {
        case PrCssKeyword_Discard: s.collapse = Collapse_Discard; break;
        case PrCssKeyword_Preserve:
        case PrCssKeyword_BreakSpaces: s.collapse = Collapse_Preserve; break;
        case PrCssKeyword_PreserveBreaks: s.collapse = Collapse_PreserveBreaks; break;
        case PrCssKeyword_PreserveSpaces: s.collapse = Collapse_PreserveSpaces; break;
        default: s.collapse = Collapse_Collapse; break;
    }
    s.wraps = !svgStyleIs(style, PrCssProperty_TextWrapMode, PrCssKeyword_Nowrap);
    s.breaks = cssTextBreakStyle(style);
    PrCssKeyword orientation = svgStyleKeyword(style, PrCssProperty_TextOrientation);
    if (orientation == PrCssKeyword_Upright)
        s.orientation = PrTextOrientation_Upright;
    else if (orientation == PrCssKeyword_Sideways)
        s.orientation = PrTextOrientation_Sideways;
    else
        s.orientation = PrTextOrientation_Mixed;
    s.lineHeight = float(cssStyleLineHeight(style));
    PrTextFeatureSet* features = cssFontFeatureSet(style);
    if (features != nullptr)
        t.featureSets.push(features);
    s.features = features;
    // A decoration is painted with the fill and stroke of the element that declares it
    // (SVG11/text.html#TextDecorationProperties).
    addDecoration(s, svgStyleLines(style), s.paints);
    PrTextBuffer names{};
    RawArray<PrFontFamily> families;
    cssFontFamilies(cssComputedValue(style, PrCssProperty_FontFamily), &names, &families);
    s.firstFamily = t.families.count;
    s.familyCount = families.count;
    for (uint32_t i = 0; i < families.count; ++i) {
        const PrFontFamily& family = families.data[i];
        Family own{family.generic, t.familyNames.size, uint32_t(family.name.size)};
        prTextBufferAppend(&t.familyNames, family.name);
        t.families.push(own);
    }
    families.reset();
    prTextBufferReset(&names);
    return s;
}

// Returns the name of a family.
PrStringView familyName(const Layout& t, const Family& family) {
    return PrStringView{t.familyNames.data + family.nameStart, family.nameSize};
}

void pushChar(Layout& t, uint32_t codePoint, uint32_t span) {
    char bytes[kPrUtf8MaxBytes];
    uint32_t length = prUtf8Encode(codePoint, bytes);
    Char c{};
    c.offset = t.text.count;
    c.length = length;
    c.span = span;
    c.x = c.y = c.rotate = kPrF32Nan;
    for (uint32_t i = 0; i < length; ++i)
        t.text.push(bytes[i]);
    t.chars.push(c);
}

// Returns the last character collected that is not removed, or null.
Char* PR_NULLABLE lastKept(Layout& t) {
    for (uint32_t i = t.chars.count; i-- > 0;) {
        if (!t.chars.data[i].removed)
            return &t.chars.data[i];
    }
    return nullptr;
}

// Removes the collapsible spaces that end the line so far (css-text-4/#white-space-phase-2).
void trimLine(Layout& t) {
    for (uint32_t i = t.chars.count; i-- > 0 && (t.chars.data[i].removed || t.chars.data[i].collapsible);)
        t.chars.data[i].removed = true;
}

// Appends a collapsible space, unless it collapses: after another collapsible space, of whichever element, or at the
// start of a line. Of a run of such spaces, the first is kept, with its own element.
void pushCollapsible(Layout& t, uint32_t span) {
    const Char* last = lastKept(t);
    if (last == nullptr || last->collapsible || last->forced)
        return;
    pushChar(t, ' ', span);
    t.chars.last().collapsible = true;
}

// Appends a forced line break, which trims the line that it ends.
void pushBreak(Layout& t, uint32_t codePoint, uint32_t span) {
    trimLine(t);
    pushChar(t, codePoint, span);
    t.chars.last().forced = true;
    t.chars.last().hidden = true;
}

// Returns true if layout discards the character, as it does a soft hyphen or a bidi control, which therefore is not
// addressable (SVG2/text.html#TermAddressableCharacter). A bidi control still orders the text around it.
bool isDiscarded(uint32_t c) {
    return c == 0xad || c == 0x61c || c == 0x200e || c == 0x200f || (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069);
}

// Appends a text node's characters, with its white space processed as the span's white-space-collapse says
// (css-text-4/#white-space-phase-1). A carriage return and the line feed after it are one segment break.
void appendText(Layout& t, PrStringView data, uint32_t span) {
    Collapse collapse = t.spans.data[span].style.collapse;
    for (size_t pos = 0; pos < data.size;) {
        uint32_t c = 0;
        pos += prUtf8Decode(data, pos, &c);
        bool segment = c == '\n' || c == '\r';
        if (c == '\r' && pos < data.size && data.data[pos] == '\n')
            ++pos;
        if (!segment && c != ' ' && c != '\t') {
            pushChar(t, c, span);
            t.chars.last().unaddressable = isDiscarded(c);
            continue;
        }
        switch (collapse) {
            case Collapse_Discard: break;
            case Collapse_PreserveSpaces: pushChar(t, ' ', span); break;
            case Collapse_Preserve:
                if (segment)
                    pushBreak(t, '\n', span);
                else
                    pushChar(t, ' ', span);
                break;
            case Collapse_PreserveBreaks:
                if (segment)
                    pushBreak(t, '\n', span);
                else
                    pushCollapsible(t, span);
                break;
            default: pushCollapsible(t, span); break;
        }
    }
}

bool hasPositions(const PrDomElement* e) {
    return svgAttribute(e, "x").data != nullptr || svgAttribute(e, "y").data != nullptr ||
        svgAttribute(e, "dx").data != nullptr || svgAttribute(e, "dy").data != nullptr ||
        svgAttribute(e, "rotate").data != nullptr || svgAttribute(e, "textLength").data != nullptr;
}

void collect(Layout& t, const PrDomElement* element, uint32_t span) {
    for (const PrDomNode* child = element->first; child != nullptr; child = child->next) {
        if (domIsText(child)) {
            const PrDomCharacterData* text = static_cast<const PrDomCharacterData*>(child);
            appendText(t, prRawStringView(&text->data), span);
            continue;
        }
        const PrDomElement* e = domElementFromNode(child);
        // A textArea's tbreak is a line separator, which always breaks the line (SVGTiny12/text.html#tbreakElement).
        if (e != nullptr && t.area && svgIs(e, PrDomTag_Tbreak)) {
            pushBreak(t, 0x2028, span);
            continue;
        }
        if (e == nullptr || !(svgIs(e, PrDomTag_Tspan) || svgIs(e, PrDomTag_A) || svgIs(e, PrDomTag_TextPath)))
            continue;
        const CssComputedStyle* style = styleOf(t, e, t.spans.data[span].computed);
        if (style == nullptr)
            continue;
        if (svgStyleIs(style, PrCssProperty_Display, PrCssKeyword_None)) {
            if (t.instance)
                cssComputedStyleRelease(style);
            continue;
        }
        Span inner{};
        TextStyle parentStyle = t.spans.data[span].style;
        inner.style = textStyleOf(t, e, style, &parentStyle);
        inner.element = e;
        inner.computed = style;
        inner.held = t.instance;
        inner.of = kArrayMissing;
        inner.language = domElementLanguage(e);
        PrStringView own = domElementOwnLanguage(e);
        inner.locale = own.data != nullptr ? prLocaleFromTag(own) : t.spans.data[span].locale;
        inner.parent = span;
        inner.start = t.chars.count;
        uint32_t index = t.spans.count;
        t.spans.push(inner);
        uint32_t positioned = kArrayMissing;
        if (hasPositions(e)) {
            positioned = t.positioned.count;
            t.positioned.push(Positioned{e, index, t.chars.count, 0});
        }
        uint32_t path = kArrayMissing;
        if (svgIs(e, PrDomTag_TextPath)) {
            path = t.paths.count;
            t.paths.push(TextPath{.element = e, .start = t.chars.count});
        }
        collect(t, e, index);
        if (positioned != kArrayMissing)
            t.positioned.data[positioned].end = t.chars.count;
        if (path != kArrayMissing)
            t.paths.data[path].end = t.chars.count;
        t.spans.data[index].end = t.chars.count;
    }
}

float percentBase(const Layout& t, SvgAxis axis) {
    PrVec2 v = t.b->viewport;
    return axis == SvgAxis_X ? v.x : axis == SvgAxis_Y ? v.y : prF32Sqrt((v.x * v.x + v.y * v.y) / 2);
}

// Resolves a list of lengths against the axis, into `t.values`.
void lengthList(Layout& t, const PrDomElement* e, PrStringView name, SvgAxis axis, const CssComputedStyle* style) {
    t.values.clear();
    PrStringView list = svgAttribute(e, name);
    size_t start = 0;
    for (size_t i = 0; i <= list.size; ++i) {
        if (i < list.size && list.data[i] != ',' && !isSpace(uint8_t(list.data[i])))
            continue;
        if (i > start) {
            SvgLengthPercentage length{};
            if (!svgParseLength(PrStringView{list.data + start, i - start}, style, &length))
                return;
            t.values.push(svgLengthResolve(length, percentBase(t, axis)));
        }
        start = i + 1;
    }
}

// Gives each character the positions that the lists of its elements specify. The list of an inner element overrides the
// list of an outer one.
void applyPositions(Layout& t) {
    for (uint32_t p = 0; p < t.positioned.count; ++p) {
        const Positioned& at = t.positioned.data[p];
        const CssComputedStyle* style = t.spans.data[at.span].computed;
        // The lists address the element's addressable characters.
        auto each = [&](PrStringView name, SvgAxis axis, float Char::*field) {
            lengthList(t, at.element, name, axis, style);
            for (uint32_t i = at.start, n = 0; i < at.end; ++i) {
                if (t.chars.data[i].unaddressable)
                    continue;
                if (n < t.values.count)
                    t.chars.data[i].*field = t.values.data[n];
                ++n;
            }
        };
        each("x", SvgAxis_X, &Char::x);
        each("y", SvgAxis_Y, &Char::y);
        each("dx", SvgAxis_X, &Char::dx);
        each("dy", SvgAxis_Y, &Char::dy);
        // The last rotation continues to the element's characters beyond the list
        // (SVG11/text.html#TSpanElementRotateAttribute).
        t.values.clear();
        svgParseNumbers(svgAttribute(at.element, "rotate"), &t.values);
        for (uint32_t i = at.start, n = 0; i < at.end && t.values.count != 0; ++i) {
            if (t.chars.data[i].unaddressable)
                continue;
            t.chars.data[i].rotate = t.values.data[n < t.values.count ? n : t.values.count - 1];
            ++n;
        }
    }
}

SvgTextFonts& fontsOf(Layout& t) {
    if (t.b->fonts == nullptr)
        t.b->fonts = allocZeroed<SvgTextFonts>();
    return *t.b->fonts;
}

// Returns the font element that a font-face-uri names, in this document or in one that the host loads.
const PrDomElement* PR_NULLABLE fontOfUri(Layout& t, const PrDomElement* uri) {
    PrStringView href = svgHref(uri);
    if (href.data == nullptr)
        return nullptr;
    size_t hash = 0;
    while (hash < href.size && href.data[hash] != '#')
        ++hash;
    PrStringView file{href.data, hash};
    PrStringView id = hash < href.size ? PrStringView{href.data + hash + 1, href.size - hash - 1} : PrStringView{};
    const PrDomNode* document = uri->document;
    const PrSvgDrawOptions& options = *buildOf(t).options;
    if (file.size != 0) {
        if (options.load == nullptr)
            return nullptr;
        PrDomDocument* loaded = options.load(options.loadContext, uri->document, file);
        if (loaded == nullptr)
            return nullptr;
        document = loaded;
    }
    if (id.size != 0) {
        const PrDomElement* found = svgFindById(document, id);
        return found != nullptr && svgIs(found, PrDomTag_Font) ? found : nullptr;
    }
    PrDomNode* root = const_cast<PrDomNode*>(document);
    for (PrDomNode* node = domNodeNext(root, root); node != nullptr; node = domNodeNext(root, node)) {
        if (const PrDomElement* e = domElementFromNode(node); e != nullptr && svgIs(e, PrDomTag_Font))
            return e;
    }
    return nullptr;
}

// Returns the SVG font of a face: the font that contains the face, or the font that its font-face-uri names
// (SVG11/fonts.html#FontFaceElement). A font is kept by the fonts of its own document, and a face keeps only one of its
// own document's.
const SvgFont* PR_NULLABLE fontOfFace(Layout& t, SvgDocumentFonts* documentFonts, SvgFontFace& face) {
    if (face.resolved)
        return face.font;
    SvgTextFonts& fonts = fontsOf(t);
    for (uint32_t i = 0; i < fonts.loaded.count; ++i) {
        if (fonts.loaded.data[i].face == &face)
            return fonts.loaded.data[i].font;
    }
    const PrDomElement* font = nullptr;
    const PrDomElement* parent = domElementFromNode(face.element->parent);
    if (parent != nullptr && svgIs(parent, PrDomTag_Font))
        font = parent;
    for (const PrDomNode* src = face.element->first; src != nullptr && font == nullptr; src = src->next) {
        const PrDomElement* s = domElementFromNode(src);
        if (s == nullptr || !svgIs(s, PrDomTag_FontFaceSrc))
            continue;
        for (const PrDomNode* uri = s->first; uri != nullptr && font == nullptr; uri = uri->next) {
            const PrDomElement* u = domElementFromNode(uri);
            if (u != nullptr && svgIs(u, PrDomTag_FontFaceUri))
                font = fontOfUri(t, u);
        }
    }
    if (font == nullptr || font->document != face.element->document) {
        const SvgFont* loaded = font != nullptr ? svgDocumentFontsLoad(svgDocumentFonts(font->document), font) : nullptr;
        fonts.loaded.push(SvgLoadedFace{&face, loaded});
        return loaded;
    }
    face.resolved = true;
    face.font = svgDocumentFontsLoad(documentFonts, font);
    return face.font;
}

// Returns the host's text font for the span's style. Each text font is created once per build.
PrTextFont* PR_NULLABLE hostFonts(Layout& t, const Span& span) {
    const PrSvgDrawOptions& options = *buildOf(t).options;
    if (options.fonts == nullptr || options.shaper == nullptr)
        return nullptr;
    const TextStyle& style = span.style;
    RawArray<char> key;
    auto append = [&](const void* bytes, size_t size) { key.append(static_cast<const char*>(bytes), uint32_t(size)); };
    for (uint32_t i = 0; i < style.familyCount; ++i) {
        const Family& family = t.families.data[style.firstFamily + i];
        PrStringView name = familyName(t, family);
        append(&family.generic, sizeof(family.generic));
        append(&family.nameSize, sizeof(family.nameSize));
        append(name.data, name.size);
    }
    float values[] = {style.fontWeight, style.fontWidth, style.fontSize};
    append(values, sizeof(values));
    append(&style.fontSlope, sizeof(style.fontSlope));
    append(&span.locale, sizeof(span.locale));
    SvgTextFonts& fonts = fontsOf(t);
    for (uint32_t i = 0; i < fonts.hosts.count; ++i) {
        const SvgHostFont& host = fonts.hosts.data[i];
        if (host.key.count == key.count && prMemEquals(host.key.data, key.data, key.count)) {
            key.reset();
            return host.font;
        }
    }
    RawArray<PrFontFamily> families;
    for (uint32_t i = 0; i < style.familyCount; ++i) {
        const Family& family = t.families.data[style.firstFamily + i];
        if (family.generic != PrFontGeneric_None)
            families.push(prFontFamilyGeneric(family.generic));
        else
            families.push(prFontFamilyFromName(familyName(t, family)));
    }
    PrFontStyle font = prFontStyleDefault();
    font.weight = style.fontWeight;
    font.width = style.fontWidth;
    font.slope = style.fontSlope;
    font.size = style.fontSize;
    PrTextFontDesc desc{
        .set = options.fonts,
        .families = families.data,
        .familyCount = families.count,
        .style = font,
        .locale = span.locale,
    };
    PrTextFont* text = prTextFontCreate(&desc);
    families.reset();
    fonts.hosts.push(SvgHostFont{key, text});
    return text;
}

bool rangesCover(const PrFontCodePointRange* PR_NULLABLE ranges, uint32_t count, uint32_t codePoint) {
    if (count == 0)
        return true;
    for (uint32_t i = 0; i < count; ++i) {
        if (codePoint >= ranges[i].first && codePoint <= ranges[i].last)
            return true;
    }
    return false;
}

// Returns the font for the cluster that starts at the character (css-fonts-4/#font-matching-algorithm). The document's
// fonts come before the host's, so the result is the first SVG font named by the span's families that has a glyph for
// the character. Otherwise it is the host's face for the cluster (prTextFontMatch), which decides what sets a cluster
// that no font serves. If the host has no face, the result is the first SVG font named, or none.
Font fontFor(Layout& t, const Char& c, const Span& span, PrStringView cluster) {
    PrStringView text{t.text.data, t.text.count};
    uint32_t codePoint = 0;
    prUtf8Decode(text, c.offset, &codePoint);
    Font first{};
    SvgDocumentFonts* documentFonts = t.documentFonts;
    for (uint32_t f = 0; f < span.style.familyCount; ++f) {
        const Family& family = t.families.data[span.style.firstFamily + f];
        if (family.generic != PrFontGeneric_None)
            continue;
        PrStringView name = familyName(t, family);
        uint32_t hash = svgFontFamilyHash(name);
        RawArray<uint32_t>& reads = t.b->tree->fontFamilies;
        if (reads.find(hash) == kArrayMissing)
            reads.push(hash);
        for (uint32_t i = 0; i < documentFonts->faces.count; ++i) {
            SvgFontFace& face = documentFonts->faces.data[i];
            if (face.familyHash != hash || !prAsciiEqualsFold(prRawStringView(&face.family), name))
                continue;
            const SvgFont* font = fontOfFace(t, documentFonts, face);
            if (font == nullptr)
                continue;
            if (first.svg == nullptr)
                first = Font{font, nullptr};
            size_t length = 0;
            if (rangesCover(face.ranges.data, face.ranges.count, codePoint) &&
                    svgFontMatch(*font, text, c.offset, span.language, SvgArabicForm_Any, &length) != kSvgMissingGlyph)
                return Font{font, nullptr};
        }
    }
    PrTextFont* PR_NULLABLE host = hostFonts(t, span);
    if (host != nullptr) {
        uint32_t face = prTextFontMatch(host, cluster, c.script);
        if (face != kPrTextMatchNone)
            return Font{nullptr, prTextFontMatchedFont(host, face)};
    }
    return first;
}

bool isAbsolute(const Char& c) {
    return !prF32IsNan(c.x) || !prF32IsNan(c.y);
}

// Moves the pen by the character's relative position. An absolute position is applied when the character's chunk
// starts.
void placeChar(const Char& c, PrVec2* pen) {
    pen->x += c.dx;
    pen->y += c.dy;
}

// Returns the spacing after a character: letter-spacing, plus word-spacing after a space. A percentage is of the font
// size (css-text-4/#letter-spacing-property).
float spacingAfter(const Layout& t, const Char& c, const TextStyle& style) {
    float spacing = svgLengthResolve(style.letterSpacing, style.fontSize);
    if (c.length == 1 && t.text.data[c.offset] == ' ')
        spacing += svgLengthResolve(style.wordSpacing, style.fontSize);
    return spacing;
}

// Sets a run in an SVG font, one glyph at a time (SVG11/fonts.html#GlyphElement). From each character, in logical
// order, it takes the first glyph that matches the text. It then places the glyphs in visual order, each kerned against
// the glyph placed before it.
void setSvgRun(Layout& t, Run& run, PrVec2* pen, uint32_t chunk) {
    const SvgFont& font = *run.font.svg;
    const Span& span = t.spans.data[t.chars.data[run.firstChar].span];
    float scale = span.style.fontSize / font.unitsPerEm;
    const Char& last = t.chars.data[run.endChar - 1];
    PrStringView text{t.text.data, last.offset + last.length};
    t.svgGlyphs.clear();
    for (uint32_t ci = run.firstChar; ci < run.endChar;) {
        const Char& c = t.chars.data[ci];
        size_t length = 0;
        uint32_t id = svgFontMatch(font, text, c.offset, span.language, c.form, &length);
        uint32_t first = ci;
        // The characters that a ligature covers have no positions of their own.
        uint32_t end = c.offset + uint32_t(length);
        while (ci < run.endChar && t.chars.data[ci].offset < end)
            ++ci;
        t.svgGlyphs.push(SvgRunGlyph{id, first, ci - 1});
    }
    bool reversed = (t.chars.data[run.firstChar].level & 1) != 0;
    uint32_t previous = t.kernFont == &font && !run.upright ? t.kernGlyph : kArrayMissing;
    for (uint32_t k = 0; k < t.svgGlyphs.count; ++k) {
        const SvgRunGlyph& g = t.svgGlyphs.data[reversed ? t.svgGlyphs.count - 1 - k : k];
        const Char& c = t.chars.data[g.character];
        placeChar(c, pen);
        if (previous != kArrayMissing)
            pen->x -= svgFontKerning(font, previous, g.id) * scale;
        previous = run.upright ? kArrayMissing : g.id;
        float rotate = prF32IsNan(c.rotate) ? 0 : c.rotate;
        Glyph glyph{
            .id = g.id,
            .position = *pen,
            .pen = *pen,
            .rotate = rotate,
            .advance = font.glyphs.data[g.id].advance * scale,
            .chunk = chunk,
            .character = g.character,
            .scale = 1,
            .stretch = 1,
            .warp = kArrayMissing,
        };
        // An upright glyph advances by the em down the line, and hangs from its vertical origin, which is half its
        // advance across and its ascent down (SVG11/fonts.html#FontElementVertOriginXAttribute).
        if (run.upright) {
            glyph.position = PrVec2{pen->x + font.ascent * scale, pen->y + glyph.advance / 2};
            glyph.advance = font.unitsPerEm * scale;
            glyph.rotate -= 90;
            glyph.upright = true;
        }
        t.glyphs.push(glyph);
        pen->x += glyph.advance + spacingAfter(t, t.chars.data[g.last], span.style);
    }
    t.kernFont = &font;
    t.kernGlyph = previous;
}

// Sets a run in a host font as the shaper shapes it, with the surrounding text as context.
void setHostRun(Layout& t, Run& run, PrVec2* pen, uint32_t chunk) {
    const Span& span = t.spans.data[t.chars.data[run.firstChar].span];
    float size = span.style.fontSize;
    uint32_t start = t.chars.data[run.firstChar].offset;
    const Char& last = t.chars.data[run.endChar - 1];
    uint32_t end = last.offset + last.length;
    SvgTextFonts& fonts = fontsOf(t);
    if (fonts.glyphs == nullptr)
        fonts.glyphs = prTextGlyphBufferCreate();
    PrTextRun shaped{};
    shaped.font = run.font.host;
    shaped.text = PrStringView{t.text.data + start, end - start};
    shaped.before = PrStringView{t.text.data, start};
    shaped.after = PrStringView{t.text.data + end, t.text.count - end};
    bool reversed = (t.chars.data[run.firstChar].level & 1) != 0;
    shaped.direction = reversed ? PrTextDirection_Rtl : PrTextDirection_Ltr;
    shaped.locale = span.locale;
    shaped.script = t.chars.data[run.firstChar].script;
    shaped.axis = run.upright ? PrTextAxis_Vertical : PrTextAxis_Horizontal;
    prTextFeatureSetApply(span.style.features, &shaped);
    if (prTextShaperShape(buildOf(t).options->shaper, &shaped, fonts.glyphs) != PrStatus_Ok)
        return;
    uint32_t count = prTextGlyphBufferGlyphCount(fonts.glyphs);
    const PrTextGlyph* glyphs = prTextGlyphBufferGlyphs(fonts.glyphs);
    // The characters not yet placed are those from `next` on, or, in right-to-left text, those before `next`.
    uint32_t next = reversed ? run.endChar : run.firstChar;
    uint32_t placed = kArrayMissing;
    for (uint32_t g = 0; g < count; ++g) {
        const PrTextGlyph& glyph = glyphs[g];
        uint32_t offset = start + glyph.cluster;
        // Each character that the glyph covers, in visual order, moves the pen by its position.
        for (;;) {
            uint32_t ci = kArrayMissing;
            if (!reversed && next < run.endChar && t.chars.data[next].offset <= offset)
                ci = next++;
            else if (reversed && next > run.firstChar && t.chars.data[next - 1].offset >= offset)
                ci = --next;
            if (ci == kArrayMissing)
                break;
            if (placed != kArrayMissing)
                pen->x += spacingAfter(t, t.chars.data[placed], span.style);
            placeChar(t.chars.data[ci], pen);
            placed = ci;
        }
        if (placed == kArrayMissing)
            placed = reversed ? run.endChar - 1 : run.firstChar;
        const Char& c = t.chars.data[placed];
        Glyph placedGlyph{
            .id = glyph.id,
            .position = {pen->x + glyph.offset.x * size, pen->y + glyph.offset.y * size},
            .pen = *pen,
            .rotate = prF32IsNan(c.rotate) ? 0 : c.rotate,
            .advance = glyph.advance.x * size,
            .chunk = chunk,
            .character = placed,
            .scale = glyph.scale,
            .stretch = 1,
            .warp = kArrayMissing,
        };
        // A run shaped down the line places its glyphs by their horizontal origins, about the line's center
        // (PrTextGlyph), which the frame of vertical text turns a quarter turn back.
        if (run.upright) {
            placedGlyph.position = PrVec2{pen->x + glyph.offset.y * size, pen->y - glyph.offset.x * size};
            placedGlyph.advance = glyph.advance.y * size;
            placedGlyph.rotate -= 90;
            placedGlyph.upright = true;
        }
        t.glyphs.push(placedGlyph);
        pen->x += placedGlyph.advance;
        if (!run.upright)
            pen->y += glyph.advance.y * size;
    }
    if (placed != kArrayMissing)
        pen->x += spacingAfter(t, t.chars.data[placed], span.style);
}

// Fits the glyphs of each element with a textLength to that length, as its lengthAdjust specifies: by spacing them out,
// or by also stretching them (SVG11/text.html#TextElementTextLengthAttribute). The glyphs that follow the element in
// its chunk move with the element's end. The elements are fitted innermost first, so an outer element fits the result
// of its inner ones.
void fitLengths(Layout& t) {
    for (uint32_t p = t.positioned.count; p-- > 0;) {
        const Positioned& at = t.positioned.data[p];
        SvgLengthPercentage length{};
        PrStringView text = svgAttribute(at.element, "textLength");
        if (text.data == nullptr || !svgParseLength(text, t.spans.data[at.span].computed, &length) ||
                length.px < 0 || length.percent < 0)
            continue;
        // No adjustment is made to an element that holds a forced line break (SVG2/text.html#TextLayoutAlgorithm).
        bool forced = false;
        for (uint32_t i = at.start; i < at.end && !forced; ++i)
            forced = t.chars.data[i].forced;
        if (forced)
            continue;
        float target = svgLengthResolve(length, percentBase(t, SvgAxis_X));
        uint32_t first = kArrayMissing;
        uint32_t end = 0;
        float left = kPrF32Infinity;
        float right = -kPrF32Infinity;
        for (uint32_t g = 0; g < t.glyphs.count; ++g) {
            const Glyph& glyph = t.glyphs.data[g];
            if (glyph.character < at.start || glyph.character >= at.end)
                continue;
            if (first == kArrayMissing)
                first = g;
            end = g + 1;
            left = prF32Min(left, glyph.pen.x);
            right = prF32Max(right, glyph.pen.x + glyph.advance);
        }
        if (first == kArrayMissing || !(right > left))
            continue;
        float delta = target - (right - left);
        bool stretch = svgAttribute(at.element, "lengthAdjust") == "spacingAndGlyphs";
        uint32_t count = end - first;
        for (uint32_t g = first; g < end; ++g) {
            Glyph& glyph = t.glyphs.data[g];
            if (stretch) {
                // The glyph stretches about its pen, and so does its offset from the pen.
                float k = target / (right - left);
                glyph.position.x = left + (glyph.position.x - left) * k;
                glyph.pen.x = left + (glyph.pen.x - left) * k;
                glyph.advance *= k;
                glyph.stretch *= k;
            } else if (count > 1) {
                float by = delta * float(g - first) / float(count - 1);
                glyph.position.x += by;
                glyph.pen.x += by;
            }
        }
        uint32_t chunk = t.glyphs.data[end - 1].chunk;
        for (uint32_t g = end; g < t.glyphs.count && t.glyphs.data[g].chunk == chunk; ++g) {
            t.glyphs.data[g].position.x += delta;
            t.glyphs.data[g].pen.x += delta;
        }
    }
}

// Returns the path that a textPath follows, in the text's user space, or null if the textPath is in error: its path
// attribute, if it has a segment, or else the path or the basic shape that its href names, under that element's
// transform (SVG2/text.html#TextPathElement). Writes the target's pathLength, or -1, to `pathLength`.
PrPath* PR_NULLABLE textPathOf(Layout& t, const PrDomElement* element, float* pathLength) {
    *pathLength = -1;
    PrPath* path = prPathCreate();
    PrStringView d = svgAttribute(element, "path");
    if (d.data != nullptr)
        prPathAppendData(path, d);
    if (prPathVerbCount(path) != 0)
        return path;
    const PrDomElement* target = svgBuildHref(*t.b, element, true);
    const CssComputedStyle* style = target != nullptr ? svgBuildStyle(*t.b, target) : nullptr;
    SvgNode shape{};
    RawArray<PrPathDataCommand> commands;
    if (style != nullptr && svgReadShape(target, style, &shape, &commands)) {
        if (shape.kind != SvgNodeKind_Path)
            svgShapeCommands(shape, t.b->viewport, &commands);
        prPathAppendCommands(path, commands.data, commands.count);
        prPathTransform(path, svgTransformMatrix(svgStyleTransform(style), PrRect{}));
        if (!svgStyleIs(style, PrCssProperty_PathLength, PrCssKeyword_None))
            *pathLength = prF32Max(svgStyleLength(style, PrCssProperty_PathLength).px, 0);
    }
    commands.reset();
    if (prPathVerbCount(path) == 0) {
        prPathDestroy(path);
        return nullptr;
    }
    return path;
}

// Measures the path of each textPath, and starts its characters at the start of the path as a chunk of their own. A
// character in the textPath with an x is placed that far along the path, and every y in it is ignored
// (SVG2/text.html#TextLayoutAlgorithm). Text after the textPath, unless it has a position of its own, starts at the end
// of the path, or where a closed path's text started (SVG2/text.html#TextpathLayoutRules). A textPath in error draws
// none of its characters.
void measurePaths(Layout& t) {
    for (uint32_t i = 0; i < t.paths.count; ++i) {
        TextPath& p = t.paths.data[i];
        float pathLength = -1;
        PrPath* path = textPathOf(t, p.element, &pathLength);
        if (path != nullptr) {
            p.measure = prPathMeasureCreate();
            prPathMeasureSetPath(p.measure, path, prAffine2Identity(), kTextPathTolerance);
            prPathDestroy(path);
            p.length = prPathMeasureLength(p.measure);
            p.closed = prPathMeasureContourCount(p.measure) == 1 && prPathMeasureContour(p.measure, 0).closed;
        }
        p.right = svgAttribute(p.element, "side") == "right";
        p.stretch = svgAttribute(p.element, "method") == "stretch";
        // startOffset is a length, a percentage of the path's length, or a number in the units of the target's
        // pathLength (SVG2/paths.html#PathLengthAttribute).
        SvgLengthPercentage offset{};
        PrStringView text = svgAttribute(p.element, "startOffset");
        const CssComputedStyle* style = t.spans.data[0].computed;
        for (uint32_t s = 0; s < t.spans.count; ++s) {
            if (t.spans.data[s].element == p.element)
                style = t.spans.data[s].computed;
        }
        if (text.data != nullptr)
            svgParseLength(text, style, &offset);
        if (pathLength > 0)
            offset.px *= p.length / pathLength;
        p.offset = svgLengthResolve(offset, p.length);
        for (uint32_t c = p.start; c < p.end; ++c) {
            t.chars.data[c].y = kPrF32Nan;
            t.chars.data[c].hidden = t.chars.data[c].hidden || p.measure == nullptr;
        }
        if (p.start < p.end) {
            Char& first = t.chars.data[p.start];
            first.x = prF32IsNan(first.x) ? 0 : first.x;
            first.y = 0;
        }
        if (p.end < t.chars.count && p.measure != nullptr && !isAbsolute(t.chars.data[p.end])) {
            float at = p.right ? 0 : p.length;
            if (p.closed)
                at = p.offset - prF32Floor(p.offset / p.length) * p.length;
            PrPathMeasurePosition end = prPathMeasurePosition(p.measure, at);
            PrVec2 point = t.vertical ? prAffine2MapPoint(fromPageOf(t), end.point) : end.point;
            t.chars.data[p.end].x = point.x;
            t.chars.data[p.end].y = point.y;
        }
    }
}

// Returns the transform that lays the line onto the textPath at `distance` along it, which is on the path. For
// side="right", the line runs back along the path from its end, with its y-axis to the path's left.
bool lineOnPath(const TextPath& p, float distance, PrAffine2* out) {
    if (!p.right)
        return prPathMeasureLineTransform(p.measure, distance, out);
    PrAffine2 along{};
    if (!prPathMeasureLineTransform(p.measure, p.length - distance, &along))
        return false;
    PrAffine2 reverse{-1, 0, 0, -1, p.length, 0};
    *out = prAffine2Concat(reverse, along);
    return true;
}

// Moves the glyphs of each textPath onto its path. A glyph's midpoint, at its distance along the line after
// startOffset, goes to that distance along the path. The glyph is rotated to the path's direction, and its distance
// from the baseline is applied across the path. A glyph whose midpoint falls off either end is not drawn. On a closed
// path, the text goes round once from where text-anchor puts it about startOffset: on from it for start, back from it
// for end, and both ways for middle, and a glyph past the circuit is not drawn.
void followPaths(Layout& t) {
    for (uint32_t i = 0; i < t.paths.count; ++i) {
        TextPath& p = t.paths.data[i];
        if (p.start == p.end)
            continue;
        float from = p.offset;
        if (p.closed) {
            Anchor anchor = t.spans.data[t.chars.data[p.start].span].style.textAnchor;
            if (t.spans.data[0].style.rightToLeft && anchor != Anchor_Middle)
                anchor = anchor == Anchor_Start ? Anchor_End : Anchor_Start;
            from -= anchor == Anchor_Middle ? p.length / 2 : anchor == Anchor_End ? p.length : 0;
        }
        for (uint32_t g = 0; g < t.glyphs.count; ++g) {
            Glyph& glyph = t.glyphs.data[g];
            if (glyph.character < p.start || glyph.character >= p.end || glyph.hidden)
                continue;
            float mid = p.offset + glyph.pen.x + glyph.advance / 2;
            if (p.closed) {
                if (mid < from || mid >= from + p.length) {
                    glyph.hidden = true;
                    continue;
                }
                mid -= prF32Floor(mid / p.length) * p.length;
            }
            PrAffine2 onPath{};
            if (p.measure == nullptr || !lineOnPath(p, mid, &onPath)) {
                glyph.hidden = true;
                continue;
            }
            // The glyph keeps its place about its pen, which goes to the midpoint's distance less half its advance.
            PrVec2 origin{mid - glyph.advance / 2 + glyph.position.x - glyph.pen.x, glyph.position.y};
            glyph.onPath = true;
            // The pen goes onto the path too, at its line rather than at the aligned baseline, where the character
            // starts.
            PrVec2 penOnPage = prAffine2MapPoint(onPath, PrVec2{mid - glyph.advance / 2, glyph.pen.y - glyph.align});
            glyph.pen = t.vertical ? prAffine2MapPoint(fromPageOf(t), penOnPage) : penOnPage;
            if (p.stretch) {
                glyph.position = origin;
                glyph.warp = i;
                continue;
            }
            // In vertical text, a glyph's up is back along the path (SVG2/text.html#TextpathLayoutRules), a quarter
            // turn less than horizontal text's. The frame turns the glyph back to the page when the text is recorded.
            float angle = prF32Atan2(onPath.b, onPath.a) * (180 / kPrF32Pi);
            PrVec2 onPage = prAffine2MapPoint(onPath, origin);
            glyph.position = t.vertical ? prAffine2MapPoint(fromPageOf(t), onPage) : onPage;
            glyph.rotate += t.vertical ? angle - 90 : angle;
        }
    }
}

// Appends `outline`, in the line's coordinates, bent onto the textPath: each point of the flattened outline goes as far
// along the path as it is along the line, and as far across the path as it is from the line
// (SVG2/text.html#TextPathMethod).
void warpOntoPath(const TextPath& p, const PrPath* outline, PrPath* out) {
    PrPathPolyline flat{};
    prPathFlatten(outline, prAffine2Identity(), kTextPathTolerance, PrPathFlattening_Inside, &flat);
    for (uint32_t c = 0; c < flat.contourCount; ++c) {
        uint32_t end = prPathPolylineContourEnd(&flat, c);
        for (uint32_t k = flat.contours[c]; k < end; ++k) {
            PrVec2 point = flat.points[k];
            float along = p.right ? p.length - point.x : point.x;
            float across = p.right ? -point.y : point.y;
            // Across is toward the right of the path, facing along it with y down. A reversed path's right is the
            // path's left, which `across` turns around.
            PrPathMeasurePosition at = prPathMeasurePosition(p.measure, along);
            PrVec2 mapped{at.point.x - at.tangent.y * across, at.point.y + at.tangent.x * across};
            if (k == flat.contours[c])
                prPathMoveTo(out, mapped);
            else
                prPathLineTo(out, mapped);
        }
        if (prPathPolylineContourIsClosed(&flat, c))
            prPathClose(out);
    }
    prPathPolylineReset(&flat);
}

// Computes the form in which each character joins, from its joining type and the joining types of its neighbors.
// Transparent characters between them are skipped (Unicode 9.2). An SVG font chooses Arabic glyphs by the form.
void arabicForms(Layout& t) {
    PrStringView text{t.text.data, t.text.count};
    auto type = [&](uint32_t i) {
        uint32_t codePoint = 0;
        prUtf8Decode(text, t.chars.data[i].offset, &codePoint);
        return prUnicodeJoiningType(codePoint);
    };
    for (uint32_t i = 0; i < t.chars.count; ++i) {
        PrUnicodeJoiningType own = type(i);
        if (own == PrUnicodeJoiningType_Transparent)
            continue;
        PrUnicodeJoiningType before = PrUnicodeJoiningType_NonJoining;
        for (uint32_t j = i; j-- > 0;) {
            before = type(j);
            if (before != PrUnicodeJoiningType_Transparent)
                break;
        }
        PrUnicodeJoiningType after = PrUnicodeJoiningType_NonJoining;
        for (uint32_t j = i + 1; j < t.chars.count; ++j) {
            after = type(j);
            if (after != PrUnicodeJoiningType_Transparent)
                break;
        }
        auto joinsAfter = [](PrUnicodeJoiningType j) {
            return j == PrUnicodeJoiningType_Left || j == PrUnicodeJoiningType_Dual ||
                j == PrUnicodeJoiningType_JoinCausing;
        };
        auto joinsBefore = [](PrUnicodeJoiningType j) {
            return j == PrUnicodeJoiningType_Right || j == PrUnicodeJoiningType_Dual ||
                j == PrUnicodeJoiningType_JoinCausing;
        };
        bool previous = joinsBefore(own) && joinsAfter(before);
        bool next = joinsAfter(own) && joinsBefore(after);
        if (previous && next)
            t.chars.data[i].form = SvgArabicForm_Medial;
        else if (previous)
            t.chars.data[i].form = SvgArabicForm_Terminal;
        else if (next)
            t.chars.data[i].form = SvgArabicForm_Initial;
        else
            t.chars.data[i].form = SvgArabicForm_Isolated;
    }
}

// Computes each character's embedding level and script (pr/text_itemize.h, SVG2/text.html#TextLayoutAlgorithm). Each
// text chunk is a paragraph in the text element's direction, and a single line. The unicode-bidi values of the chunk's
// elements become controls around their text. A chunk closes the controls that are open at its end, and the next chunk
// opens them again.
void itemize(Layout& t) {
    t.pieces.clear();
    t.spanStack.clear();
    auto element = [&](uint32_t span, PrTextPieceKind kind, uint32_t at) {
        const TextStyle& style = t.spans.data[span].style;
        PrTextPiece piece{};
        piece.kind = kind;
        piece.bidi = style.unicodeBidi;
        piece.direction = style.rightToLeft ? PrTextDirection_Rtl : PrTextDirection_Ltr;
        piece.start = piece.end = at;
        t.pieces.push(piece);
    };
    for (uint32_t i = 0; i < t.chars.count; ++i) {
        const Char& c = t.chars.data[i];
        if (i != 0 && isAbsolute(c)) {
            PrTextPiece chunk{};
            chunk.kind = PrTextPieceKind_Break;
            chunk.start = chunk.end = c.offset;
            t.pieces.push(chunk);
        }
        // The elements that contain the character and represent bidi controls, outermost first.
        t.order.clear();
        for (uint32_t s = c.span; s != 0 && s != kArrayMissing; s = t.spans.data[s].parent) {
            if (t.spans.data[s].style.unicodeBidi != PrTextBidi_Normal)
                t.order.push(s);
        }
        uint32_t depth = 0;
        while (depth < t.spanStack.count && depth < t.order.count &&
                t.spanStack.data[depth] == t.order.data[t.order.count - 1 - depth])
            ++depth;
        while (t.spanStack.count > depth)
            element(t.spanStack.data[--t.spanStack.count], PrTextPieceKind_Close, c.offset);
        for (; depth < t.order.count; ++depth) {
            uint32_t span = t.order.data[t.order.count - 1 - depth];
            element(span, PrTextPieceKind_Open, c.offset);
            t.spanStack.push(span);
        }
        PrTextPiece* last = t.pieces.count != 0 ? &t.pieces.last() : nullptr;
        if (last != nullptr && last->kind == PrTextPieceKind_Text && last->style == c.span && last->end == c.offset) {
            last->end = c.offset + c.length;
        } else {
            PrTextPiece text{};
            text.kind = PrTextPieceKind_Text;
            text.start = c.offset;
            text.end = c.offset + c.length;
            text.style = c.span;
            text.orientation = t.vertical ? t.spans.data[c.span].style.orientation : PrTextOrientation_Horizontal;
            t.pieces.push(text);
        }
    }
    while (t.spanStack.count != 0)
        element(t.spanStack.data[--t.spanStack.count], PrTextPieceKind_Close, uint32_t(t.text.count));
    const TextStyle& own = t.spans.data[0].style;
    PrTextItemizeDesc desc{};
    desc.text = PrStringView{t.text.data, t.text.count};
    desc.pieces = t.pieces.data;
    desc.pieceCount = t.pieces.count;
    desc.direction = own.rightToLeft ? PrTextDirection_Rtl : PrTextDirection_Ltr;
    desc.bidi = own.unicodeBidi;
    if (t.items == nullptr)
        t.items = prTextItemsCreate();
    prTextItemsItemize(t.items, &desc);
    const uint8_t* levels = prTextItemsLevels(t.items);
    const PrScript* scripts = prTextItemsScripts(t.items);
    const bool* upright = prTextItemsUpright(t.items);
    for (uint32_t i = 0; i < t.chars.count; ++i) {
        Char& c = t.chars.data[i];
        c.level = levels != nullptr ? levels[c.offset] : 0;
        c.script = scripts[c.offset];
        c.upright = upright != nullptr && upright[c.offset];
    }
}

// Moves each chunk as the text-anchor of its first character specifies (SVG11/text.html#TextAnchorProperty).
void anchorChunks(Layout& t) {
    for (uint32_t g = 0; g < t.glyphs.count;) {
        uint32_t chunk = t.glyphs.data[g].chunk;
        uint32_t end = g;
        float left = t.glyphs.data[g].pen.x;
        float right = left;
        bool any = false;
        while (end < t.glyphs.count && t.glyphs.data[end].chunk == chunk) {
            const Glyph& glyph = t.glyphs.data[end++];
            if (glyph.hidden)
                continue;
            left = any ? prF32Min(left, glyph.pen.x) : glyph.pen.x;
            right = any ? prF32Max(right, glyph.pen.x + glyph.advance) : glyph.pen.x + glyph.advance;
            any = true;
        }
        Anchor anchor = Anchor_Start;
        for (uint32_t i = 0; i < t.runs.count; ++i) {
            const Run& run = t.runs.data[i];
            if (g >= run.firstGlyph && g < run.endGlyph)
                anchor = t.spans.data[t.chars.data[run.firstChar].span].style.textAnchor;
        }
        // The start of text-anchor is the side on which the text's direction starts. Vertical text starts at the top.
        if (t.spans.data[0].style.rightToLeft && !t.vertical && anchor != Anchor_Middle)
            anchor = anchor == Anchor_Start ? Anchor_End : Anchor_Start;
        float shift = anchor == Anchor_Middle ? -(right - left) / 2 : anchor == Anchor_End ? -(right - left) : 0;
        shift += t.glyphs.data[g].pen.x - left;
        for (uint32_t i = g; i < end; ++i) {
            t.glyphs.data[i].position.x += shift;
            t.glyphs.data[i].pen.x += shift;
        }
        g = end;
    }
}

// Writes the ascent and descent of a run's font, in user units.
void extentOf(const Run& run, float size, float* ascent, float* descent) {
    if (run.font.svg != nullptr) {
        *ascent = run.font.svg->ascent * size / run.font.svg->unitsPerEm;
        *descent = run.font.svg->descent * size / run.font.svg->unitsPerEm;
    } else if (run.font.host != nullptr) {
        PrFontMetrics metrics = prFontMetrics(run.font.host);
        *ascent = metrics.ascent * size;
        *descent = metrics.descent * size;
    } else {
        *ascent = 0;
        *descent = 0;
    }
}

// Writes how far a run's glyph cells reach across its line from the baseline: up by the ascent of its font and down by
// its descent, or for vertical text, whose baseline is central, half their sum each way.
void cellOf(const Layout& t, const Run& run, float* above, float* below) {
    extentOf(run, t.spans.data[t.chars.data[run.firstChar].span].style.fontSize, above, below);
    if (t.vertical)
        *above = *below = (*above + *below) / 2;
}

// Returns the position of a baseline of the run's font at font size `size`, down from its alphabetic baseline.
float baselineIn(const Run& run, float size, Baseline baseline) {
    if (run.font.svg != nullptr) {
        const SvgFont& f = *run.font.svg;
        float up = 0;
        switch (baseline) {
            case Baseline_Ideographic: up = f.ideographic; break;
            case Baseline_Hanging: up = f.hanging; break;
            case Baseline_Mathematical: up = f.mathematical; break;
            case Baseline_Central: up = (f.ascent - f.descent) / 2; break;
            case Baseline_Middle: up = f.xHeight / 2; break;
            case Baseline_TextTop: up = f.ascent; break;
            case Baseline_TextBottom: up = -f.descent; break;
            default: break;
        }
        return -up * size / f.unitsPerEm;
    }
    if (run.font.host == nullptr)
        return 0;
    PrFontMetrics m = prFontMetrics(run.font.host);
    switch (baseline) {
        case Baseline_Ideographic: return m.ideographicUnder * size;
        case Baseline_Hanging: return m.hangingBaseline * size;
        case Baseline_Mathematical: return m.mathBaseline * size;
        case Baseline_Central: return m.centralBaseline * size;
        case Baseline_Middle: return -m.xHeight / 2 * size;
        case Baseline_TextTop: return -m.ascent * size;
        case Baseline_TextBottom: return m.descent * size;
        default: return 0;
    }
}

// Returns how far down the run's glyphs move from the pen to align a baseline of the run's font with the pen's
// baseline. That baseline is the element's alignment-baseline, or else the dominant baseline. Upright glyphs are
// centered on the line already.
float runAlign(const Layout& t, const Run& run) {
    const TextStyle& own = t.spans.data[t.chars.data[run.firstChar].span].style;
    Baseline baseline = own.alignmentBaseline != Baseline_Auto ? own.alignmentBaseline : own.dominantBaseline;
    // The dominant baseline of vertical text is its central one (css-inline-3/#valdef-dominant-baseline-auto).
    if (baseline == Baseline_Auto && t.vertical)
        baseline = Baseline_Central;
    float align = -baselineIn(run, own.fontSize, baseline);
    if (run.upright)
        align += baselineIn(run, own.fontSize, Baseline_Central);
    return align;
}

// Returns how far down the run's glyphs move from the pen: the baseline-shift of each enclosing element, with sub and
// super at the subscript and superscript positions of the run's font.
float runShift(const Layout& t, const Run& run) {
    uint32_t index = t.chars.data[run.firstChar].span;
    float shift = 0;
    for (; index != kArrayMissing; index = t.spans.data[index].parent) {
        const TextStyle& style = t.spans.data[index].style;
        if (style.shiftKind == Shift_Length) {
            shift -= svgLengthResolve(style.shift, style.fontSize);
            continue;
        }
        bool sub = style.shiftKind == Shift_Sub;
        float position = sub ? 0.2f : -0.33f;
        if (run.font.host != nullptr) {
            PrFontMetrics m = prFontMetrics(run.font.host);
            position = sub ? m.subscriptPosition : m.superscriptPosition;
        }
        shift += position * style.fontSize;
    }
    return shift;
}

// Returns a glyph's own transform about its origin: scaled, stretched horizontally, then rotated.
PrAffine2 glyphOwn(const Glyph& glyph) {
    return prAffine2Concat(prAffine2Scale(PrVec2{glyph.scale * glyph.stretch, glyph.scale}),
        prAffine2Rotate(glyph.rotate * (kPrF32Pi / 180)));
}

PrAffine2 glyphTransform(const Glyph& glyph) {
    return prAffine2Concat(glyphOwn(glyph), prAffine2Translate(glyph.position));
}

// Writes the center of a decoration line, down from the baseline, and its thickness, in user units, as the run's font
// places them.
void decorationLine(const Run& run, float size, SvgLineFlags line, float* center, float* thickness) {
    if (run.font.svg != nullptr) {
        const SvgFont& f = *run.font.svg;
        float scale = size / f.unitsPerEm;
        float position = f.strikethroughPosition;
        if (line == SvgLineFlag_Underline)
            position = f.underlinePosition;
        else if (line == SvgLineFlag_Overline)
            position = f.overlinePosition;
        float width = f.strikethroughThickness;
        if (line == SvgLineFlag_Underline)
            width = f.underlineThickness;
        else if (line == SvgLineFlag_Overline)
            width = f.overlineThickness;
        *center = -position * scale;
        *thickness = width * scale;
        return;
    }
    if (run.font.host == nullptr) {
        *center = *thickness = 0;
        return;
    }
    PrFontMetrics m = prFontMetrics(run.font.host);
    if (line == SvgLineFlag_Underline) {
        *center = m.underlinePosition * size;
        *thickness = m.underlineThickness * size;
    } else if (line == SvgLineFlag_Overline) {
        *thickness = m.underlineThickness * size;
        *center = -m.ascent * size + *thickness / 2;
    } else {
        *center = m.strikeoutPosition * size;
        *thickness = m.strikeoutThickness * size;
    }
}

// Records outlines with `paints`, unless the paints draw nothing.
void recordPath(Layout& t, PrPath* path, const SvgPaints& paints, PrPathFillRule rule) {
    if (prPathVerbCount(path) == 0) {
        prPathDestroy(path);
        return;
    }
    t.out->pieces.push(SvgTextPiece{.paints = paints, .rule = rule, .path = path});
}

// Records the decorations in `lines` of the run's element and its ancestors. Each line is drawn under the glyphs that
// share a baseline, and is painted with the paints of the element that declares it.
void recordDecorations(Layout& t, const Run& run, SvgLineFlags lines) {
    const TextStyle& style = t.spans.data[t.chars.data[run.firstChar].span].style;
    if (run.firstGlyph == run.endGlyph || !style.visible)
        return;
    for (uint32_t d = 0; d < style.decorationCount; ++d) {
        const Decoration& decoration = style.decorations[d];
        for (uint32_t bit = 0; bit < 3; ++bit) {
            SvgLineFlags line = SvgLineFlags(1u << bit);
            if ((decoration.lines & line & lines) == 0)
                continue;
            float center = 0;
            float thickness = 0;
            decorationLine(run, style.fontSize, line, &center, &thickness);
            PrPath* path = prPathCreate();
            for (uint32_t g = run.firstGlyph; g < run.endGlyph;) {
                if (t.glyphs.data[g].hidden) {
                    ++g;
                    continue;
                }
                float y = t.glyphs.data[g].pen.y;
                float left = t.glyphs.data[g].pen.x;
                float right = left;
                for (; g < run.endGlyph && t.glyphs.data[g].pen.y == y && !t.glyphs.data[g].hidden; ++g)
                    right = t.glyphs.data[g].pen.x + t.glyphs.data[g].advance;
                prPathAddRect(path, PrRect{left, y + center - thickness / 2, right, y + center + thickness / 2});
            }
            recordPath(t, path, decoration.paints, PrPathFillRule_NonZero);
        }
    }
}

// Appends a glyph's outline to `out`, placed by `transform`, or bent onto its text path if it has method="stretch".
void addOutline(Layout& t, const Glyph& glyph, const PrPath* outline, PrAffine2 transform, PrPath* out) {
    if (glyph.warp == kArrayMissing) {
        prPathAddPath(out, outline, transform);
        return;
    }
    PrPath* line = prPathCreate();
    prPathAddPath(line, outline, transform);
    PrPath* bent = prPathCreate();
    warpOntoPath(t.paths.data[glyph.warp], line, bent);
    prPathAddPath(out, bent, t.vertical ? fromPageOf(t) : prAffine2Identity());
    prPathDestroy(bent);
    prPathDestroy(line);
}

// Records the run's glyphs: a host font's glyphs, or an SVG font's outlines and the content of its glyph elements,
// which inherits from the run's element. A glyph that a text path bends is recorded as its outline.
void recordRun(Layout& t, const Run& run) {
    const Span& span = t.spans.data[t.chars.data[run.firstChar].span];
    const TextStyle& style = span.style;
    if (run.firstGlyph == run.endGlyph || !style.visible)
        return;
    if (run.font.svg != nullptr) {
        const SvgFont& font = *run.font.svg;
        float scale = style.fontSize / font.unitsPerEm;
        PrPath* path = prPathCreate();
        for (uint32_t g = run.firstGlyph; g < run.endGlyph; ++g) {
            const Glyph& glyph = t.glyphs.data[g];
            const PrPath* outline = font.glyphs.data[glyph.id].path;
            if (!glyph.hidden && outline != nullptr)
                addOutline(t, glyph, outline,
                    prAffine2Concat(prAffine2Scale(PrVec2{scale, -scale}), glyphTransform(glyph)), path);
        }
        recordPath(t, path, style.paints, style.fillRule);
        // The child elements of a glyph draw in its font units with y up, the same space as its path data, and inherit
        // the text's style (SVG11/fonts.html#GlyphElement).
        for (uint32_t g = run.firstGlyph; g < run.endGlyph; ++g) {
            const Glyph& glyph = t.glyphs.data[g];
            const PrDomElement* content = font.glyphs.data[glyph.id].content;
            if (glyph.hidden || content == nullptr)
                continue;
            SvgBuilder& b = *t.b;
            SvgNode group{
                .kind = SvgNodeKind_Group,
                .visible = true,
                .opacity = 1,
                .element = content,
                .transform = prAffine2Concat(prAffine2Scale(PrVec2{scale, -scale}), glyphTransform(glyph)),
                .transformStyle = kArrayMissing,
            };
            // The text owns the stroke of the paints.
            group.paints = style.paints;
            group.paints.strokeStyle = kArrayMissing;
            group.markers = kArrayMissing;
            group.effects = kArrayMissing;
            SvgNodeIndex index = svgBuildNode(b, group, t.node, false);
            svgBuildInstanceChildren(b, content, span.computed, index);
        }
        return;
    }
    if (run.font.host == nullptr)
        return;
    SvgTextPiece piece{
        .paints = style.paints,
        .rule = style.fillRule,
        .font = run.font.host,
        .size = style.fontSize,
        .firstGlyph = t.out->glyphs.count,
    };
    PrPath* bent = prPathCreate();
    PrPath* outline = prPathCreate();
    for (uint32_t g = run.firstGlyph; g < run.endGlyph; ++g) {
        const Glyph& glyph = t.glyphs.data[g];
        if (glyph.hidden)
            continue;
        if (glyph.warp != kArrayMissing) {
            prPathClear(outline);
            PrAffine2 em =
                prAffine2Concat(prAffine2Scale(PrVec2{style.fontSize, style.fontSize}), glyphTransform(glyph));
            if (prFontGlyphOutline(run.font.host, uint16_t(glyph.id), 0, em, outline))
                addOutline(t, glyph, outline, prAffine2Identity(), bent);
            continue;
        }
        t.out->glyphs.push(PrFontGlyph{uint16_t(glyph.id), glyph.position});
        t.out->transforms.push(glyphOwn(glyph));
        piece.transformed = piece.transformed || glyph.rotate != 0 || glyph.scale != 1 || glyph.stretch != 1;
    }
    prPathDestroy(outline);
    recordPath(t, bent, style.paints, PrPathFillRule_NonZero);
    piece.glyphCount = t.out->glyphs.count - piece.firstGlyph;
    if (piece.glyphCount == 0)
        return;
    prFontRetain(piece.font);
    t.out->pieces.push(piece);
}

void layoutReset(Layout& t) {
    for (uint32_t i = 0; i < t.paths.count; ++i) {
        TextPath& p = t.paths.data[i];
        prPathMeasureDestroy(p.measure);
    }
    for (uint32_t i = 0; i < t.spans.count; ++i) {
        if (t.spans.data[i].held)
            cssComputedStyleRelease(t.spans.data[i].computed);
    }
    t.paths.reset();
    t.text.reset();
    t.spans.reset();
    t.families.reset();
    prTextBufferReset(&t.familyNames);
    t.chars.reset();
    t.positioned.reset();
    t.runs.reset();
    t.glyphs.reset();
    t.values.reset();
    t.svgGlyphs.reset();
    for (uint32_t i = 0; i < t.featureSets.count; ++i)
        prTextFeatureSetRelease(t.featureSets.data[i]);
    t.featureSets.reset();
    t.pieces.reset();
    prTextItemsDestroy(t.items);
    t.items = nullptr;
    t.codePoints.reset();
    t.levels.reset();
    t.order.reset();
    t.spanStack.reset();
}

// Drops the characters that trimming removed, and moves the ranges of the elements, the text and the paths with what
// remains.
void dropRemoved(Layout& t) {
    // By the index of a character before dropping: the characters before it that remain.
    RawArray<uint32_t> kept;
    RawArray<char> text;
    uint32_t count = 0;
    for (uint32_t i = 0; i < t.chars.count; ++i) {
        kept.push(count);
        Char c = t.chars.data[i];
        if (c.removed)
            continue;
        uint32_t offset = text.count;
        text.append(t.text.data + c.offset, c.length);
        c.offset = offset;
        t.chars.data[count++] = c;
    }
    kept.push(count);
    t.chars.count = count;
    t.text.clear();
    t.text.append(text.data, text.count);
    text.reset();
    for (uint32_t i = 0; i < t.spans.count; ++i) {
        t.spans.data[i].start = kept.data[t.spans.data[i].start];
        t.spans.data[i].end = kept.data[t.spans.data[i].end];
    }
    for (uint32_t i = 0; i < t.positioned.count; ++i) {
        t.positioned.data[i].start = kept.data[t.positioned.data[i].start];
        t.positioned.data[i].end = kept.data[t.positioned.data[i].end];
    }
    for (uint32_t i = 0; i < t.paths.count; ++i) {
        t.paths.data[i].start = kept.data[t.paths.data[i].start];
        t.paths.data[i].end = kept.data[t.paths.data[i].end];
    }
    kept.reset();
}

// Starts laying out a text element or a textArea by collecting its characters, each with its containing element. A
// decoration declared outside the text element is painted with the fill and stroke of the text element
// (SVG11/text.html#TextDecorationProperties).
void beginLayout(Layout& t, const PrDomElement* text, const CssComputedStyle* style) {
    t.documentFonts = svgDocumentFonts(text->document);
    TextStyle textStyle = textStyleOf(t, text, style, nullptr);
    textStyle.decorationCount = 0;
    textStyle.textAlign = Anchor_Start;
    textStyle.displayAlign = Anchor_Start;
    // The ancestors' SVG Tiny alignments, and their decorations, reach the text. An outermost svg element is an atomic
    // inline, which no decoration crosses (css-text-decor-3/#line-decoration), so the ancestors end with it.
    RawArray<const PrDomElement*> ancestors;
    for (const PrDomElement* e = t.instance ? nullptr : domElementFromNode(text->parent);
            e != nullptr && e->space == PrDomNamespace_Svg; e = domElementFromNode(e->parent))
        ancestors.push(e);
    for (uint32_t i = ancestors.count; i-- > 0;) {
        const PrDomElement* e = ancestors.data[i];
        textStyle.textAlign = alignOf(e, "text-align", textStyle.textAlign);
        textStyle.displayAlign = alignOf(e, "display-align", textStyle.displayAlign);
        textStyle.lineIncrement = lineIncrementOf(e, textStyle.lineIncrement);
        const CssComputedStyle* ancestor = svgBuildStyle(*t.b, e);
        if (ancestor != nullptr)
            addDecoration(textStyle, svgStyleLines(ancestor), textStyle.paints);
    }
    ancestors.reset();
    textStyle.textAlign = alignOf(text, "text-align", textStyle.textAlign);
    textStyle.displayAlign = alignOf(text, "display-align", textStyle.displayAlign);
    textStyle.lineIncrement = lineIncrementOf(text, textStyle.lineIncrement);
    addDecoration(textStyle, svgStyleLines(style), textStyle.paints);
    PrStringView language = domElementLanguage(text);
    t.spans.push(Span{
        .style = textStyle,
        .element = text,
        .computed = style,
        .held = false,
        .of = kArrayMissing,
        .language = language,
        .locale = prLocaleFromTag(language),
        .parent = kArrayMissing,
    });
    t.positioned.push(Positioned{text, 0, 0, 0});
    // writing-mode applies to the text element alone (SVG2/text.html#WritingModeProperty). A sideways mode sets every
    // character on its side.
    PrCssKeyword mode = svgStyleKeyword(style, PrCssProperty_WritingMode);
    t.vertical = mode == PrCssKeyword_VerticalRl || mode == PrCssKeyword_VerticalLr ||
        mode == PrCssKeyword_SidewaysRl || mode == PrCssKeyword_SidewaysLr;
    t.stacksRight = mode == PrCssKeyword_VerticalLr;
    t.turn = mode == PrCssKeyword_SidewaysLr ? -1 : 1;
    collect(t, text, 0);
    if (mode == PrCssKeyword_SidewaysRl || mode == PrCssKeyword_SidewaysLr) {
        for (uint32_t i = 0; i < t.spans.count; ++i)
            t.spans.data[i].style.orientation = PrTextOrientation_Sideways;
    }
    t.positioned.data[0].end = t.chars.count;
    t.spans.data[0].end = t.chars.count;
    trimLine(t);
    dropRemoved(t);
}

// Splits the characters into runs of one element, font, level and script. Each character that starts a chunk also
// starts a run. A grapheme cluster is set in the font of its first character.
void buildRuns(Layout& t) {
    t.runs.clear();
    Font font{};
    PrStringView all{t.text.data, t.text.count};
    size_t clusterEnd = 0;
    for (uint32_t i = 0; i < t.chars.count; ++i) {
        const Char& c = t.chars.data[i];
        Font next = font;
        if (c.forced) {
            next = Font{};
            clusterEnd = c.offset + c.length;
        } else if (c.offset >= clusterEnd) {
            clusterEnd = prUnicodeGraphemeNext(all, c.offset);
            next = fontFor(t, c, t.spans.data[c.span], prStringSlice(all, c.offset, clusterEnd - c.offset));
        }
        const Char* head = t.runs.count != 0 ? &t.chars.data[t.runs.last().firstChar] : nullptr;
        const Char* previous = i != 0 ? &t.chars.data[i - 1] : nullptr;
        if (head == nullptr || isAbsolute(c) || !(next == font) || head->span != c.span || head->level != c.level ||
                head->script != c.script || head->upright != c.upright || c.forced || previous->forced)
            t.runs.push(Run{.firstChar = i, .endChar = i, .font = next, .upright = c.upright});
        t.runs.last().endChar = i + 1;
        font = next;
    }
}

// Sets each chunk from its start, with its runs in visual order, and hides the glyphs of hidden characters.
void placeRuns(Layout& t) {
    t.glyphs.clear();
    t.kernFont = nullptr;
    PrVec2 pen{0, 0};
    uint32_t chunk = 0;
    for (uint32_t r0 = 0; r0 < t.runs.count; ++chunk) {
        uint32_t r1 = r0 + 1;
        while (r1 < t.runs.count && !isAbsolute(t.chars.data[t.runs.data[r1].firstChar]))
            ++r1;
        const Char& head = t.chars.data[t.runs.data[r0].firstChar];
        // A chunk starts at a position of its own, so its first glyph kerns against nothing before it.
        t.kernFont = nullptr;
        if (!prF32IsNan(head.x))
            pen.x = head.x;
        if (!prF32IsNan(head.y))
            pen.y = head.y;
        uint32_t first = t.runs.data[r0].firstChar;
        uint32_t count = t.runs.data[r1 - 1].endChar - first;
        t.levels.clear();
        for (uint32_t i = 0; i < count; ++i)
            t.levels.push(t.chars.data[first + i].level);
        t.order.clear();
        for (uint32_t i = 0; i < count; ++i)
            t.order.push(0);
        prUnicodeBidiReorder(t.levels.data, count, t.order.data);
        uint32_t previous = kArrayMissing;
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t character = first + t.order.data[k];
            uint32_t index = r0;
            while (t.runs.data[index].endChar <= character)
                ++index;
            if (index == previous)
                continue;
            previous = index;
            Run& run = t.runs.data[index];
            run.firstGlyph = t.glyphs.count;
            if (run.font.svg == nullptr)
                t.kernFont = nullptr;
            if (run.font.svg != nullptr) {
                setSvgRun(t, run, &pen, chunk);
            } else if (run.font.host != nullptr) {
                setHostRun(t, run, &pen, chunk);
            } else {
                for (uint32_t c = run.firstChar; c < run.endChar; ++c)
                    placeChar(t.chars.data[c], &pen);
            }
            run.endGlyph = t.glyphs.count;
            run.align = runAlign(t, run);
            float shift = run.align + runShift(t, run);
            for (uint32_t g = run.firstGlyph; g < run.endGlyph; ++g) {
                t.glyphs.data[g].position.y += shift;
                t.glyphs.data[g].pen.y += shift;
                t.glyphs.data[g].align = run.align;
            }
        }
        r0 = r1;
    }
    for (uint32_t i = 0; i < t.glyphs.count; ++i) {
        Glyph& glyph = t.glyphs.data[i];
        if (t.chars.data[glyph.character].hidden)
            glyph.hidden = true;
    }
}

// Returns the bounds of the glyph cells of the laid-out text, across the whole text element, which a paint server maps
// onto.
PrRect textBounds(const Layout& t) {
    PrRect box{kPrF32Infinity, kPrF32Infinity, -kPrF32Infinity, -kPrF32Infinity};
    for (uint32_t i = 0; i < t.runs.count; ++i) {
        const Run& run = t.runs.data[i];
        if (run.firstGlyph == run.endGlyph)
            continue;
        float ascent = 0;
        float descent = 0;
        cellOf(t, run, &ascent, &descent);
        for (uint32_t g = run.firstGlyph; g < run.endGlyph; ++g) {
            const Glyph& glyph = t.glyphs.data[g];
            if (glyph.hidden || glyph.warp != kArrayMissing)
                continue;
            box.left = prF32Min(box.left, glyph.pen.x);
            box.right = prF32Max(box.right, glyph.pen.x + glyph.advance);
            box.top = prF32Min(box.top, glyph.pen.y - ascent);
            box.bottom = prF32Max(box.bottom, glyph.pen.y + descent);
        }
    }
    return box.left <= box.right ? box : PrRect{};
}

// Records what draws the laid-out text: decorations and glyphs, run by run.
void record(Layout& t) {
    // Underlines and overlines go under the text, and line-throughs over it (css-text-decor-3/#painting-order).
    for (uint32_t i = 0; i < t.runs.count; ++i) {
        const Run& run = t.runs.data[i];
        recordDecorations(t, run, SvgLineFlag_Underline | SvgLineFlag_Overline);
        recordRun(t, run);
        recordDecorations(t, run, SvgLineFlag_LineThrough);
    }
}

// Records each character's position, advance and cell, and the characters of each element, which the methods of
// SVGTextContentElement read (SVG2/text.html#InterfaceSVGTextContentElement).
void recordCharacters(Layout& t) {
    SvgText& out = *t.out;
    PrStringView text{t.text.data, t.text.count};
    for (uint32_t i = 0; i < t.chars.count; ++i) {
        const Char& c = t.chars.data[i];
        uint32_t codePoint = 0;
        prUtf8Decode(text, c.offset, &codePoint);
        SvgTextCharacter character{};
        character.units = c.unaddressable ? 0 : codePoint > 0xffff ? 2 : 1;
        character.hidden = true;
        out.characters.push(character);
    }
    // A character takes the position of its typographic character's first glyph, and the advance of all of them.
    RawArray<bool> placed;
    for (uint32_t i = 0; i < t.chars.count; ++i)
        placed.push(false);
    for (uint32_t r = 0; r < t.runs.count; ++r) {
        const Run& run = t.runs.data[r];
        float ascent = 0;
        float descent = 0;
        cellOf(t, run, &ascent, &descent);
        for (uint32_t g = run.firstGlyph; g < run.endGlyph; ++g) {
            const Glyph& glyph = t.glyphs.data[g];
            SvgTextCharacter& character = out.characters.data[glyph.character];
            if (!placed.data[glyph.character]) {
                placed.data[glyph.character] = true;
                // A glyph on a path has only its position there. Its line runs along its rotation, less the quarter
                // turn that stands an upright glyph up.
                character.position = glyph.onPath ? glyph.pen : PrVec2{glyph.pen.x, glyph.pen.y - run.align};
                character.rotate = glyph.rotate;
                character.direction = glyph.rotate + (glyph.upright ? 90 : 0);
                // The cell is the glyph's, about the baseline that the run aligns, which the position leaves out.
                character.ascent = ascent - glyph.align;
                character.descent = descent + glyph.align;
                character.hidden = glyph.hidden || t.chars.data[glyph.character].hidden;
                const Char& c = t.chars.data[glyph.character];
                character.advance = spacingAfter(t, c, t.spans.data[c.span].style);
            }
            character.advance += glyph.advance;
        }
    }
    // The characters after the first of a typographic character, as of a ligature, share its advance.
    for (uint32_t i = 0; i < t.chars.count;) {
        uint32_t end = i + 1;
        while (end < t.chars.count && !placed.data[end] && !t.chars.data[end].unaddressable)
            ++end;
        if (placed.data[i] && end - i > 1) {
            SvgTextCharacter& first = out.characters.data[i];
            float share = first.advance / float(end - i);
            float radians = first.direction * (kPrF32Pi / 180);
            PrVec2 step{prF32Cos(radians) * share, prF32Sin(radians) * share};
            for (uint32_t k = i; k < end; ++k) {
                SvgTextCharacter& character = out.characters.data[k];
                float n = float(k - i);
                character = first;
                character.advance = share;
                character.position = PrVec2{first.position.x + step.x * n, first.position.y + step.y * n};
            }
        }
        i = end;
    }
    placed.reset();
    for (uint32_t i = 0; i < t.spans.count; ++i) {
        const Span& span = t.spans.data[i];
        if (span.of != kArrayMissing)
            continue;
        const SvgPaints& paints = span.style.paints;
        out.elements.push(SvgTextElement{
            .element = span.element,
            .start = span.start,
            .end = span.end,
            .pointerEvents = span.style.pointerEvents,
            .visible = span.style.visible,
            .painted = paints.fill.kind != SvgPaintKind_None || paints.stroke.kind != SvgPaintKind_None,
        });
    }
}

// Writes the advance of each character in a single-line layout, and the ascent and descent of its font. Line breaking
// and the heights of lines measure these.
void measureChars(Layout& t, RawArray<float>* advances, RawArray<float>* ascents, RawArray<float>* descents) {
    advances->clear();
    ascents->clear();
    descents->clear();
    for (uint32_t i = 0; i < t.chars.count; ++i) {
        advances->push(0);
        ascents->push(0);
        descents->push(0);
    }
    for (uint32_t i = 0; i < t.glyphs.count; ++i) {
        const Glyph& glyph = t.glyphs.data[i];
        advances->data[glyph.character] += glyph.advance;
    }
    for (uint32_t i = 0; i < t.runs.count; ++i) {
        const Run& run = t.runs.data[i];
        float ascent = 0;
        float descent = 0;
        cellOf(t, run, &ascent, &descent);
        for (uint32_t c = run.firstChar; c < run.endChar; ++c) {
            ascents->data[c] = ascent;
            descents->data[c] = descent;
        }
    }
}

// Breaks the characters into lines at forced line breaks, and where they would overflow `width` if it is above zero, at
// the soft wrap opportunities of the elements whose text wraps, as their word-break tailors them (prTextBreaksTailor).
// A word that no opportunity lets fit breaks at the last opportunity that its overflow-wrap adds and that fits, as SVG
// Tiny's textArea breaks any word; otherwise it overflows the line. The spaces that end a line hang past it. A first
// line that `firstEnd` gives ends there. Returns the start of each line, with the end last.
void breakLines(Layout& t, const RawArray<float>& advances, float width, RawArray<uint32_t>* starts,
        uint32_t firstEnd = kArrayMissing) {
    PrStringView text{t.text.data, t.text.count};
    uint32_t count = t.chars.count;
    t.codePoints.clear();
    RawArray<uint32_t> ends;
    RawArray<PrTextBreakStyle> styles;
    for (uint32_t i = 0; i < count; ++i) {
        const Char& c = t.chars.data[i];
        uint32_t codePoint = 0;
        prUtf8Decode(text, c.offset, &codePoint);
        t.codePoints.push(codePoint);
        ends.push(c.offset + c.length);
        PrTextBreakStyle style = t.spans.data[c.span].style.breaks;
        if (t.area)
            style.overflowWrap = PrTextOverflowWrap_Anywhere;
        styles.push(style);
    }
    t.levels.clear();
    for (uint32_t i = 0; i < count; ++i)
        t.levels.push(0);
    RawArray<PrTextEmergency> emergency;
    emergency.resize(count);
    prUnicodeLineBreaks(t.codePoints.data, count, t.levels.data);
    PrTextBreakDesc desc{text, t.codePoints.data, ends.data, styles.data, count};
    if (count != 0)
        prTextBreaksTailor(&desc, t.levels.data, emergency.data);
    auto isSpaceAt = [&](uint32_t i) { return t.codePoints.data[i] == ' '; };
    starts->clear();
    uint32_t start = 0;
    while (start < count) {
        starts->push(start);
        if (start == 0 && firstEnd != kArrayMissing) {
            start = firstEnd;
            continue;
        }
        float used = 0;
        uint32_t fits = kArrayMissing;      // the last soft wrap opportunity that fits
        uint32_t rescue = kArrayMissing;    // the last opportunity that overflow-wrap adds and that fits
        uint32_t end = start;
        for (; end < count; ++end) {
            bool fitsWidth = !(width > 0) || isSpaceAt(end) || used + advances.data[end] <= width || end == start;
            if (!fitsWidth && (fits != kArrayMissing || rescue != kArrayMissing)) {
                end = (fits != kArrayMissing ? fits : rescue) + 1;
                break;
            }
            used += advances.data[end];
            PrUnicodeLineBreak opportunity = t.levels.data[end];
            if (opportunity == PrUnicodeLineBreak_Mandatory && end + 1 < count) {
                ++end;
                break;
            }
            bool wraps = t.spans.data[t.chars.data[end].span].style.wraps || t.area;
            if (wraps && opportunity == PrUnicodeLineBreak_Allowed)
                fits = end;
            else if (wraps && emergency.data[end] != PrTextEmergency_None)
                rescue = end;
        }
        start = end;
    }
    starts->push(count);
    ends.reset();
    styles.reset();
    emergency.reset();
}

// Ignores the positioning attributes of auto-wrapped text, except the x and y of its first character, which are its
// initial position (SVG2/text.html#TextLayoutAuto). Wrapped lines are shifted only by text-anchor
// (SVG2/text.html#TextLayoutAlgorithm).
void ignorePositions(Layout& t) {
    for (uint32_t i = 0; i < t.chars.count; ++i) {
        Char& c = t.chars.data[i];
        if (i != 0)
            c.x = c.y = kPrF32Nan;
        c.dx = c.dy = 0;
        c.rotate = kPrF32Nan;
    }
}

// Returns true if any character is a forced line break.
bool hasForcedBreaks(const Layout& t) {
    for (uint32_t i = 0; i < t.chars.count; ++i) {
        if (t.chars.data[i].forced)
            return true;
    }
    return false;
}

// Breaks the text, set as one line, into lines as breakLines does, and starts each line after the first at the text's
// initial x, its baseline below the line before it. Line boxes stack as CSS stacks them
// (CSS2/visudet.html#line-height): each character's font and line-height give it a box about its baseline, half the
// leading above its ascent and half below its descent, and so does the text element's own font, as a strut. A line
// reaches from the highest top of these boxes to the lowest bottom, and the next line starts at its bottom. A line that
// has a position of its own stays there, and the lines after it follow it. Writes the start of each line to `starts`,
// with the end last, and ends the first line at `firstEnd` if it is given.
void setLines(Layout& t, float width, RawArray<uint32_t>& starts, uint32_t firstEnd = kArrayMissing) {
    RawArray<float> advances;
    RawArray<float> ascents;
    RawArray<float> descents;
    measureChars(t, &advances, &ascents, &descents);
    breakLines(t, advances, width, &starts, firstEnd);
    const Char& first = t.chars.data[0];
    float x = prF32IsNan(first.x) ? 0 : first.x;
    float baseline = prF32IsNan(first.y) ? 0 : first.y;
    // The strut: the text element's line-height about the ascent and descent of the first font that sets the text.
    const TextStyle& own = t.spans.data[0].style;
    float strutAscent = 0;
    float strutDescent = 0;
    for (uint32_t i = 0; i < t.runs.count && strutAscent + strutDescent == 0; ++i)
        extentOf(t.runs.data[i], own.fontSize, &strutAscent, &strutDescent);
    if (t.vertical)
        strutAscent = strutDescent = (strutAscent + strutDescent) / 2;
    float strutLeading = (own.lineHeight - (strutAscent + strutDescent)) / 2;
    float below = 0;
    for (uint32_t l = 0; l + 1 < starts.count; ++l) {
        float lineAbove = strutAscent + strutLeading;
        float lineBelow = strutDescent + strutLeading;
        for (uint32_t c = starts.data[l]; c < starts.data[l + 1]; ++c) {
            if (t.chars.data[c].forced)
                continue;
            const TextStyle& style = t.spans.data[t.chars.data[c].span].style;
            float leading = (style.lineHeight - (ascents.data[c] + descents.data[c])) / 2;
            lineAbove = prF32Max(lineAbove, ascents.data[c] + leading);
            lineBelow = prF32Max(lineBelow, descents.data[c] + leading);
        }
        // The collapsible spaces at the end of a wrapped line are removed (css-text-4/#white-space-phase-2), so that
        // text-anchor places the line by what it draws.
        for (uint32_t c = starts.data[l + 1]; c-- > starts.data[l] && t.chars.data[c].collapsible;)
            t.chars.data[c].hidden = true;
        Char& head = t.chars.data[starts.data[l]];
        // vertical-lr stacks its lines to the right, which is up the frame: the next line's below side faces this
        // line's above side.
        if (t.stacksRight) {
            float above = lineAbove;
            lineAbove = lineBelow;
            lineBelow = above;
        }
        if (l != 0) {
            baseline += t.stacksRight ? -(below + lineAbove) : below + lineAbove;
            if (prF32IsNan(head.x))
                head.x = x;
            if (prF32IsNan(head.y))
                head.y = baseline;
        }
        if (!prF32IsNan(head.y))
            baseline = head.y;
        below = lineBelow;
    }
    advances.reset();
    ascents.reset();
    descents.reset();
}

// Moves the positions of vertical text's characters into its frame (Layout::vertical).
void intoFrame(Layout& t) {
    for (uint32_t i = 0; i < t.chars.count; ++i) {
        Char& c = t.chars.data[i];
        float x = c.x;
        float dx = c.dx;
        c.x = prF32IsNan(c.y) ? kPrF32Nan : c.y * t.turn;
        c.y = prF32IsNan(x) ? kPrF32Nan : -x * t.turn;
        c.dx = c.dy * t.turn;
        c.dy = -dx * t.turn;
    }
}

// Turns what vertical text recorded in its frame onto the page: its outlines, glyphs, bounds and characters, and the
// nodes of its SVG fonts' glyph elements.
void toPage(Layout& t) {
    SvgText& out = *t.out;
    PrAffine2 page = toPageOf(t);
    PrAffine2 turn = prAffine2Rotate(t.turn * kPrF32Pi / 2);
    for (uint32_t i = 0; i < out.pieces.count; ++i) {
        SvgTextPiece& piece = out.pieces.data[i];
        if (piece.path != nullptr) {
            prPathTransform(piece.path, page);
            continue;
        }
        for (uint32_t g = piece.firstGlyph; g < piece.firstGlyph + piece.glyphCount; ++g) {
            out.glyphs.data[g].position = prAffine2MapPoint(page, out.glyphs.data[g].position);
            out.transforms.data[g] = prAffine2Concat(out.transforms.data[g], turn);
        }
        piece.transformed = true;
    }
    auto turnRect = [&](PrRect r) {
        PrVec2 a = prAffine2MapPoint(page, PrVec2{r.left, r.top});
        PrVec2 b = prAffine2MapPoint(page, PrVec2{r.right, r.bottom});
        return PrRect{prF32Min(a.x, b.x), prF32Min(a.y, b.y), prF32Max(a.x, b.x), prF32Max(a.y, b.y)};
    };
    if (out.bounds.right > out.bounds.left)
        out.bounds = turnRect(out.bounds);
    if (out.clipped)
        out.clip = turnRect(out.clip);
    for (uint32_t i = 0; i < out.characters.count; ++i) {
        SvgTextCharacter& c = out.characters.data[i];
        c.position = prAffine2MapPoint(page, c.position);
        c.rotate += 90 * t.turn;
        c.direction += 90 * t.turn;
    }
    SvgTree& tree = *t.b->tree;
    for (SvgNodeIndex child = tree.nodes.data[t.node].first; child != kSvgNodeNone; child = tree.nodes.data[child].next)
        tree.nodes.data[child].transform = prAffine2Concat(tree.nodes.data[child].transform, page);
}

// Gives each span a part under the text's ::first-line style `line`, which the layout then holds: a part of the text's
// own span for the line itself, and a part of each element's span that inherits from the part of its parent's
// (css-pseudo-4/#first-line-inheritance). Every character moves to its span's part, as if all of the text were on its
// first line.
void addLineParts(Layout& t, const CssComputedStyle* line) {
    uint32_t count = t.spans.count;
    RawArray<uint32_t> parts;
    for (uint32_t i = 0; i < count; ++i) {
        Span part = t.spans.data[i];
        // The line's own part sits between the text and its content.
        uint32_t parent = i == 0 ? 0 : parts.data[part.parent];
        part.computed = i == 0 ? line : svgBuildStyleUnder(*t.b, part.element, t.spans.data[parent].computed);
        TextStyle parentStyle = t.spans.data[parent].style;
        PrTextOrientation orientation = part.style.orientation;
        part.style = textStyleOf(t, part.element, part.computed, &parentStyle);
        // text-orientation does not apply to ::first-line, and the sideways modes set every character sideways.
        part.style.orientation = orientation;
        part.held = true;
        part.of = i;
        part.parent = parent;
        parts.push(t.spans.count);
        t.spans.push(part);
    }
    for (uint32_t i = 0; i < t.chars.count; ++i)
        t.chars.data[i].span = parts.data[t.chars.data[i].span];
    parts.reset();
}

// Gives the first-letter text of the text's first line (css-pseudo-4/#first-letter-pattern) a part under the text's
// ::first-letter style, if the text has one and the letter lies in one span, which the part inherits from
// (css-pseudo-4/#first-letter-tree). Before the text is broken into lines, its first line ends at its first forced
// break.
void addLetterPart(Layout& t, const PrDomElement* text) {
    uint32_t lineEnd = 0;
    while (lineEnd < t.chars.count && !t.chars.data[lineEnd].forced)
        ++lineEnd;
    if (lineEnd == 0)
        return;
    const Char& last = t.chars.data[lineEnd - 1];
    size_t start = 0;
    size_t end = 0;
    if (!prTextFirstLetter(PrStringView{t.text.data, last.offset + last.length}, &start, &end))
        return;
    uint32_t first = 0;
    while (t.chars.data[first].offset < start)
        ++first;
    uint32_t span = t.chars.data[first].span;
    uint32_t after = first;
    for (; after < lineEnd && t.chars.data[after].offset < end; ++after) {
        if (t.chars.data[after].span != span)
            return;
    }
    const CssComputedStyle* letter =
        svgBuildPseudoStyleUnder(*t.b, text, CssPseudo_FirstLetter, t.spans.data[span].computed);
    if (letter == nullptr)
        return;
    Span part = t.spans.data[span];
    TextStyle parentStyle = part.style;
    part.style = textStyleOf(t, part.element, letter, &parentStyle);
    part.style.orientation = parentStyle.orientation;
    part.computed = letter;
    part.held = true;
    part.of = part.of != kArrayMissing ? part.of : span;
    part.parent = span;
    for (uint32_t i = first; i < after; ++i)
        t.chars.data[i].span = t.spans.count;
    t.spans.push(part);
}

// What setting the text as one line leaves on a character, which breaking it into lines changes.
struct CharPlace {
    float x;
    float y;
    bool hidden;
};

// Breaks the text into lines as setLines does, for a text whose first line or first letter has a style of its own. The
// first line is found with every character set as if on it. The characters after it go back to their elements' spans,
// with which the lines after it are found.
void setStyledLines(Layout& t, float width, RawArray<uint32_t>& lines) {
    RawArray<CharPlace> places;
    for (uint32_t i = 0; i < t.chars.count; ++i)
        places.push(CharPlace{t.chars.data[i].x, t.chars.data[i].y, t.chars.data[i].hidden});
    setLines(t, width, lines);
    uint32_t firstEnd = lines.data[1];
    for (uint32_t i = 0; i < t.chars.count; ++i) {
        Char& c = t.chars.data[i];
        c.x = places.data[i].x;
        c.y = places.data[i].y;
        c.hidden = places.data[i].hidden;
        if (i >= firstEnd && t.spans.data[c.span].of != kArrayMissing)
            c.span = t.spans.data[c.span].of;
    }
    places.reset();
    itemize(t);
    buildRuns(t);
    placeRuns(t);
    setLines(t, width, lines, firstEnd);
}

// Applies text-overflow to auto-wrapped text, whose wrapping area is `inlineSize` long, placed about its initial
// position by its text-anchor (SVG2/text.html#TextOverflowProperty). For clip, the text draws inside the area's inline
// extent. For ellipsis, an overflowing line hides the glyphs at its end that leave no room for an ellipsis, which
// follows the last glyph that remains, in the text element's style. The characters as recorded, and the bounds, are
// those of the text without it.
void overflowLines(Layout& t, const CssComputedStyle* style, float inlineSize, const RawArray<uint32_t>& lines) {
    PrCssKeyword overflow = svgStyleKeyword(style, PrCssProperty_TextOverflow);
    if (overflow != PrCssKeyword_Clip && overflow != PrCssKeyword_Ellipsis)
        return;
    const TextStyle& own = t.spans.data[0].style;
    float x = prF32IsNan(t.chars.data[0].x) ? 0 : t.chars.data[0].x;
    bool rtl = own.rightToLeft;
    float left;
    if (own.textAnchor == Anchor_Middle)
        left = x - inlineSize / 2;
    else if ((own.textAnchor == Anchor_Start) != rtl)
        left = x;
    else
        left = x - inlineSize;
    float right = left + inlineSize;
    if (overflow == PrCssKeyword_Clip) {
        constexpr float kUnbounded = 1e7f;
        t.out->clipped = true;
        t.out->clip = PrRect{left, -kUnbounded, right, kUnbounded};
        return;
    }
    constexpr float kTolerance = 0.01f;
    uint32_t glyphCount = t.glyphs.count;
    for (uint32_t l = 0; l + 1 < lines.count; ++l) {
        uint32_t lineStart = lines.data[l];
        uint32_t lineEnd = lines.data[l + 1];
        auto onLine = [&](const Glyph& glyph) {
            return !glyph.hidden && glyph.character >= lineStart && glyph.character < lineEnd;
        };
        bool overflows = false;
        for (uint32_t g = 0; g < glyphCount; ++g) {
            const Glyph& glyph = t.glyphs.data[g];
            if (onLine(glyph))
                overflows = overflows ||
                    (rtl ? glyph.pen.x < left - kTolerance : glyph.pen.x + glyph.advance > right + kTolerance);
        }
        if (!overflows)
            continue;
        // The ellipsis is a character of the text element, set on the line's baseline.
        uint32_t ci = t.chars.count;
        pushChar(t, 0x2026, 0);
        Char& c = t.chars.data[ci];
        const Char& head = t.chars.data[lineStart];
        c.level = rtl ? 1 : 0;
        c.script = head.script;
        Run run{.firstChar = ci, .endChar = ci + 1, .upright = false};
        run.font = fontFor(t, c, t.spans.data[0], PrStringView{t.text.data + c.offset, c.length});
        if (run.font.svg == nullptr && run.font.host == nullptr)
            continue;
        PrVec2 pen{0, prF32IsNan(head.y) ? 0 : head.y};
        run.firstGlyph = t.glyphs.count;
        t.kernFont = nullptr;
        if (run.font.svg != nullptr)
            setSvgRun(t, run, &pen, 0);
        else
            setHostRun(t, run, &pen, 0);
        run.endGlyph = t.glyphs.count;
        float width = pen.x;
        float limit = rtl ? left + width : right - width;
        float edge = rtl ? right : left;
        bool kept = false;
        for (uint32_t g = 0; g < glyphCount; ++g) {
            Glyph& glyph = t.glyphs.data[g];
            if (!onLine(glyph))
                continue;
            if (rtl ? glyph.pen.x < limit - kTolerance : glyph.pen.x + glyph.advance > limit + kTolerance) {
                glyph.hidden = true;
                continue;
            }
            float end = rtl ? glyph.pen.x : glyph.pen.x + glyph.advance;
            edge = !kept ? end : rtl ? prF32Min(edge, end) : prF32Max(edge, end);
            kept = true;
        }
        run.align = runAlign(t, run);
        float shift = run.align + runShift(t, run);
        float start = rtl ? edge - width : edge;
        for (uint32_t g = run.firstGlyph; g < run.endGlyph; ++g) {
            Glyph& glyph = t.glyphs.data[g];
            glyph.position.x += start;
            glyph.pen.x += start;
            glyph.position.y += shift;
            glyph.pen.y += shift;
            glyph.align = run.align;
        }
        t.runs.push(run);
    }
}

void layoutText(Layout& t, const PrDomElement* text, const CssComputedStyle* style) {
    beginLayout(t, text, style);
    applyPositions(t);
    if (t.vertical)
        intoFrame(t);
    measurePaths(t);
    // An inline-size gives the text a wrapping area of that width, or that height for vertical text
    // (SVG2/text.html#InlineSize). Zero creates none.
    float inlineSize = svgLengthResolve(svgStyleLength(style, PrCssProperty_InlineSize),
        percentBase(t, t.vertical ? SvgAxis_Y : SvgAxis_X));
    bool wraps = inlineSize > 0 && t.chars.count != 0;
    if (wraps)
        ignorePositions(t);
    // The text element's ::first-line and ::first-letter apply to it (SVG2/text.html#TextProperties). Text that is not
    // broken into lines is all on its first line.
    uint32_t spans = t.spans.count;
    if (t.chars.count != 0) {
        if (const CssComputedStyle* line = svgBuildPseudoStyleUnder(*t.b, text, CssPseudo_FirstLine, style))
            addLineParts(t, line);
        addLetterPart(t, text);
    }
    bool styled = t.spans.count != spans;
    arabicForms(t);
    itemize(t);
    buildRuns(t);
    // Text that wraps or holds forced line breaks is set as one line, broken into lines, and set again.
    RawArray<uint32_t> lines;
    if (wraps || hasForcedBreaks(t)) {
        placeRuns(t);
        if (styled)
            setStyledLines(t, wraps ? inlineSize : -1, lines);
        else
            setLines(t, wraps ? inlineSize : -1, lines);
        itemize(t);
        buildRuns(t);
    }
    placeRuns(t);
    if (!wraps)
        fitLengths(t);
    anchorChunks(t);
    followPaths(t);
    recordCharacters(t);
    t.out->bounds = textBounds(t);
    // text-overflow does not apply to text on a path.
    if (wraps && t.paths.count == 0)
        overflowLines(t, style, inlineSize, lines);
    record(t);
    if (t.vertical)
        toPage(t);
    lines.reset();
}

// Lays out an SVG Tiny 1.2 textArea, with its text broken into lines that fit its rectangle.
void layoutTextArea(Layout& t, const PrDomElement* area, const CssComputedStyle* style) {
    t.area = true;
    beginLayout(t, area, style);
    if (t.chars.count == 0)
        return;
    arabicForms(t);
    // The text is measured as one line and then broken into lines. Each line is a chunk that starts at the line's
    // start.
    itemize(t);
    buildRuns(t);
    placeRuns(t);
    RawArray<float> advances;
    RawArray<float> ascents;
    RawArray<float> descents;
    measureChars(t, &advances, &ascents, &descents);
    const TextStyle& own = t.spans.data[0].style;
    SvgLengthPercentage none{-1, 0};
    SvgLengthPercentage widthLength = svgAttributeLength(area, "width", none, style);
    SvgLengthPercentage heightLength = svgAttributeLength(area, "height", none, style);
    float x = svgLengthResolve(svgAttributeLength(area, "x", SvgLengthPercentage{}, style), percentBase(t, SvgAxis_X));
    float y = svgLengthResolve(svgAttributeLength(area, "y", SvgLengthPercentage{}, style), percentBase(t, SvgAxis_Y));
    float width = widthLength.px < 0 ? -1 : svgLengthResolve(widthLength, percentBase(t, SvgAxis_X));
    float height = heightLength.px < 0 ? -1 : svgLengthResolve(heightLength, percentBase(t, SvgAxis_Y));
    RawArray<uint32_t> starts;
    breakLines(t, advances, width, &starts);

    // Each line's baseline is a line-increment below the previous one, and the first is that far below the top. If
    // line-increment is auto, the increment is the line's largest font size
    // (SVGTiny12/text.html#LineIncrementProperty). A line whose baseline falls below the height is not drawn, and
    // neither is any line after it.
    RawArray<float> baselines;
    float baseline = y;
    float bottom = y;
    uint32_t lines = starts.count - 1;
    uint32_t shown = 0;
    for (uint32_t l = 0; l < lines; ++l) {
        float extent = 0;
        float descent = 0;
        // The size of a line comes from the characters that it draws. Its hidden line break and its spaces are
        // excluded, because they may belong to the element after them.
        for (uint32_t c = starts.data[l]; c < starts.data[l + 1]; ++c) {
            if (t.chars.data[c].hidden || t.codePoints.data[c] == ' ')
                continue;
            extent = prF32Max(extent, t.spans.data[t.chars.data[c].span].style.fontSize);
            descent = prF32Max(descent, descents.data[c]);
        }
        // A line that draws nothing, such as one between two tbreaks, is one em of its element high. The line-increment
        // comes from the element in which the previous line ended at its break. For the first line, it comes from the
        // element in which the line starts.
        uint32_t from = l == 0 ? starts.data[0] : starts.data[l] - 1;
        const TextStyle& lineStyle = t.spans.data[t.chars.data[from].span].style;
        if (extent == 0)
            extent = t.spans.data[t.chars.data[starts.data[l]].span].style.fontSize;
        float increment = lineStyle.lineIncrement;
        baseline += increment > 0 ? increment : extent;
        if (height >= 0 && baseline > y + height)
            break;
        baselines.push(baseline);
        bottom = baseline + descent;
        ++shown;
    }
    float shift = 0;
    if (height >= 0 && own.displayAlign != Anchor_Start)
        shift = (y + height - bottom) / (own.displayAlign == Anchor_Middle ? 2 : 1);
    // Each line starts where text-align places it, with its text anchored there.
    Anchor align = own.textAlign;
    if (own.rightToLeft && align != Anchor_Middle)
        align = align == Anchor_Start ? Anchor_End : Anchor_Start;
    float start = width < 0 || align == Anchor_Start ? x : align == Anchor_Middle ? x + width / 2 : x + width;
    Anchor textAlign = own.textAlign;
    for (uint32_t i = 0; i < t.spans.count; ++i) {
        Span& span = t.spans.data[i];
        span.style.textAnchor = textAlign;
    }
    for (uint32_t l = 0; l < lines; ++l) {
        for (uint32_t c = starts.data[l]; c < starts.data[l + 1]; ++c) {
            t.chars.data[c].hidden = t.chars.data[c].hidden || l >= shown;
            t.chars.data[c].x = t.chars.data[c].y = kPrF32Nan;
        }
        Char& head = t.chars.data[starts.data[l]];
        head.x = start;
        head.y = l < shown ? baselines.data[l] + shift : y;
    }
    itemize(t);
    buildRuns(t);
    placeRuns(t);
    anchorChunks(t);
    t.out->bounds = textBounds(t);
    record(t);
    recordCharacters(t);
    advances.reset();
    ascents.reset();
    descents.reset();
    starts.reset();
    baselines.reset();
}

} // namespace

SvgNodeIndex svgBuildText(SvgBuilder& b, const PrDomElement* element, const CssComputedStyle* style,
        SvgNodeIndex parent, bool instance) {
    PrCssKeyword visibility = svgStyleKeyword(style, PrCssProperty_Visibility);
    SvgNode own{
        .kind = SvgNodeKind_Text,
        .visible = visibility != PrCssKeyword_Hidden && visibility != PrCssKeyword_Collapse,
        .fillRule = svgStyleFillRule(style, PrCssProperty_FillRule),
        .clipRule = svgStyleFillRule(style, PrCssProperty_ClipRule),
        .opacity = prF32Clamp(svgStyleNumber(style, PrCssProperty_Opacity, 1), 0, 1),
        .pointerEvents = svgStylePointerEvents(style),
        .element = element,
    };
    SvgTransformStyle transform = svgStyleTransform(style);
    own.transform = svgTransformMatrix(transform, PrRect{});
    own.transformStyle = kArrayMissing;
    // The text paints its pieces, each with the paints of its element.
    own.paints = SvgPaints{
        .fill = {.server = kSvgResourceNone},
        .stroke = {.server = kSvgResourceNone},
        .strokeStyle = kArrayMissing,
    };
    own.markers = kArrayMissing;
    own.effects = kArrayMissing;
    own.text = allocZeroed<SvgText>();
    SvgNodeIndex node = svgBuildNode(b, own, parent, svgBuildsOwn(b, instance));
    svgBuildEffects(b, node, style);
    Layout t{};
    t.b = &b;
    t.instance = instance;
    t.out = b.tree->nodes.data[node].text;
    t.node = node;
    if (svgIs(element, PrDomTag_TextArea))
        layoutTextArea(t, element, style);
    else
        layoutText(t, element, style);
    layoutReset(t);
    return node;
}

void svgDrawText(SvgDrawer& d, const SvgNode& node) {
    const SvgText* text = node.text;
    if (text->clipped)
        prDrawListPushClipRect(d.list, text->clip, PrDrawEdges_Antialiased);
    for (uint32_t i = 0; i < text->pieces.count; ++i) {
        const SvgTextPiece& piece = text->pieces.data[i];
        if (piece.path != nullptr) {
            svgPaintShape(d, piece.paints, true, text->bounds, -1,
                [&](PrDrawPaint paint) { prDrawListPath(d.list, piece.path, piece.rule, paint); }, [] {});
            continue;
        }
        PrFontGlyphRun glyphs{};
        glyphs.font = piece.font;
        glyphs.size = piece.size;
        glyphs.glyphs = text->glyphs.data + piece.firstGlyph;
        glyphs.glyphCount = piece.glyphCount;
        glyphs.transforms = piece.transformed ? text->transforms.data + piece.firstGlyph : nullptr;
        svgPaintShape(d, piece.paints, true, text->bounds, -1,
            [&](PrDrawPaint paint) { prDrawListGlyphs(d.list, &glyphs, PrVec2{}, paint); }, [] {});
    }
    if (text->clipped)
        prDrawListPopClip(d.list);
}

PrRect svgTextBounds(const SvgText* text) {
    return text->bounds;
}

uint32_t svgTextCharacterCount(const SvgText* text) {
    return text->characters.count;
}

const SvgTextCharacter* PR_NULLABLE svgTextCharacters(const SvgText* text) {
    return text->characters.data;
}

uint32_t svgTextElementCount(const SvgText* text) {
    return text->elements.count;
}

const SvgTextElement* PR_NULLABLE svgTextElements(const SvgText* text) {
    return text->elements.data;
}

const SvgTextElement* PR_NULLABLE svgTextElementFromElement(const SvgText* text, const PrDomElement* element) {
    for (uint32_t i = 0; i < text->elements.count; ++i) {
        if (text->elements.data[i].element == element)
            return &text->elements.data[i];
    }
    return nullptr;
}

uint32_t svgTextPaintsCount(const SvgText* text) {
    return text->paints.count;
}

const SvgPaints* PR_NULLABLE svgTextPaints(const SvgText* text) {
    return text->paints.data;
}

bool svgTextSame(const SvgTree* tree, const SvgText* text, const SvgTree* expectedTree, const SvgText* expected,
        PrTextBuffer* difference) {
    if (!prMemEquals(&text->bounds, &expected->bounds, sizeof(PrRect))) {
        prTextBufferAppend(difference, "text bounds differ");
        return false;
    }
    if (text->clipped != expected->clipped ||
            (text->clipped && !prMemEquals(&text->clip, &expected->clip, sizeof(PrRect)))) {
        prTextBufferAppend(difference, "text clips differ");
        return false;
    }
    bool sameGlyphs = text->glyphs.count == expected->glyphs.count &&
        text->transforms.count == expected->transforms.count &&
        prMemEquals(text->transforms.data, expected->transforms.data, text->transforms.count * sizeof(PrAffine2));
    // A glyph is compared by its fields, since its id leaves padding before its position.
    for (uint32_t i = 0; sameGlyphs && i < text->glyphs.count; ++i) {
        const PrFontGlyph& a = text->glyphs.data[i];
        const PrFontGlyph& e = expected->glyphs.data[i];
        sameGlyphs = a.id == e.id && prMemEquals(&a.position, &e.position, sizeof(PrVec2));
    }
    if (!sameGlyphs) {
        prTextBufferAppend(difference, "glyphs differ");
        return false;
    }
    size_t characterBytes = text->characters.count * sizeof(SvgTextCharacter);
    bool sameCharacters = text->characters.count == expected->characters.count &&
        prMemEquals(text->characters.data, expected->characters.data, characterBytes);
    if (!sameCharacters) {
        prTextBufferAppend(difference, "characters differ");
        return false;
    }
    bool sameElements = text->elements.count == expected->elements.count;
    for (uint32_t i = 0; sameElements && i < text->elements.count; ++i) {
        const SvgTextElement& a = text->elements.data[i];
        const SvgTextElement& e = expected->elements.data[i];
        sameElements = a.start == e.start && a.end == e.end && a.pointerEvents == e.pointerEvents &&
            a.visible == e.visible && a.painted == e.painted;
    }
    if (!sameElements) {
        prTextBufferAppend(difference, "the elements of the text differ");
        return false;
    }
    if (text->pieces.count != expected->pieces.count) {
        prTextBufferAppend(difference, "text pieces differ in number");
        return false;
    }
    for (uint32_t i = 0; i < text->pieces.count; ++i) {
        const SvgTextPiece& a = text->pieces.data[i];
        const SvgTextPiece& e = expected->pieces.data[i];
        bool same = a.rule == e.rule && a.font == e.font && a.size == e.size && a.firstGlyph == e.firstGlyph &&
            a.glyphCount == e.glyphCount && a.transformed == e.transformed &&
            (a.path == nullptr) == (e.path == nullptr) && (a.path == nullptr || svgPathSame(a.path, e.path));
        if (!same) {
            prTextBufferAppend(difference, "a text piece differs");
            return false;
        }
        if (!svgPaintsSame(tree, a.paints, expectedTree, e.paints, difference))
            return false;
    }
    return true;
}

void svgTextDestroy(SvgTree* tree, SvgText* PR_NULLABLE text) {
    if (text == nullptr)
        return;
    for (uint32_t i = 0; i < text->paints.count; ++i)
        svgTreeFreeStroke(tree, text->paints.data[i]);
    text->paints.reset();
    for (uint32_t i = 0; i < text->pieces.count; ++i) {
        SvgTextPiece& piece = text->pieces.data[i];
        prPathDestroy(piece.path);
        prFontRelease(piece.font);
    }
    text->pieces.reset();
    text->glyphs.reset();
    text->transforms.reset();
    text->characters.reset();
    text->elements.reset();
    prMemoryFree(text);
}

void svgTextFontsDestroy(SvgTextFonts* PR_NULLABLE fonts) {
    if (fonts == nullptr)
        return;
    fonts->loaded.reset();
    for (uint32_t i = 0; i < fonts->hosts.count; ++i) {
        SvgHostFont& host = fonts->hosts.data[i];
        host.key.reset();
        prTextFontRelease(host.font);
    }
    fonts->hosts.reset();
    prTextGlyphBufferDestroy(fonts->glyphs);
    prMemoryFree(fonts);
}

} // namespace pr_impl
