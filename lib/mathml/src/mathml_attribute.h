// MathML's attributes, parsed by their grammars
//
// mathml-core - https://w3c.github.io/mathml-core/
// mathml4 - https://w3c.github.io/mathml/
//
// Each function reads an element's attributes and returns their values as MathML Core defines them. Where Core accepts
// a subset of MathML 4's grammar, the functions accept MathML 4's, so a length may also be a named space
// (mathml4/#type_length) and a fraction's line thickness may be a keyword. A value that does not parse is treated as
// absent, as both specifications say for every attribute that layout reads. Keywords and booleans match ASCII
// case-insensitively.
//
// MathML 4 lets an enclosing mstyle or math element give the default of an attribute that is not inherited, such as an
// mfrac's linethickness or an mo's stretchy (mathml4/#presm_mstyle). The functions read such defaults
// (mathMlInheritedValue), so a change to an attribute of an mstyle reaches the elements under it that read it
// (mathMlInherits).

#pragma once

#include "pr/dom.h"
#include "pr/string_view.h"
#include "pr/unicode.h"

#include "dom/src/dom_node.h"
#include "mathml/src/mathml_dictionary.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Values

// The value of a boolean attribute (mathml-core/#dfn-boolean).
enum {
    MathMlBoolean_None,         // absent or invalid
    MathMlBoolean_False,
    MathMlBoolean_True,
};
typedef uint8_t MathMlBoolean;

// The units of a term of a length. The absolute CSS units are converted to px when an attribute is parsed. The others
// are relative, and resolve where the element's style is known.
enum {
    MathMlUnit_Px,
    MathMlUnit_Em,
    MathMlUnit_Ex,
    MathMlUnit_Ch,
    MathMlUnit_Ic,
    MathMlUnit_Cap,
    MathMlUnit_Rem,
    MathMlUnit_Lh,
    MathMlUnit_Rlh,
    MathMlUnit_Vw,
    MathMlUnit_Vh,
    MathMlUnit_Vmin,
    MathMlUnit_Vmax,
    MathMlUnit_Percent,         // a percentage of a base that the attribute's definition names
    MathMlUnit_Width,           // a multiple of the content's width, an mpadded pseudo-unit (mathml4/#presm_mpadded)
    MathMlUnit_Height,          // a multiple of the content's height
    MathMlUnit_Depth,           // a multiple of the content's depth
    MathMlUnit_Count,
};
typedef uint8_t MathMlUnit;

// The most terms that a length holds: one of each unit, since the terms of a unit add up into one.
constexpr uint32_t kMathMlLengthMaxTerms = MathMlUnit_Count;

// What a length is beyond its terms. An increment or a decrement changes the default value instead of replacing it, as
// a leading + or - does in MathML 3's mpadded (mathml4/#presm_mpadded).
enum {
    MathMlLengthFlag_Increment = 1u << 0,
    MathMlLengthFlag_Decrement = 1u << 1,
    MathMlLengthFlag_Infinite = 1u << 2,        // an mo's maxsize of infinity, which MathML 4 deprecates
};
typedef uint8_t MathMlLengthFlags;

// A length that an attribute gives, the sum of its terms, each a number of a unit. A length with no terms that is not
// infinite is absent, because the attribute is absent or its value does not parse.
struct MathMlLength {
    float values[kMathMlLengthMaxTerms];
    MathMlUnit units[kMathMlLengthMaxTerms];
    uint8_t count;
    MathMlLengthFlags flags;
};

// Adds `value` of `unit` to the length, to its term of that unit if it has one.
void mathMlLengthAdd(MathMlLength* length, double value, MathMlUnit unit);

// Returns true if the length is neither absent nor invalid.
inline bool mathMlLengthIsPresent(const MathMlLength& length) {
    return length.count != 0 || (length.flags & MathMlLengthFlag_Infinite) != 0;
}

// Returns true if a term of the length is a percentage, which MathML Core treats as absent in the attributes whose
// percentages it leaves undefined, such as mspace's height (mathml-core/#space-mspace).
bool mathMlLengthHasPercent(const MathMlLength& length);

// Returns true if a term of the length is a pseudo-unit.
bool mathMlLengthHasPseudoUnit(const MathMlLength& length);

