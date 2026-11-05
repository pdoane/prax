// Draw structures: what the content of draw lists means, as a tree in reading order whose leaves are the producer's
// keys that the lists' marks carry (prDrawListPushMark).
//
// A structure knows nothing of where its content came from. A writer joins it to the marks of the pages that it writes,
// and a platform's accessibility tree can join it to the bounds of the same keys.

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A tree of nodes, each with a type and children in reading order. A child is a node, or a key that stands for every
// mark of that key, in the order that the marks were drawn. The structure has one owner.
typedef struct PrDrawStructure PrDrawStructure;

// A node of a structure, by its index. The root is node zero.
typedef uint32_t PrDrawStructureNode;
enum { kPrDrawStructureRoot = 0 };

// The absent node. An item whose node is absent is a key.
PR_CONSTANT PrDrawStructureNode kPrDrawStructureNodeNone = 0xffffffffu;

// The types of node: the standard structure types of PDF 2.0 (ISO 32000-2, 14.8.4), which name what a reader of any
// format recognizes, then those of PDF 1.7 (ISO 32000-1, 14.8.4) that PDF 2.0 dropped and that documents still use.
enum {
    // Grouping
    PrDrawStructureType_Document,
    PrDrawStructureType_DocumentFragment,
    PrDrawStructureType_Part,
    PrDrawStructureType_Sect,
    PrDrawStructureType_Div,
    PrDrawStructureType_Aside,
    PrDrawStructureType_NonStruct,

    // Blocks
    PrDrawStructureType_P,
    PrDrawStructureType_H1,
    PrDrawStructureType_H2,
    PrDrawStructureType_H3,
    PrDrawStructureType_H4,
    PrDrawStructureType_H5,
    PrDrawStructureType_H6,
    PrDrawStructureType_Title,
    PrDrawStructureType_FENote,
    PrDrawStructureType_Sub,

    // Inline content
    PrDrawStructureType_Lbl,
    PrDrawStructureType_Span,
    PrDrawStructureType_Em,
    PrDrawStructureType_Strong,
    PrDrawStructureType_Link,
    PrDrawStructureType_Annot,
    PrDrawStructureType_Form,
    PrDrawStructureType_Ruby,
    PrDrawStructureType_RB,
    PrDrawStructureType_RT,
    PrDrawStructureType_RP,

    // Lists
    PrDrawStructureType_L,
    PrDrawStructureType_LI,
    PrDrawStructureType_LBody,

    // Tables
    PrDrawStructureType_Table,
    PrDrawStructureType_TR,
    PrDrawStructureType_TH,
    PrDrawStructureType_TD,
    PrDrawStructureType_THead,
    PrDrawStructureType_TBody,
    PrDrawStructureType_TFoot,
    PrDrawStructureType_Caption,

    // Illustrations
    PrDrawStructureType_Figure,
    PrDrawStructureType_Formula,
    PrDrawStructureType_Artifact,

    // PDF 1.7's types
    PrDrawStructureType_BlockQuote,
    PrDrawStructureType_Quote,
    PrDrawStructureType_Code,
    PrDrawStructureType_Note,
    PrDrawStructureType_Reference,
    PrDrawStructureType_BibEntry,
    PrDrawStructureType_Index,
    PrDrawStructureType_TOC,
    PrDrawStructureType_TOCI,

    PrDrawStructureType_Count,
};
typedef uint8_t PrDrawStructureType;

// Returns the type's name as PDF spells it, such as "H1" or "LBody".
PrStringView prDrawStructureTypeName(PrDrawStructureType type);

// Returns true if the type is one of PDF 1.7's that PDF 2.0 dropped.
bool prDrawStructureTypeIsLegacy(PrDrawStructureType type);

// Returns the level of a heading type, from 1 through 6, or zero for any other type.
uint32_t prDrawStructureTypeHeadingLevel(PrDrawStructureType type);

// The cells that a header cell heads (ISO 32000-2, 14.8.5.7).
enum {
    PrDrawStructureScope_None,
    PrDrawStructureScope_Row,
    PrDrawStructureScope_Column,
    PrDrawStructureScope_Both,
};
typedef uint8_t PrDrawStructureScope;

// A node: its type, and what a reader needs beyond its content. A string that is empty is absent. The structure copies
// the strings.
typedef struct PrDrawStructureNodeDesc {
    PrDrawStructureType type;

    PrStringView alt;           // a description of the content, which replaces it for a reader, as of a figure
    PrStringView actualText;    // text that the content shows, which replaces the text that it draws
    PrStringView expansion;     // the expansion of an abbreviation
    PrStringView language;      // the node's language as a BCP 47 tag, if it differs from the parent's
    PrStringView title;         // a title of the node, which an outline shows for a heading

    // For a Link: the URI that the link leads to, or else the key whose first mark it leads to, or zero for none.
    PrStringView uri;
    uint64_t destination;

    // For a TH, the cells that it heads. For a TH or a TD, the rows and columns that the cell spans, or zero for one.
    PrDrawStructureScope scope;
    uint32_t rowSpan;
    uint32_t columnSpan;
} PrDrawStructureNodeDesc;

// Returns a node of `type` with nothing else.
PrDrawStructureNodeDesc prDrawStructureNodeDescDefault(PrDrawStructureType type);

// A child of a node: a node, or a key if `node` is kPrDrawStructureNodeNone.
typedef struct PrDrawStructureItem {
    PrDrawStructureNode node;
    uint64_t key;
} PrDrawStructureItem;

// Creates a structure whose root is described by `root`, or is a Document if `root` is null.
PrDrawStructure* prDrawStructureCreate(const PrDrawStructureNodeDesc* PR_NULLABLE root);

// Frees the structure.
void prDrawStructureDestroy(PrDrawStructure* PR_NULLABLE structure);

// Appends a node described by `desc` to the children of `parent`, and returns it.
PrDrawStructureNode prDrawStructureAppendNode(PrDrawStructure* structure, PrDrawStructureNode parent,
    const PrDrawStructureNodeDesc* desc);

// Appends `key`, which is not zero, to the children of `parent`. The key stands for its marks.
void prDrawStructureAppendKey(PrDrawStructure* structure, PrDrawStructureNode parent, uint64_t key);

// Returns the number of nodes, the root included.
uint32_t prDrawStructureNodeCount(const PrDrawStructure* structure);

// Returns the node's description, or replaces it. The strings that it returns stay valid while the structure lives.
PrDrawStructureNodeDesc prDrawStructureNodeDesc(const PrDrawStructure* structure, PrDrawStructureNode node);
void prDrawStructureNodeSetDesc(PrDrawStructure* structure, PrDrawStructureNode node, const PrDrawStructureNodeDesc* desc);

// Returns the node's parent, or kPrDrawStructureNodeNone for the root.
PrDrawStructureNode prDrawStructureNodeParent(const PrDrawStructure* structure, PrDrawStructureNode node);

// Returns the node's children in reading order. They stay valid until the structure changes.
uint32_t prDrawStructureItemCount(const PrDrawStructure* structure, PrDrawStructureNode node);
const PrDrawStructureItem* PR_NULLABLE prDrawStructureItems(const PrDrawStructure* structure, PrDrawStructureNode node);

// Returns the structure as text, one node to a line, indented by depth, for tests and diagnostics.
PrRawString prDrawStructureDump(const PrDrawStructure* structure);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
