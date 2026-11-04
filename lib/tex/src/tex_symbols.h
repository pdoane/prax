// TeX math's control sequences, its fonts as Unicode's mathematical alphanumerics, and MathML Core's operator
// dictionary.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

#include "tex/src/tex_dictionary.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// TeX's classes of atom, which decide the space between neighbors (The TeXbook, chapter 18 and Appendix G).
enum {
    TexClass_Ord,
    TexClass_Op,
    TexClass_Bin,
    TexClass_Rel,
    TexClass_Open,
    TexClass_Close,
    TexClass_Punct,
    TexClass_Inner,
    TexClass_Middle,                // a \middle delimiter: Close to the atom before it and Open to the atom after it
    TexClass_None,                  // not an atom: glue and kerns, which leave the atoms on either side neighbors
};
typedef uint8_t TexClass;

// TeX's four styles. MathML expresses the cramped styles with math-shift, which its layout derives.
enum {
    TexStyle_Display,
    TexStyle_Text,
    TexStyle_Script,
    TexStyle_ScriptScript,
};
typedef uint8_t TexStyle;

// The fonts of math, which are TeX's math italic and the alphabets of Unicode's Mathematical Alphanumeric Symbols.
enum {
    TexFont_None,                   // math italic, with uppercase Greek and digits upright
    TexFont_Normal,                 // \mathnormal, math italic that includes uppercase Greek
    TexFont_Roman,
    TexFont_Bold,
    TexFont_Italic,
    TexFont_BoldItalic,
    TexFont_Script,                 // \mathscr, the script alphabet with roundhand capitals
    TexFont_BoldScript,
    TexFont_Calligraphic,           // \mathcal, the script alphabet with chancery capitals
    TexFont_BoldCalligraphic,
    TexFont_Fraktur,
    TexFont_BoldFraktur,
    TexFont_DoubleStruck,
    TexFont_SansSerif,
    TexFont_SansSerifBold,
    TexFont_SansSerifItalic,
    TexFont_SansSerifBoldItalic,
    TexFont_Monospace,
};
typedef uint8_t TexFont;

// What a control sequence is.
enum {
    TexEntryKind_Letter,            // an <mi>, italic if MathML's math-auto makes it italic
    TexEntryKind_Upright,           // an upright <mi>
    TexEntryKind_Operator,          // an <mo> of the entry's class
    TexEntryKind_LargeOperator,     // an <mo> of class Op whose scripts are limits in display style
    TexEntryKind_Integral,          // an <mo> of class Op whose scripts are scripts
    TexEntryKind_Function,          // an operator name, an upright <mi> of class Op whose scripts are scripts
    TexEntryKind_LimitFunction,     // an operator name whose scripts are limits in display style
    TexEntryKind_Accent,            // `arg` holds TexAccentFlags
    TexEntryKind_TextAccent,        // a text mode accent whose combining mark is `text`
    TexEntryKind_TextSymbol,        // text mode only
    TexEntryKind_Space,             // `arg` is the width in mu, signed
    TexEntryKind_Font,              // a command whose argument is in the font `arg`
    TexEntryKind_FontSwitch,        // a declaration that sets the font `arg` to the end of the group
    TexEntryKind_TextFont,          // a command whose argument is text in the TexTextFont `arg`
    TexEntryKind_Style,             // a declaration that sets the TexStyle `arg`
    TexEntryKind_Size,              // a declaration that sets the font size to `arg` hundredths of an em
    TexEntryKind_BigDelimiter,      // \big and its kin, with the size, 1 to 4, and the TexClass shifted by 4 in `arg`
    TexEntryKind_ExtensibleArrow,   // \xrightarrow and its kin, with the arrow in `text`
    TexEntryKind_Macro,             // `text` is the replacement, and `arg` the number of parameters
    TexEntryKind_Command,           // `arg` is a TexCommand
};
typedef uint8_t TexEntryKind;

// How an accent sits on its base.
enum {
    TexAccentFlag_Stretchy = 1u << 0,   // the accent stretches to the base's width
    TexAccentFlag_Under = 1u << 1,      // the accent is below the base
    TexAccentFlag_Limits = 1u << 2,     // the result is an Op whose scripts are always limits, as for \overbrace
    TexAccentFlag_Clear = 1u << 3,      // a rule, brace or arrow, not an accent, set clear of the base at the base's
                                        // size, as \overline and \overbrace are (TeXbook Appendix G, rules 9 and 10)
};
typedef uint16_t TexAccentFlags;

// The fonts of text mode, which <mtext> sets with CSS.
enum {
    TexTextFont_Upright,
    TexTextFont_Bold,
    TexTextFont_Italic,
    TexTextFont_SansSerif,
    TexTextFont_Monospace,
};
typedef uint8_t TexTextFont;