// The forms of length that an attribute accepts beyond a <length-percentage> and a named space (mathml4/#type_length),
// which every length attribute accepts.
//
// A unitless number is a multiple of the reference value, so 2 means 200%, as MathML 4 reads an mo's minsize and
// maxsize (mathml4/#presm_mo) and MathML 3 reads a fraction's linethickness. MathML 3's mpadded syntax, which legacy
// content writes, lets a pseudo-unit follow a number or a percentage (mathml4/#presm_mpadded). In that syntax, a
// leading
// + makes an increment, and a leading - makes a decrement of a length with a pseudo-unit. A leading - before any other
// length makes a negative length, as MathML Core reads it.
enum {
    MathMlLengthSyntax_Normal = 0,
    MathMlLengthSyntaxFlag_Unitless = 1u << 0,  // a number without a unit
    MathMlLengthSyntaxFlag_Pseudo = 1u << 1,    // MathML 3's mpadded syntax
    MathMlLengthSyntaxFlag_Infinity = 1u << 2,  // the keyword infinity (mathml4/#presm_mo)
};
typedef uint8_t MathMlLengthSyntax;

// Parses `text` as a length in `syntax`. Returns false, writing an absent length, if the text does not parse. The value
// is a <length-percentage> with its absolute units in px, a calc() of a sum of such values, or a named space in ems.
bool mathMlParseLength(PrStringView text, MathMlLengthSyntax syntax, MathMlLength* out);

// Parses a boolean (mathml-core/#dfn-boolean).
MathMlBoolean mathMlParseBoolean(PrStringView text);

// Parses an unsigned-integer (mathml-core/#dfn-unsigned-integer), an integer whose first character is a digit. Returns
// false if the text is not one or overflows.
bool mathMlParseUnsigned(PrStringView text, uint32_t* out);

// Parses an integer, an optional - followed by an unsigned-integer (mathml4/#fund_attval).
bool mathMlParseInteger(PrStringView text, int32_t* out);

// Parses a scriptlevel (mathml-core/#dfn-scriptlevel), which is +U, -U or U for an unsigned-integer U. Writes the
// number, which is negative for -U, and whether it is relative, as +U and -U are.
bool mathMlParseScriptLevel(PrStringView text, int32_t* level, bool* relative);

// Returns the item of a whitespace-separated list at `index`, or its last item if it has fewer, as MathML's table
// attributes repeat their last value (mathml4/#presm_mtable). Returns the null view for an empty list.
PrStringView mathMlListItem(PrStringView text, uint32_t index);

// Returns the number of items of a whitespace-separated list.
uint32_t mathMlListLength(PrStringView text);

// ---------------------------------------------------------------------------------------------------------------------
// Attributes

// Returns the value of the attribute `name` of the element, in no namespace, or the null view if it has none.
PrStringView mathMlAttributeValue(const PrDomElement* element, PrStringView name);

// Returns true if an enclosing mstyle or math element gives the default of the attribute `name` of the elements under
// it (mathml4/#presm_mstyle), so that a change to it on an mstyle changes what its descendants read.
bool mathMlInherits(PrStringView name);

// Returns the value of the attribute `name` of the element. If the element has none and the attribute inherits
// (mathMlInherits), returns the value of the nearest enclosing mstyle or math element that has it, up to and including
// the math element that holds the element. Returns the null view if none has it.
PrStringView mathMlInheritedValue(const PrDomElement* element, PrStringView name);

// Returns the boolean that the attribute `name` of the element gives, inherited as mathMlInheritedValue reads it.
MathMlBoolean mathMlBooleanAttribute(const PrDomElement* element, PrStringView name);

// Returns the length that the attribute `name` of the element gives in `syntax`, inherited as mathMlInheritedValue
// reads it, or an absent length.
MathMlLength mathMlLengthAttribute(const PrDomElement* element, PrStringView name, MathMlLengthSyntax syntax);

// ---------------------------------------------------------------------------------------------------------------------
// Alignment and lines

// A horizontal alignment of a fraction's numerator or denominator, an underscript or an overscript, a table's columns,
// or a stack's characters. None means the attribute's default.
enum {
    MathMlAlign_None,
    MathMlAlign_Left,
    MathMlAlign_Center,
    MathMlAlign_Right,
    MathMlAlign_DecimalPoint,   // a stack's alignment on its decimal character (mathml4/#presm_mstack)
};
typedef uint8_t MathMlAlign;

