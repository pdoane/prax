// TeX math: formulas in the TeX language of LaTeX, amsmath, MathJax, KaTeX and Temml, written as MathML Core.

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Documents

// The state that the formulas of one document share, as in a LaTeX document. It holds macro definitions, the equation
// counter and the labels of numbered equations. A formula sees the document's definitions. Its \gdef and \global\def
// add to them, and its \newcommand, \renewcommand and \def last only to its end. Each numbered equation takes the next
// number of the document's counter, and each \label names that number for the \ref and \eqref of every later formula. A
// formula without a document is a document of its own, whose equations are numbered from 1.
//
// A document is not thread-safe. A formula that reads it must not run concurrently with one that changes it.
typedef struct PrTexDocument PrTexDocument;

// Creates an empty document.
PrTexDocument* prTexDocumentCreate(void);

// Frees the document.
void prTexDocumentDestroy(PrTexDocument* PR_NULLABLE document);

// Removes every definition and label and sets the equation counter to 0, keeping the storage.
void prTexDocumentClear(PrTexDocument* document);

// Defines the control sequence `name`, written without its backslash, to expand to `replacement`, in which #1 to #9
// stand for its `parameterCount` arguments. `parameterCount` must be at most 9. A definition replaces an earlier one of
// the same name, and hides a built-in command of that name.
void prTexDocumentDefineMacro(PrTexDocument* document, PrStringView name, uint32_t parameterCount,
    PrStringView replacement);

// Returns the number of macro definitions.
uint32_t prTexDocumentMacroCount(const PrTexDocument* document);

// Returns the number that the document's last numbered equation took, or 0 if none has.
uint32_t prTexDocumentEquationCount(const PrTexDocument* document);

// Returns the tag of the equation that `label` names, as \ref writes it, or the null view if no formula of the document
// defined the label. The tag is valid until the document changes.
PrStringView prTexDocumentLabel(const PrTexDocument* document, PrStringView label);

// ---------------------------------------------------------------------------------------------------------------------
// Options

// How a formula sits in its text.
enum {
    PrTexDisplay_Inline,            // in a line of text, in TeX's text style: $…$
    PrTexDisplay_Block,             // a block of its own, in TeX's display style: $$…$$
};
typedef uint8_t PrTexDisplay;

// How a formula is written.
typedef struct PrTexOptions {
    PrTexDisplay display;

    // True to wrap the MathML in <semantics> with an <annotation encoding="application/x-tex"> that holds the source,
    // so that copying the formula copies its TeX.
    bool annotate;

    // The document whose definitions, counter and labels the formula reads and adds to, or null for a formula that is a
    // document of its own.
    PrTexDocument* PR_NULLABLE document;
} PrTexOptions;

// An inline formula, annotated with its source, that is a document of its own.
PrTexOptions prTexOptionsDefault(void);

// ---------------------------------------------------------------------------------------------------------------------
// MathML

// The kinds of MathML event.
enum {
    PrTexEventKind_StartTag,
    PrTexEventKind_EndTag,
    PrTexEventKind_Text,            // characters
};
typedef uint8_t PrTexEventKind;

// An attribute of a start tag.
typedef struct PrTexAttribute {
    PrStringView name;
    PrStringView value;
} PrTexAttribute;

// One event of a formula's MathML. Text and values are not escaped. Every element has an end tag, including those that
// are empty, such as <mspace>.
typedef struct PrTexEvent {
    PrTexEventKind kind;
    PrStringView name;              // StartTag, EndTag: the element's local name, in the MathML namespace
    PrStringView text;              // Text

    // StartTag: the attributes in the order that they are written.
    const PrTexAttribute* PR_NULLABLE attributes;
    uint32_t attributeCount;
} PrTexEvent;

// Called with each event of a formula's MathML, in order. The event and its views are valid only during the call.
typedef void (*PrTexEventFn)(void* PR_NULLABLE context, const PrTexEvent* event);

// Calls `fn` with the events of the MathML of the TeX formula `source`, which is one <math> element. A null `options`
// means the defaults.
//
// The MathML carries TeX's typesetting, including the spacing of its atoms, its styles, limits, delimiter sizes and
// equation numbers. The spacing is written as explicit lspace and rspace where MathML's operator dictionary would
// differ. Source that does not parse never fails the call. Each malformed construct, and each undefined control
// sequence, becomes an <merror> that holds its source, and the rest of the formula is written around it.
void prTexWriteMathMl(PrStringView source, const PrTexOptions* PR_NULLABLE options, PrTexEventFn fn, void* PR_NULLABLE context);

// Appends the MathML markup of the TeX formula `source` to `out`, with its text and values escaped. A null `options`
// means the defaults.
void prTexAppendMathMl(PrStringView source, const PrTexOptions* PR_NULLABLE options, PrRawString* out);

// Appends the markup of `event` to `out`, with its text and values escaped, as prTexAppendMathMl writes each event.
void prTexEventAppendText(const PrTexEvent* event, PrRawString* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
