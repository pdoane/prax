// TeX math parsed into a tree of MathML elements that carries the TeX classes and styles which decide its spacing.

#pragma once

#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/tex.h"
#include "pr/text_buffer.h"

#include "tex/src/tex_symbols.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Trees

// A position in a tree's nodes.
typedef uint32_t TexNodeIndex;

// The absent node.
constexpr TexNodeIndex kTexNodeNone = 0xffffffffu;

// The MathML elements that a formula is written with.
enum {
    TexElement_Math,
    TexElement_Semantics,
    TexElement_Annotation,
    TexElement_Mrow,
    TexElement_Mi,
    TexElement_Mn,
    TexElement_Mo,
    TexElement_Mtext,
    TexElement_Mspace,
    TexElement_Msub,
    TexElement_Msup,
    TexElement_Msubsup,
    TexElement_Munder,
    TexElement_Mover,
    TexElement_Munderover,
    TexElement_Mmultiscripts,
    TexElement_Mprescripts,
    TexElement_Mfrac,
    TexElement_Msqrt,
    TexElement_Mroot,
    TexElement_Mstyle,
    TexElement_Merror,
    TexElement_Mpadded,
    TexElement_Mphantom,
    TexElement_Menclose,
    TexElement_Mtable,
    TexElement_Mtr,
    TexElement_Mtd,
    TexElement_Mlabeledtr,
};
typedef uint8_t TexElement;

// An operator property that TeX requires, or leaves to the operator dictionary.
enum {
    TexTri_Any,
    TexTri_False,
    TexTri_True,
};
typedef uint8_t TexTri;

// What a node is beyond its element and class. The scripts of an Op atom without a limits flag attach as scripts.
enum {
    TexNodeFlag_Function = 1u << 0,         // an operator name that U+2061 follows, as amsmath's \sin is
    TexNodeFlag_ApplyFunction = 1u << 1,    // the U+2061 after an operator name, a part of the name's atom
    TexNodeFlag_Transparent = 1u << 2,      // a declaration's <mstyle>, whose children are atoms of the list it is in
    TexNodeFlag_LeadingOrd = 1u << 3,       // a list after an empty Ord, as in amsmath's even alignment columns, so
                                            // that {}=x spaces the relation
    TexNodeFlag_Formed = 1u << 4,           // an operator whose form is decided
    TexNodeFlag_Spaced = 1u << 5,           // an operator whose lspace and rspace are laid out, in a row
    TexNodeFlag_ModSpacing = 1u << 6,       // \bmod's operator, with a thick space on each side in every style
    TexNodeFlag_DisplayLimits = 1u << 7,    // an Op whose scripts are limits in display style, as for \sum
    TexNodeFlag_Limits = 1u << 8,           // an Op whose scripts are limits in every style, as for \overbrace
    TexNodeFlag_UnderOver = 1u << 9,        // an operator in <munder>, <mover> or <munderover>, which stretch it inline
};
typedef uint16_t TexNodeFlags;

// A MathML element, or the characters of a token element.
struct TexNode {
    TexNodeIndex firstChild;
    TexNodeIndex lastChild;
    TexNodeIndex next;
    uint32_t textStart;             // the token's characters, a range of the tree's text
    uint32_t textSize;
    uint32_t firstAttribute;        // a position in the tree's attributes, or kTexNodeNone
    uint32_t lastAttribute;
    TexNodeFlags flags;
    TexElement element;
    TexClass texClass;              // the atom's class in the list it is in, or None for glue
    TexStyle style;                 // the style that the node is set in

    // An operator's form, and the properties that TeX requires of it. Spaces are in mu.
    TexForm form;
    TexTri stretchy;
    TexTri symmetric;
    TexTri largeop;
    TexTri movablelimits;
    uint8_t lspace;
    uint8_t rspace;
};

// An attribute of a node, whose name is a literal and whose value is a range of the tree's text.
struct TexAttributeRecord {
    PrStringView name;
    uint32_t valueStart;
    uint32_t valueSize;
    uint32_t next;
};

// A formula's MathML.
struct TexTree {
    RawArray<TexNode> nodes;
    RawArray<TexAttributeRecord> attributes;
    PrTextBuffer text;
    TexNodeIndex root;
};

// Creates a node with no children, text or attributes.
TexNodeIndex texNodeCreate(TexTree* tree, TexElement element, TexClass texClass, TexStyle style);

// Appends `child`, which has no parent, to `parent`'s children.
void texNodeAppend(TexTree* tree, TexNodeIndex parent, TexNodeIndex child);

// Inserts `child`, which has no parent, among `parent`'s children before `before`, or last if `before` is kTexNodeNone.
void texNodeInsertBefore(TexTree* tree, TexNodeIndex parent, TexNodeIndex child, TexNodeIndex before);