// A vertical alignment of a table or a stack within its environment, or of a table's rows (mathml4/#presm_mtable).
enum {
    MathMlVerticalAlign_None,
    MathMlVerticalAlign_Top,
    MathMlVerticalAlign_Bottom,
    MathMlVerticalAlign_Center,
    MathMlVerticalAlign_Baseline,
    MathMlVerticalAlign_Axis,
};
typedef uint8_t MathMlVerticalAlign;

// A line that a table draws between its rows or columns, or around itself (mathml4/#presm_mtable).
enum {
    MathMlLine_None,
    MathMlLine_Solid,
    MathMlLine_Dashed,
};
typedef uint8_t MathMlLine;

// ---------------------------------------------------------------------------------------------------------------------
// Operators

// The attributes of an mo that the dictionary would otherwise decide (mathml-core/#dictionary-based-attributes), as the
// element and its enclosing mstyle elements give them. `present` holds which of the boolean attributes are present, and
// `flags` holds their values.
struct MathMlOperatorAttributes {
    bool hasForm;
    MathMlForm form;
    MathMlOperatorFlags flags;
    MathMlOperatorFlags present;
    MathMlLength lspace;
    MathMlLength rspace;
    MathMlLength minsize;
    MathMlLength maxsize;

    // The accent attribute, which applies when the operator is a script of munder, mover or munderover
    // (mathml4/#presm_mo).
    MathMlBoolean accent;
};

// Returns the attributes of the mo `element`.
MathMlOperatorAttributes mathMlOperatorAttributes(const PrDomElement* element);

// The desirability of a line break at an mo or an mspace (mathml4/#presm_linebreaking).
enum {
    MathMlLineBreak_Auto,
    MathMlLineBreak_Newline,
    MathMlLineBreak_NoBreak,
    MathMlLineBreak_GoodBreak,
    MathMlLineBreak_BadBreak,
};
typedef uint8_t MathMlLineBreak;

// Where an operator goes when a line breaks at it (mathml4/#presm_mo). None means the dictionary's default, which is
// Before.
enum {
    MathMlLineBreakStyle_None,
    MathMlLineBreakStyle_Before,
    MathMlLineBreakStyle_After,
    MathMlLineBreakStyle_Duplicate,
    MathMlLineBreakStyle_Infix,     // the infixlinebreakstyle of the enclosing mstyle
};
typedef uint8_t MathMlLineBreakStyle;

// The horizontal position of the lines after a line break (mathml4/#presm_mo, indentation attributes).
enum {
    MathMlIndentAlign_None,
    MathMlIndentAlign_Left,
    MathMlIndentAlign_Center,
    MathMlIndentAlign_Right,
    MathMlIndentAlign_Auto,
    MathMlIndentAlign_Id,           // at the element that indenttarget names
    MathMlIndentAlign_IndentAlign,  // as indentalign says, for the first or the last line
};
typedef uint8_t MathMlIndentAlign;

// How a line breaks at an mo or an mspace, and how the lines after it are indented, with the defaults that enclosing
// mstyle elements give (mathml4/#presm_linebreaking). Absent lengths and views mean the attributes' defaults.
struct MathMlLineBreaking {
    MathMlLineBreak lineBreak;
    MathMlLineBreakStyle style;
    MathMlIndentAlign indentAlign;
    MathMlIndentAlign indentAlignFirst;
    MathMlIndentAlign indentAlignLast;
    MathMlLength lineLeading;
    MathMlLength indentShift;
    MathMlLength indentShiftFirst;
    MathMlLength indentShiftLast;
    PrStringView multChar;          // linebreakmultchar, the character that shows an invisible times at a break
    PrStringView indentTarget;      // the id that indentalign=id aligns at
};