// The commands that the parser implements, each a construct of its own.
enum {
    TexCommand_Frac,
    TexCommand_DisplayFrac,
    TexCommand_TextFrac,
    TexCommand_ContinuedFrac,
    TexCommand_Binom,
    TexCommand_DisplayBinom,
    TexCommand_TextBinom,
    TexCommand_GenFrac,
    TexCommand_Over,
    TexCommand_Atop,
    TexCommand_Choose,
    TexCommand_Brack,
    TexCommand_Brace,
    TexCommand_Above,
    TexCommand_Sqrt,
    TexCommand_Left,
    TexCommand_Right,
    TexCommand_Middle,
    TexCommand_Limits,
    TexCommand_NoLimits,
    TexCommand_DisplayLimits,
    TexCommand_OverSet,
    TexCommand_UnderSet,
    TexCommand_StackRel,
    TexCommand_OperatorName,
    TexCommand_OperatorNameWithLimits,
    TexCommand_MathOrd,
    TexCommand_MathOp,
    TexCommand_MathBin,
    TexCommand_MathRel,
    TexCommand_MathOpen,
    TexCommand_MathClose,
    TexCommand_MathPunct,
    TexCommand_MathInner,
    TexCommand_HSpace,
    TexCommand_Kern,
    TexCommand_MKern,
    TexCommand_Color,
    TexCommand_TextColor,
    TexCommand_ColorBox,
    TexCommand_FColorBox,
    TexCommand_Boxed,
    TexCommand_FBox,
    TexCommand_Phantom,
    TexCommand_HPhantom,
    TexCommand_VPhantom,
    TexCommand_Smash,
    TexCommand_RLap,
    TexCommand_LLap,
    TexCommand_CLap,
    TexCommand_Cancel,
    TexCommand_BCancel,
    TexCommand_XCancel,
    TexCommand_SOut,
    TexCommand_Enclose,
    TexCommand_Not,
    TexCommand_Begin,
    TexCommand_End,
    TexCommand_Newline,
    TexCommand_HLine,
    TexCommand_HDashLine,
    TexCommand_Tag,
    TexCommand_NoTag,
    TexCommand_Label,
    TexCommand_NewCommand,
    TexCommand_RenewCommand,
    TexCommand_ProvideCommand,
    TexCommand_Def,
    TexCommand_GDef,
    TexCommand_Global,
    TexCommand_Let,
    TexCommand_DeclareMathOperator,
    TexCommand_Href,
    TexCommand_Url,
    TexCommand_Unicode,
    TexCommand_Char,
    TexCommand_Class,
    TexCommand_CssId,
    TexCommand_Style,
    TexCommand_Mod,
    TexCommand_BMod,
    TexCommand_PMod,
    TexCommand_Pod,
    TexCommand_Dots,
    TexCommand_SubStack,
    TexCommand_MathChoice,
    TexCommand_Relax,
    TexCommand_Strut,
    TexCommand_MathStrut,
    TexCommand_RaiseBox,
    TexCommand_Rule,
    TexCommand_SideSet,
    TexCommand_Ensuremath,
    TexCommand_Pmb,
    TexCommand_Ref,
    TexCommand_TextMode,            // \text, \mbox and \hbox, which set text in the surrounding text font
    TexCommand_Count,
};
typedef uint16_t TexCommand;

// A control sequence that the language defines.
struct TexEntry {
    const char* name;               // without its backslash
    const char* PR_NULLABLE text;   // UTF-8
    TexEntryKind kind;
    TexClass texClass;
    int16_t arg;
};

// Returns the entry of the control sequence `name`, written without its backslash, or null if the language does not
// define it.
const TexEntry* PR_NULLABLE texEntryFromName(PrStringView name);

// Returns the entry whose text is the one code point `codePoint`, for a symbol written as its character, or null if
// there is none.
const TexEntry* PR_NULLABLE texEntryFromCodePoint(uint32_t codePoint);

// Returns `codePoint` in the alphabet of `font`, or `codePoint` itself if the alphabet has no such character. Latin
// letters, Greek letters and digits have alphabets, including the letters that Unicode encodes outside the Mathematical
// Alphanumeric Symbols block, such as U+211D ℝ.
uint32_t texFontMap(uint32_t codePoint, TexFont font);

// Returns the variation selector that chooses the style of `font` for `codePoint`, a character of the font's alphabet,
// by Unicode's standardized variation sequences. It is U+FE01 for roundhand script capitals. Returns 0 if the character
// needs none, as the chancery capitals of \mathcal do. A math font's script capitals are chancery by default, and a
// selector that the math font does not list would move the character to a fallback face
// (css-fonts-4/#cluster-matching).
uint32_t texFontVariation(uint32_t codePoint, TexFont font);

// Returns true if MathML's math-auto transform sets the one-character <mi> `codePoint` in italic.
bool texIsAutoItalic(uint32_t codePoint);

// Returns the relation `codePoint` crossed out, as \not makes it, or 0 if Unicode has no precomposed character for it.
uint32_t texNegate(uint32_t codePoint);

// ---------------------------------------------------------------------------------------------------------------------
// The operator dictionary

// Returns the values that the operator dictionary gives the operator `content` in `form`, by MathML Core's algorithm
// (mathml-core/#dictionary-based-attributes). An operator that the form does not list takes the first of its infix,
// postfix and prefix entries, unless `explicitForm`. Spaces are in mu, which are the dictionary's eighteenths of an em.
TexCategoryValues texOperatorDictionary(PrStringView content, TexForm form, bool explicitForm);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