// Moves every child of `from` to the end of `to`'s children.
void texNodeMoveChildren(TexTree* tree, TexNodeIndex from, TexNodeIndex to);

// Returns the number of children.
uint32_t texNodeChildCount(const TexTree* tree, TexNodeIndex node);

// Sets or appends to the node's characters.
void texNodeSetText(TexTree* tree, TexNodeIndex node, PrStringView text);
void texNodeAppendText(TexTree* tree, TexNodeIndex node, PrStringView text);

// The node's characters, valid until the tree's text grows.
PrStringView texNodeText(const TexTree* tree, TexNodeIndex node);

// Sets the attribute `name`, a literal, replacing its value if the node has it already.
void texNodeSetAttribute(TexTree* tree, TexNodeIndex node, PrStringView name, PrStringView value);

// Appends `from`'s attributes to `to`, which has none of the same names.
void texNodeCopyAttributes(TexTree* tree, TexNodeIndex from, TexNodeIndex to);

// Returns true if the node has the attribute `name`.
bool texNodeHasAttribute(const TexTree* tree, TexNodeIndex node, PrStringView name);

// The value of an attribute record, valid until the tree's text grows.
PrStringView texAttributeValue(const TexTree* tree, const TexAttributeRecord& attribute);

// Frees the tree's storage.
void texTreeReset(TexTree* tree);

// ---------------------------------------------------------------------------------------------------------------------
// Embellished operators (mathml-core/#embellished-operators)

// Returns true if the node is an embellished operator, or returns the core operator of one.
bool texNodeIsEmbellished(const TexTree* tree, TexNodeIndex node);
TexNodeIndex texNodeCore(const TexTree* tree, TexNodeIndex node);

// Returns true if the node is space-like.
bool texNodeIsSpaceLike(const TexTree* tree, TexNodeIndex node);

// Returns true if the element lays out its children as a row: a grouping element, <mpadded>, <msqrt>, or the anonymous
// <mrow> of a table cell.
bool texElementIsRow(TexElement element);

// ---------------------------------------------------------------------------------------------------------------------
// Documents

// A definition of a control sequence, as \newcommand or \def makes it.
struct TexMacro {
    PrRawString name;
    PrRawString replacement;
    PrRawString defaultArgument;    // the first argument's default, if `hasDefault`

    // The parameter text of a \def whose parameters are delimited, as \def\a#1.#2\end{...} writes it: the tokens that
    // must follow the name and each parameter. Empty for a macro whose parameters are each one argument.
    PrRawString parameterText;

    uint8_t parameterCount;
    bool hasDefault;
};

// Macro definitions, the later of two with one name hiding the earlier.
struct TexMacroTable {
    RawArray<TexMacro> macros;
};

// A label of a numbered equation, and the tag that \ref writes for it.
struct TexLabel {
    PrRawString name;
    PrRawString tag;
};

// The state that the formulas of a document share (PrTexDocument).
struct TexDocument {
    TexMacroTable macros;
    RawArray<TexLabel> labels;      // the later of two with one name replaces the earlier
    uint32_t equationCount;         // the number of the last numbered equation
};

// Defines `name`, replacing any earlier definition. A null `parameterText` makes a macro whose parameters are each one
// argument.
void texMacroTableDefine(TexMacroTable* table, PrStringView name, uint32_t parameterCount, PrStringView replacement,
    const PrStringView* PR_NULLABLE defaultArgument, PrStringView parameterText);

// Returns the definition of `name`, or null. The definition is valid until the table changes.
const TexMacro* PR_NULLABLE texMacroTableFind(const TexMacroTable* table, PrStringView name);

// Removes every definition, keeping the storage, or frees the storage.
void texMacroTableClear(TexMacroTable* table);
void texMacroTableReset(TexMacroTable* table);

// Sets the tag of the label `name`.
void texDocumentSetLabel(TexDocument* document, PrStringView name, PrStringView tag);

// Returns the tag of the label `name`, or the null view. The tag is valid until the document changes.
PrStringView texDocumentLabel(const TexDocument* document, PrStringView name);

// Removes every definition and label and sets the counter to 0, keeping the storage, or frees the storage.
void texDocumentClear(TexDocument* document);
void texDocumentReset(TexDocument* document);

// ---------------------------------------------------------------------------------------------------------------------
// Parsing

// Parses `source` into `tree`, which must be empty, as the <math> element at the tree's root. `document` holds the
// shared definitions, counter and labels, and receives the formula's global definitions, numbers and labels. A null
// `document` makes the formula a document of its own.
void texParse(TexTree* tree, PrStringView source, PrTexDisplay display, bool annotate, TexDocument* PR_NULLABLE document);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