// Returns how a line breaks at the mo or mspace `element`.
MathMlLineBreaking mathMlLineBreaking(const PrDomElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// Layout schemata

// The attributes of an mspace (mathml-core/#space-mspace).
struct MathMlSpace {
    MathMlLength width;
    MathMlLength height;
    MathMlLength depth;
};

// Returns the attributes of the mspace `element`.
MathMlSpace mathMlSpace(const PrDomElement* element);

// The attributes of an mfrac (mathml-core/#fractions-mfrac, mathml4/#presm_mfrac). An absent line thickness is the
// font's FractionRuleThickness. A percentage is of that thickness, and MathML 4's thin, medium and thick are 50%, 100%
// and 200%.
struct MathMlFraction {
    MathMlLength lineThickness;
    MathMlAlign numeratorAlign;
    MathMlAlign denominatorAlign;
    bool bevelled;
};

// Returns the attributes of the mfrac `element`.
MathMlFraction mathMlFraction(const PrDomElement* element);

// The attributes of an mpadded (mathml-core/#adjust-space-around-content-mpadded). The lengths keep MathML 3's
// pseudo-units and increments (MathMlLengthSyntaxFlag_Pseudo) for layout to apply.
struct MathMlPadded {
    MathMlLength width;
    MathMlLength height;
    MathMlLength depth;
    MathMlLength lspace;
    MathMlLength voffset;
};

// Returns the attributes of the mpadded `element`.
MathMlPadded mathMlPadded(const PrDomElement* element);

// The attributes of an munder, mover or munderover (mathml-core/#underscripts-and-overscripts-munder-mover-munderover,
// mathml4/#presm_munderover). An absent accent is false in MathML Core, and is the core operator's accent property in
// MathML 4.
struct MathMlUnderOver {
    MathMlBoolean accent;
    MathMlBoolean accentUnder;
    MathMlAlign align;              // the alignment of the scripts over the base, centered if absent
};

// Returns the attributes of the munder, mover or munderover `element`.
MathMlUnderOver mathMlUnderOver(const PrDomElement* element);

// The notations of an menclose (mathml4/#presm_menclose). An menclose with none draws nothing.
enum {
    MathMlNotationFlag_LongDiv = 1u << 0,
    MathMlNotationFlag_Actuarial = 1u << 1,
    MathMlNotationFlag_PhasorAngle = 1u << 2,
    MathMlNotationFlag_Radical = 1u << 3,
    MathMlNotationFlag_Box = 1u << 4,
    MathMlNotationFlag_RoundedBox = 1u << 5,
    MathMlNotationFlag_Circle = 1u << 6,
    MathMlNotationFlag_Left = 1u << 7,
    MathMlNotationFlag_Right = 1u << 8,
    MathMlNotationFlag_Top = 1u << 9,
    MathMlNotationFlag_Bottom = 1u << 10,
    MathMlNotationFlag_UpDiagonalStrike = 1u << 11,
    MathMlNotationFlag_DownDiagonalStrike = 1u << 12,
    MathMlNotationFlag_VerticalStrike = 1u << 13,
    MathMlNotationFlag_HorizontalStrike = 1u << 14,
    MathMlNotationFlag_Madruwb = 1u << 15,
    MathMlNotationFlag_Text = 1u << 16,
    MathMlNotationFlag_UpArrow = 1u << 17,
    MathMlNotationFlag_RightArrow = 1u << 18,
    MathMlNotationFlag_DownArrow = 1u << 19,
    MathMlNotationFlag_LeftArrow = 1u << 20,
    MathMlNotationFlag_NorthWestArrow = 1u << 21,
    MathMlNotationFlag_SouthWestArrow = 1u << 22,
    MathMlNotationFlag_SouthEastArrow = 1u << 23,
    MathMlNotationFlag_NorthEastArrow = 1u << 24,
    MathMlNotationFlag_UpDownArrow = 1u << 25,
    MathMlNotationFlag_LeftRightArrow = 1u << 26,
    MathMlNotationFlag_NorthWestSouthEastArrow = 1u << 27,
    MathMlNotationFlag_NorthEastSouthWestArrow = 1u << 28,
};
typedef uint32_t MathMlNotationFlags;

// Returns the notations of the menclose `element`. A notation that the engine does not know is ignored.
MathMlNotationFlags mathMlNotations(const PrDomElement* element);

// Returns the opening or closing fence of the mfenced `element` (mathml4/#presm_mfenced). It is the open or close
// attribute with white space trimmed, or "(" or ")" if the attribute is absent. An empty fence draws nothing.
PrStringView mathMlFencedOpen(const PrDomElement* element);
PrStringView mathMlFencedClose(const PrDomElement* element);

// Returns the separator of the mfenced `element` after its argument at `index`, or 0 if there is none. The separators
// attribute lists characters, ignoring white space, and its last character repeats. The default is a comma.
uint32_t mathMlFencedSeparator(const PrDomElement* element, uint32_t index);

// Returns the columns and the rows that the mtd `element` spans (mathml-core/#entry-in-table-or-matrix-mtd), as HTML
// parses a td's colspan and rowspan (html/#attributes-common-to-td-and-th-elements). The column span is from 1 to 1000.
// The row span is from 0 to 65534, and 0 spans the rest of the row group.
void mathMlCellSpans(const PrDomElement* element, uint32_t* columns, uint32_t* rows);

// ---------------------------------------------------------------------------------------------------------------------
// Tables

// The vertical alignment of a table or a stack within its environment (mathml4/#presm_mtable). `row` is the row that it
// aligns by, counted from 1 at the top or from -1 at the bottom, or 0 for the table as a whole.
struct MathMlTableAlign {
    MathMlVerticalAlign align;
    int32_t row;
};

// Returns the alignment of the mtable or mstack `element` (its align attribute). The default is axis for a table and
// baseline for a stack.
MathMlTableAlign mathMlTableAlign(const PrDomElement* element);

// Returns the vertical alignment of the cell `element` (an mtd). It is the cell's rowalign, or else its row's, or else
// the item of its table's rowalign for the row at `row`, which an mstyle may give, or else baseline.
MathMlVerticalAlign mathMlCellRowAlign(const PrDomElement* element, uint32_t row);

// Returns the horizontal alignment of the cell `element` (an mtd) in the column at `column`. It is the cell's
// columnalign, or else the item of its row's for the column, or else its table's, which an mstyle may give, or else
// center.
MathMlAlign mathMlCellColumnAlign(const PrDomElement* element, uint32_t column);

// The attributes of an mtable that space and rule its rows and columns (mathml4/#presm_mtable). The lists are read by
// index (mathMlTableRowSpacing and the like).
struct MathMlTable {
    MathMlLine frame;
    MathMlLength frameSpacing[2];   // around the frame, inline then block, used if the frame is drawn
    bool equalRows;
    bool equalColumns;
    MathMlLength width;             // absent for auto
    MathMlBoolean displayStyle;

    // Where an mlabeledtr's label goes (mathml4/#presm_mtr), and the least space between the label and the row.
    PrStringView side;
    MathMlLength minLabelSpacing;
};

// Returns the attributes of the mtable `element`.
MathMlTable mathMlTable(const PrDomElement* element);

// Returns the space of the mtable `element` between the rows at `gap` and `gap + 1`, or between the columns. An absent
// length means the default, 1ex between rows and 0.8em between columns.
MathMlLength mathMlTableRowSpacing(const PrDomElement* element, uint32_t gap);
MathMlLength mathMlTableColumnSpacing(const PrDomElement* element, uint32_t gap);

// Returns the line of the mtable `element` between the rows at `gap` and `gap + 1`, or between the columns.
MathMlLine mathMlTableRowLine(const PrDomElement* element, uint32_t gap);
MathMlLine mathMlTableColumnLine(const PrDomElement* element, uint32_t gap);

// Returns the number of items of the longest of the lists of the mtable `element` that are read by a row's index
// (rowalign, rowspacing and rowlines). Each list repeats its last item, so the rows from this index on read the same
// items.
uint32_t mathMlTableRowItems(const PrDomElement* element);

// The width of a table's column (mathml4/#presm_mtable, columnwidth).
enum {
    MathMlColumnWidth_Auto,         // as wide as its content
    MathMlColumnWidth_Fit,          // a share of the width that the other columns leave
    MathMlColumnWidth_Length,       // `length`, whose percentage is of the table's width
};
typedef uint8_t MathMlColumnWidth;

// Returns the width of the column at `column` of the mtable `element`, and writes its length if it has one.
MathMlColumnWidth mathMlTableColumnWidth(const PrDomElement* element, uint32_t column, MathMlLength* length);

// ---------------------------------------------------------------------------------------------------------------------
// Elementary math

// The attributes of an mstack or mlongdiv that align its rows and columns (mathml4/#presm_mstack).
struct MathMlStack {
    MathMlTableAlign align;
    MathMlAlign stackAlign;         // decimal point if absent
    MathMlAlign charAlign;          // right if absent
    MathMlLength charSpacing;       // a length, or the percentage of medium spacing that loose or tight stands for
    uint32_t decimalCharacter;      // the decimalcharacter, inherited from an mstyle, or a period by default
};

// Returns the attributes of the mstack or mlongdiv `element`.
MathMlStack mathMlStack(const PrDomElement* element);

// The styles of long division (mathml4/#presm_mlongdiv), lefttop by default.
enum {
    MathMlLongDivStyle_LeftTop,
    MathMlLongDivStyle_StackedRightRight,
    MathMlLongDivStyle_MediumStackedRightRight,
    MathMlLongDivStyle_ShortStackedRightRight,
    MathMlLongDivStyle_RightTop,
    MathMlLongDivStyle_LeftSlashRight,       // left/\right
    MathMlLongDivStyle_LeftParenRight,       // left)(right
    MathMlLongDivStyle_ColonRightEqualRight, // :right=right
    MathMlLongDivStyle_StackedLeftLeft,
    MathMlLongDivStyle_StackedLeftLineTop,
};
typedef uint8_t MathMlLongDivStyle;

// Returns the style of the mlongdiv `element`.
MathMlLongDivStyle mathMlLongDivStyle(const PrDomElement* element);

// Returns the position of the msgroup, msrow, mscarries or msline `element`, and the shift of an msgroup, in columns
// (mathml4/#presm_msgroup). Each is 0 if absent.
int32_t mathMlStackPosition(const PrDomElement* element);
int32_t mathMlStackShift(const PrDomElement* element);

// Where a carry sits beside its column's character, as a compass direction (mathml4/#presm_mscarries).
enum {
    MathMlCarryLocation_None,
    MathMlCarryLocation_W,
    MathMlCarryLocation_Nw,
    MathMlCarryLocation_N,
    MathMlCarryLocation_Ne,
    MathMlCarryLocation_E,
    MathMlCarryLocation_Se,
    MathMlCarryLocation_S,
    MathMlCarryLocation_Sw,
};
typedef uint8_t MathMlCarryLocation;

// How the character under a carry is crossed out (mathml4/#presm_mscarries).
enum {
    MathMlCrossoutFlag_UpDiagonalStrike = 1u << 0,
    MathMlCrossoutFlag_DownDiagonalStrike = 1u << 1,
    MathMlCrossoutFlag_VerticalStrike = 1u << 2,
    MathMlCrossoutFlag_HorizontalStrike = 1u << 3,
};
typedef uint8_t MathMlCrossoutFlags;

// The attributes of an mscarry with the defaults that its mscarries gives, or of an mscarries itself. The default
// location is north.
struct MathMlCarry {
    MathMlCarryLocation location;
    MathMlCrossoutFlags crossout;
    float scriptSizeMultiplier;     // the scriptsizemultiplier of the mscarries, 0.6 by default
};

// Returns the attributes of the mscarry or mscarries `element`.
MathMlCarry mathMlCarry(const PrDomElement* element);

// The attributes of an msline (mathml4/#presm_msline).
struct MathMlStackLine {
    uint32_t length;                // the number of columns that the line spans, or 0 for every column
    MathMlLength leftOverhang;
    MathMlLength rightOverhang;
    MathMlLength thickness;         // a percentage of the default rule thickness, as for a fraction's line thickness
};

// Returns the attributes of the msline `element`.
MathMlStackLine mathMlStackLine(const PrDomElement* element);

// ---------------------------------------------------------------------------------------------------------------------
// Other elements

// The attributes of an mglyph, an image that stands for a character, with the size and the baseline shift of MathML 3.
struct MathMlGlyph {
    PrStringView src;
    PrStringView alt;
    MathMlLength width;
    MathMlLength height;
    MathMlLength valign;
};

// Returns the attributes of the mglyph `element`.
MathMlGlyph mathMlGlyph(const PrDomElement* element);

// Writes to `out` the mathematical alphanumeric style that the mathvariant attribute of the token `element`, or of an
// enclosing mstyle, asks for (mathml4/#presm_commatt). Returns false if the variant is normal, absent or invalid, which
// leaves the text as CSS transforms it.
bool mathMlTokenVariant(const PrDomElement* element, PrUnicodeMathVariant* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
