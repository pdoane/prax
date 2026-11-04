// MathML's presentational hints: the attributes of MathML elements that map to CSS properties
//
// mathml-core - https://w3c.github.io/mathml-core/
// mathml4 - https://w3c.github.io/mathml/

#include "mathml/src/mathml_hints.h"

#include "pr/ascii.h"
#include "pr/count_of.h"
#include "pr/string_trim.h"
#include "pr/text_buffer.h"

#include "css/src/css_serialize.h"
#include "dom/src/dom_node.h"
#include "mathml/src/mathml_attribute.h"
#include "mathml/src/mathml_element.h"
#include "mathml/src/mathml_operator.h"

namespace pr_impl {

namespace {

// The CSS spelling of each MathMlUnit that CSS has, by MathMlUnit up to the percentage.
constexpr PrStringView kUnits[] = {
    "px", "em", "ex", "ch", "ic", "cap", "rem", "lh", "rlh", "vw", "vh", "vmin", "vmax", "%",
};

// Appends `length` as a CSS <length-percentage>, a single term as it is and a sum of terms as a calc(). Returns false
// for a length that is absent, an increment or a decrement, or has a pseudo-unit, and the caller then discards what was
// appended.
bool appendLength(PrTextBuffer* out, const MathMlLength& length) {
    if (length.count == 0 || length.flags != 0 || mathMlLengthHasPseudoUnit(length))
        return false;
    if (length.count > 1)
        prTextBufferAppend(out, "calc(");
    for (uint32_t i = 0; i < length.count; ++i) {
        if (i != 0)
            prTextBufferAppend(out, " + ");
        cssSerializeNumber(out, double(length.values[i]));
        prTextBufferAppend(out, kUnits[length.units[i]]);
    }
    if (length.count > 1)
        prTextBufferAppendChar(out, ')');
    return true;
}

// Appends a declaration of `property` with `length` if the length is a <length-percentage>. A length with a percentage
// is appended only if `percent` is true.
void addLength(PrTextBuffer* out, PrStringView property, const MathMlLength& length, bool percent) {
    if (!percent && mathMlLengthHasPercent(length))
        return;
    uint32_t mark = out->size;
    prTextBufferAppend(out, property);
    prTextBufferAppendChar(out, ':');
    if (appendLength(out, length))
        prTextBufferAppendChar(out, ';');
    else
        out->size = mark;
}

// Returns true if `text` holds one declaration's value and nothing that would end it, mark it important or start a
// block, so that the value declares one property or nothing.
bool isOneValue(PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (c == ';' || c == '!' || c == '{' || c == '}' || c == '\\' || c == '/')
            return false;
    }
    return true;
}

// Returns the row of the table cell `cell`, an mtr or mlabeledtr, or null.
const PrDomElement* PR_NULLABLE rowOf(const PrDomElement* cell) {
    const PrDomElement* row = domNodeParentElement(cell);
    return mathMlIs(row, PrDomTag_Mtr) || mathMlIs(row, PrDomTag_Mlabeledtr) ? row : nullptr;
}

// Returns the table of the row `row`, or null.
const PrDomElement* PR_NULLABLE tableOf(const PrDomElement* PR_NULLABLE row) {
    const PrDomElement* table = row != nullptr ? domNodeParentElement(row) : nullptr;
    return mathMlIs(table, PrDomTag_Mtable) ? table : nullptr;
}

// Returns true if the node is a row of a table, an mtr or an mlabeledtr.
bool isRow(const PrDomNode* node) {
    const PrDomElement* element = domElementFromNode(node);
    return mathMlIs(element, PrDomTag_Mtr) || mathMlIs(element, PrDomTag_Mlabeledtr);
}

// Where a row is among the rows of its table: its position, counted no further than `limit`, and whether it is the
// first or the last.
struct RowPlace {
    uint32_t index;
    bool first;
    bool last;
};

// Returns the place of the row `row` among the rows of its table, its position counted up to `limit`. The cost is
// bounded by `limit`, not by the number of rows, so that building the hints of every cell of a long table is linear.
RowPlace rowPlaceOf(const PrDomElement* row, uint32_t limit) {
    RowPlace place{.first = true, .last = true};
    for (const PrDomNode* node = static_cast<const PrDomNode*>(row)->previous; node != nullptr; node = node->previous) {
        if (!isRow(node))
            continue;
        place.first = false;
        if (place.index == limit)
            break;
        ++place.index;
    }
    for (const PrDomNode* node = static_cast<const PrDomNode*>(row)->next; node != nullptr && place.last;
            node = node->next)
        place.last = !isRow(node);
    return place;
}

// Returns the first column of the cell `cell` in its row, counting the columns that the cells before it span, and
// writes the columns that it spans to `span`. An mlabeledtr's first cell is its label, which takes no column.
uint32_t columnOf(const PrDomElement* cell, uint32_t* span) {
    const PrDomElement* row = domNodeParentElement(cell);
    bool labeled = mathMlIs(row, PrDomTag_Mlabeledtr);
    uint32_t column = 0;
    bool first = true;
    uint32_t rows = 0;
    for (const PrDomNode* node = row->first; node != nullptr && node != cell; node = node->next) {
        const PrDomElement* element = domElementFromNode(node);
        if (!mathMlIs(element, PrDomTag_Mtd))
            continue;
        uint32_t columns = 0;
        mathMlCellSpans(element, &columns, &rows);
        if (!(labeled && first))
            column += columns;
        first = false;
    }
    mathMlCellSpans(cell, span, &rows);
    return column;
}

// Appends half of a table's spacing, `spacing`, as the declaration of `property`.
void addHalfSpacing(PrTextBuffer* out, PrStringView property, MathMlLength spacing) {
    for (uint32_t i = 0; i < spacing.count; ++i)
        spacing.values[i] /= 2;
    addLength(out, property, spacing, true);
}

// Appends a border of `line` as the declaration of `property`, a border shorthand, if the line is drawn.
void addLine(PrTextBuffer* out, PrStringView property, MathMlLine line) {
    if (line == MathMlLine_None)
        return;
    // TeX rules a table with \arrayrulewidth, 0.4pt, which is 0.04em of its 10pt type.
    if (line == MathMlLine_Solid)
        cssHintAdd(out, property, "0.04em solid");
    else
        cssHintAdd(out, property, "0.04em dashed");
}

// Returns true if the table has the attribute `name`, or an mstyle or math element around it gives its default.
bool givesTable(const PrDomElement* table, PrStringView name) {
    return mathMlInheritedValue(table, name).data != nullptr;
}

// Returns true if the table has one of MathML 4's spacing, line or frame attributes (mathml4/#presm_mtable). Such a
// table takes MathML 4's model of spacing, with space between its rows and columns and none outside them but its frame
// spacing.
bool spacesAsMathMl4(const PrDomElement* table) {
    constexpr PrStringView kSpacing[] = {"rowspacing", "columnspacing", "rowlines", "columnlines", "frame", "framespacing"};
    for (PrStringView name : kSpacing) {
        if (givesTable(table, name))
            return true;
    }
    return false;
}

// Returns true if the cell is the label of its row, which is the first mtd of an mlabeledtr.
bool isLabel(const PrDomElement* cell) {
    for (const PrDomNode* node = cell->parent->first; node != nullptr; node = node->next) {
        const PrDomElement* element = domElementFromNode(node);
        if (mathMlIs(element, PrDomTag_Mtd))
            return element == cell;
    }
    return false;
}

// Returns the mtd that follows the cell in its row, past text and comments, or null.
const PrDomElement* PR_NULLABLE nextCell(const PrDomElement* cell) {
    for (const PrDomNode* node = cell->next; node != nullptr; node = node->next) {
        if (mathMlIs(domElementFromNode(node), PrDomTag_Mtd))
            return domElementFromNode(node);
    }
    return nullptr;
}

// Returns true if the table has a labeled row, whose label it holds.
bool hasLabels(const PrDomElement* table) {
    for (const PrDomNode* node = table->first; node != nullptr; node = node->next) {
        if (mathMlIs(domElementFromNode(node), PrDomTag_Mlabeledtr))
            return true;
    }
    return false;
}

// Returns the number of columns of the table, which is the most that the cells of any of its rows span.
uint32_t columnCountOf(const PrDomElement* table) {
    uint32_t most = 0;
    for (const PrDomNode* node = table->first; node != nullptr; node = node->next) {
        const PrDomElement* row = domElementFromNode(node);
        if (!mathMlIs(row, PrDomTag_Mtr) && !mathMlIs(row, PrDomTag_Mlabeledtr))
            continue;
        const PrDomElement* last = nullptr;
        for (const PrDomNode* child = row->first; child != nullptr; child = child->next) {
            if (mathMlIs(domElementFromNode(child), PrDomTag_Mtd))
                last = domElementFromNode(child);
        }
        if (last == nullptr)
            continue;
        uint32_t span = 1;
        uint32_t count = columnOf(last, &span) + span;
        if (mathMlIs(row, PrDomTag_Mlabeledtr) && isLabel(last))
            count = 0;
        most = count > most ? count : most;
    }
    return most;
}

// Appends the width that the table's columnwidth gives the column of a cell that spans only that column
// (mathml4/#presm_mtable). The width is a length, or for fit, an equal share of the table's width that the other
// columns leave. Each fit column asks for its share of the whole width, and CSS table layout reduces it to what the
// other columns leave.
void addColumnWidth(PrTextBuffer* out, const PrDomElement* table, uint32_t column) {
    if (!givesTable(table, "columnwidth"))
        return;
    MathMlLength length{};
    MathMlColumnWidth width = mathMlTableColumnWidth(table, column, &length);
    if (width == MathMlColumnWidth_Length) {
        addLength(out, "width", length, true);
        return;
    }
    if (width != MathMlColumnWidth_Fit)
        return;
    uint32_t fits = 0;
    uint32_t columns = columnCountOf(table);
    for (uint32_t c = 0; c < columns; ++c) {
        if (mathMlTableColumnWidth(table, c, &length) == MathMlColumnWidth_Fit)
            ++fits;
    }
    if (fits == 0)
        return;
    prTextBufferAppend(out, "width:");
    cssSerializeNumber(out, 100.0 / double(fits));
    prTextBufferAppend(out, "%;");
}

// Appends the hints that the attributes of the table and the row of the cell `cell` give it (mathml4/#presm_mtable).
// The hints are its alignment in its column and row, its column's width, half of the spacing between it and the
// adjacent cells as its padding, and the lines between it and those cells as its borders. Each applies only if an
// attribute gives it, so that the user agent's sheet decides the rest.
void addCell(PrTextBuffer* out, const PrDomElement* cell) {
    const PrDomElement* row = rowOf(cell);
    const PrDomElement* table = tableOf(row);
    RowPlace place{.first = true, .last = true};
    if (row != nullptr && table != nullptr)
        place = rowPlaceOf(row, mathMlTableRowItems(table));
    uint32_t rowIndex = place.index;
    uint32_t span = 1;
    uint32_t column = row != nullptr ? columnOf(cell, &span) : 0;
    // The label of an mlabeledtr goes outside the table's columns, on the side that the table's side attribute names,
    // which is the right by default (mathml4/#presm_mtr). The table is its containing block.
    if (mathMlIs(row, PrDomTag_Mlabeledtr) && isLabel(cell) && table != nullptr) {
        PrStringView side = mathMlTable(table).side;
        bool left = prAsciiEqualsFold(side, "left") || prAsciiEqualsFold(side, "leftoverlap");
        cssHintAdd(out, "position", "absolute");
        cssHintAdd(out, left ? "left" : "right", "0");
        cssHintAdd(out, "padding", "0");
        return;
    }
    switch (mathMlCellColumnAlign(cell, column)) {
        case MathMlAlign_Left: cssHintAdd(out, "text-align", "left"); break;
        case MathMlAlign_Right: cssHintAdd(out, "text-align", "right"); break;
        case MathMlAlign_Center: cssHintAdd(out, "text-align", "center"); break;
        default: break;
    }
    switch (mathMlCellRowAlign(cell, rowIndex)) {
        case MathMlVerticalAlign_Top: cssHintAdd(out, "vertical-align", "top"); break;
        case MathMlVerticalAlign_Bottom: cssHintAdd(out, "vertical-align", "bottom"); break;
        case MathMlVerticalAlign_Baseline: cssHintAdd(out, "vertical-align", "baseline"); break;
        case MathMlVerticalAlign_Center:
        case MathMlVerticalAlign_Axis: cssHintAdd(out, "vertical-align", "middle"); break;
        default: break;
    }
    if (table == nullptr)
        return;
    if (span == 1)
        addColumnWidth(out, table, column);
    // A table of MathML 4's model has no spacing outside its outer cells, where its frame spacing pads it instead. A
    // table without its attributes keeps MathML Core's padding on every side of every cell.
    bool mathMl4 = spacesAsMathMl4(table);
    bool lastColumn = row != nullptr && domNodeParentElement(cell) == row && nextCell(cell) == nullptr;
    if (mathMl4 && place.first)
        cssHintAdd(out, "padding-top", "0");
    else if (givesTable(table, "rowspacing") && !place.first)
        addHalfSpacing(out, "padding-top", mathMlTableRowSpacing(table, rowIndex != 0 ? rowIndex - 1 : 0));
    if (mathMl4 && place.last)
        cssHintAdd(out, "padding-bottom", "0");
    else if (givesTable(table, "rowspacing") && !place.last)
        addHalfSpacing(out, "padding-bottom", mathMlTableRowSpacing(table, rowIndex));
    if (mathMl4 && column == 0)
        cssHintAdd(out, "padding-left", "0");
    else if (givesTable(table, "columnspacing") && column != 0)
        addHalfSpacing(out, "padding-left", mathMlTableColumnSpacing(table, column - 1));
    if (mathMl4 && lastColumn)
        cssHintAdd(out, "padding-right", "0");
    else if (givesTable(table, "columnspacing") && !lastColumn)
        addHalfSpacing(out, "padding-right", mathMlTableColumnSpacing(table, column + span - 1));
    if (!place.last)
        addLine(out, "border-bottom", mathMlTableRowLine(table, rowIndex));
    if (!lastColumn)
        addLine(out, "border-right", mathMlTableColumnLine(table, column + span - 1));
}

// Returns true if the msgroup, msrow, mscarries or mstyle `element` passes its children through to the stack that holds
// it (mathml4/#presm_elementary). A row, group or carries does so as a child of an mstack or an msgroup, or as a child
// of an mlongdiv after its divisor and result. An mstyle in such a row does so too, and its children belong to the row
// (mathml4/#presm_msrow).
bool passesToStack(const PrDomElement* element) {
    const PrDomElement* parent = domNodeParentElement(element);
    if (parent == nullptr || parent->space != PrDomNamespace_MathMl)
        return false;
    if (element->tag == PrDomTag_Mstyle)
        return parent->tag == PrDomTag_Msrow && passesToStack(parent);
    if (parent->tag == PrDomTag_Mstack || parent->tag == PrDomTag_Msgroup)
        return true;
    if (parent->tag != PrDomTag_Mlongdiv)
        return false;
    uint32_t index = 0;
    for (const PrDomNode* node = parent->first; node != nullptr && node != element; node = node->next)
        index += mathMlIsElement(node) ? 1 : 0;
    return index >= 2;
}

// Returns true if the element is one that may pass its children through to a stack.
bool isStackGroup(const PrDomElement* element) {
    return element->tag == PrDomTag_Msgroup || element->tag == PrDomTag_Msrow || element->tag == PrDomTag_Mscarries ||
        element->tag == PrDomTag_Mstyle;
}

// Returns true if the element is an munder, mover or munderover.
bool isUnderOver(const PrDomElement* PR_NULLABLE element) {
    return mathMlIs(element, PrDomTag_Munder) || mathMlIs(element, PrDomTag_Mover) ||
        mathMlIs(element, PrDomTag_Munderover);
}

// Appends the rules that the user agent's sheet gives an accent script and the base of an accent overscript, if the
// child `element` of an munder, mover or munderover is such a part. The sheet's rules match the element's own accent
// attributes, but MathML 4 also makes a script an accent by the default that an mstyle gives (mathml4/#presm_mstyle),
// or by its core operator if neither gives one (mathml4/#presm_munder). An accent keeps its base's size, and the base
// of an accent overscript is cramped.
void addDefaultAccent(PrTextBuffer* out, const PrDomElement* element) {
    const PrDomElement* parent = domNodeParentElement(element);
    if (!isUnderOver(parent))
        return;
    // The parts are the in-flow children. Hints take every element child as in flow, because hints are computed before
    // the boxes that decide what is out of flow.
    const MathMlFlow flow{};
    MathMlUnderOver resolved = mathMlUnderOverAccents(parent, flow);
    const PrDomElement* base = mathMlFirstInFlow(parent, flow);
    const PrDomElement* first = base != nullptr ? mathMlNextInFlow(parent, base, flow) : nullptr;
    const PrDomElement* second = first != nullptr ? mathMlNextInFlow(parent, first, flow) : nullptr;
    bool over = resolved.accent == MathMlBoolean_True;
    bool under = resolved.accentUnder == MathMlBoolean_True;
    const PrDomElement* overscript = mathMlIs(parent, PrDomTag_Mover) ? first : second;
    const PrDomElement* underscript = mathMlIs(parent, PrDomTag_Mover) ? nullptr : first;
    if ((over && element == overscript) || (under && element == underscript))
        cssHintAdd(out, "font-size", "inherit");
    else if (over && element == base)
        cssHintAdd(out, "math-shift", "compact");
}

// Appends the hints of a MathML element's attributes.
void addMathMl(PrTextBuffer* out, const PrDomElement* element) {
    addDefaultAccent(out, element);
    if (isStackGroup(element) && passesToStack(element))
        cssHintAdd(out, "display", "contents");
    PrStringView dir = stringTrimBy(domAttributeValue(element, "dir"), prAsciiIsSpace);
    if (prAsciiEqualsFold(dir, "ltr"))
        cssHintAdd(out, "direction", "ltr");
    else if (prAsciiEqualsFold(dir, "rtl"))
        cssHintAdd(out, "direction", "rtl");
    PrStringView color = domAttributeValue(element, "mathcolor");
    if (color.data != nullptr && isOneValue(color))
        cssHintAdd(out, "color", color);
    PrStringView background = domAttributeValue(element, "mathbackground");
    if (background.data != nullptr && isOneValue(background))
        cssHintAdd(out, "background-color", background);
    // mathsize is a <length-percentage> (mathml-core/#legacy-mathml-style-attributes) or one of MathML 4's keywords
    // (mathml4/#presm_commatt). MathML 4 leaves the sizes of the keywords to the renderer, and small and big scale by
    // the factor of one script level, 0.71, and by its inverse.
    PrStringView size = stringTrimBy(domAttributeValue(element, "mathsize"), prAsciiIsSpace);
    MathMlLength length{};
    if (prAsciiEqualsFold(size, "small"))
        cssHintAdd(out, "font-size", "71%");
    else if (prAsciiEqualsFold(size, "big"))
        cssHintAdd(out, "font-size", "calc(100% / 0.71)");
    else if (size.data != nullptr && !prAsciiEqualsFold(size, "normal") &&
            mathMlParseLength(size, MathMlLengthSyntax_Normal, &length))
        addLength(out, "font-size", length, true);
    MathMlBoolean display = mathMlParseBoolean(domAttributeValue(element, "displaystyle"));
    if (display == MathMlBoolean_True)
        cssHintAdd(out, "math-style", "normal");
    else if (display == MathMlBoolean_False)
        cssHintAdd(out, "math-style", "compact");
    int32_t level = 0;
    bool relative = false;
    if (mathMlParseScriptLevel(domAttributeValue(element, "scriptlevel"), &level, &relative)) {
        prTextBufferAppend(out, "math-depth:");
        if (relative)
            prTextBufferAppend(out, "add(");
        cssSerializeNumber(out, double(level));
        if (relative)
            prTextBufferAppendChar(out, ')');
        prTextBufferAppendChar(out, ';');
    }
    // A token's mathvariant sets its text in a style of the Mathematical Alphanumeric Symbols (mathml4/#presm_commatt),
    // which the engine's private values of text-transform name, and normal on an mi sets it as it is written
    // (mathml-core/#the-mathvariant-attribute).
    PrUnicodeMathVariant variant = 0;
    if (mathMlTokenVariant(element, &variant)) {
        static constexpr PrStringView kVariants[] = {
            "-pr-math-bold", "-pr-math-italic", "-pr-math-bold-italic", "-pr-math-script", "-pr-math-bold-script",
            "-pr-math-fraktur", "-pr-math-double-struck", "-pr-math-bold-fraktur", "-pr-math-sans-serif",
            "-pr-math-bold-sans-serif", "-pr-math-sans-serif-italic", "-pr-math-sans-serif-bold-italic",
            "-pr-math-monospace", "-pr-math-initial", "-pr-math-tailed", "-pr-math-looped", "-pr-math-stretched",
        };
        static_assert(countOf(kVariants) == PrUnicodeMathVariant_Count);
        cssHintAdd(out, "text-transform", kVariants[variant]);
    } else if (element->tag == PrDomTag_Mi &&
            prAsciiEqualsFold(stringTrimBy(domAttributeValue(element, "mathvariant"), prAsciiIsSpace), "normal")) {
        cssHintAdd(out, "text-transform", "none");
    }
    switch (element->tag) {
        case PrDomTag_Mspace: {
            // The width, and the height and depth together as the height, unless they are percentages
            // (mathml-core/#space-mspace).
            MathMlSpace space = mathMlSpace(element);
            // A negative width, which CSS's width rejects, moves what follows back over the space
            // (mathml4/#presm_mspace), as a negative inline-end margin does.
            if (space.width.count == 1 && space.width.values[0] < 0 && !mathMlLengthHasPercent(space.width)) {
                cssHintAdd(out, "width", "0");
                addLength(out, "margin-inline-end", space.width, false);
            } else {
                addLength(out, "width", space.width, false);
            }
            bool height = mathMlLengthIsPresent(space.height) && !mathMlLengthHasPercent(space.height);
            bool depth = mathMlLengthIsPresent(space.depth) && !mathMlLengthHasPercent(space.depth);
            if (height && depth) {
                MathMlLength sum = space.height;
                for (uint32_t i = 0; i < space.depth.count; ++i)
                    mathMlLengthAdd(&sum, double(space.depth.values[i]), space.depth.units[i]);
                addLength(out, "height", sum, false);
            } else if (height || depth) {
                addLength(out, "height", height ? space.height : space.depth, false);
            }
            break;
        }
        case PrDomTag_Mpadded:
            addLength(out, "width", mathMlPadded(element).width, false);
            break;
        case PrDomTag_Mglyph: {
            // The image's size, whose ratio CSS keeps if only one is given, and the shift of its bottom above the
            // baseline (mathml4/#presm_mglyph). In a token, it is set in the token's line as a character is.
            MathMlGlyph glyph = mathMlGlyph(element);
            const PrDomElement* parent = domNodeParentElement(element);
            if (parent != nullptr && parent->space == PrDomNamespace_MathMl && mathMlIsTokenElement(parent))
                cssHintAdd(out, "display", "inline");
            addLength(out, "width", glyph.width, false);
            addLength(out, "height", glyph.height, false);
            addLength(out, "vertical-align", glyph.valign, false);
            break;
        }
        case PrDomTag_Mtd:
            addCell(out, element);
            break;
        case PrDomTag_Mtable: {
            MathMlTable table = mathMlTable(element);
            // MathML 4 sizes a table by its width (mathml4/#presm_mtable).
            addLength(out, "width", table.width, true);
            if (hasLabels(element))
                cssHintAdd(out, "position", "relative");
            addLine(out, "border", table.frame);
            if (table.frame != MathMlLine_None) {
                // The default frame spacing is 0.4em along the inline axis and 0.5ex along the block axis.
                MathMlLength inlineSpacing = table.frameSpacing[0];
                MathMlLength blockSpacing = table.frameSpacing[1];
                if (!mathMlLengthIsPresent(inlineSpacing))
                    mathMlParseLength("0.4em", MathMlLengthSyntax_Normal, &inlineSpacing);
                if (!mathMlLengthIsPresent(blockSpacing))
                    mathMlParseLength("0.5ex", MathMlLengthSyntax_Normal, &blockSpacing);
                addLength(out, "padding-inline", inlineSpacing, true);
                addLength(out, "padding-block", blockSpacing, true);
            }
            break;
        }
        default: break;
    }
}

// The attributes that are hints on every MathML element.
constexpr PrStringView kGlobal[] = {"dir", "mathcolor", "mathbackground", "mathsize", "displaystyle", "scriptlevel"};

// The attributes of a table and of a row that the hints of the table's cells read.
constexpr PrStringView kTable[] = {
    "rowalign", "columnalign", "rowspacing", "columnspacing", "rowlines", "columnlines", "columnwidth",
};

// The attributes whose defaults an mstyle or a math element gives (mathMlInherits) that the hints of the elements under
// it read: a token's mathvariant, the accents of an munder, an mover or an munderover and of its core operator, and the
// attributes of a table that its cells read.
constexpr PrStringView kInheritedReads[] = {
    "mathvariant", "accent", "accentunder", "form", "rowalign", "columnalign", "rowspacing", "columnspacing",
    "rowlines", "columnlines", "columnwidth", "frame", "framespacing",
};

// Returns the elements that a change to the attribute `name` of the MathML element restyles through hints.
CssHintReach mapped(const PrDomElement* element, PrStringView name) {
    for (PrStringView global : kGlobal) {
        if (name == global)
            return CssHintReach_Self;
    }
    if (mathMlIsTokenElement(element) && name == "mathvariant")
        return CssHintReach_Self;
    switch (element->tag) {
        case PrDomTag_Mstyle:
        case PrDomTag_Math:
            for (PrStringView inherited : kInheritedReads) {
                if (name == inherited)
                    return CssHintReach_Readers;
            }
            return CssHintReach_None;
        case PrDomTag_Mspace:
            if (name == "width" || name == "height" || name == "depth")
                return CssHintReach_Self;
            return CssHintReach_None;
        case PrDomTag_Mpadded: return name == "width" ? CssHintReach_Self : CssHintReach_None;
        case PrDomTag_Mglyph:
            if (name == "width" || name == "height" || name == "valign")
                return CssHintReach_Self;
            return CssHintReach_None;
        case PrDomTag_Mtd:
            if (name == "rowalign" || name == "columnalign" || name == "columnspan" || name == "rowspan")
                return CssHintReach_Self;
            return CssHintReach_None;
        case PrDomTag_Munder:
        case PrDomTag_Mover:
        case PrDomTag_Munderover:
            // The hints of the parts read whether the attributes are present.
            return name == "accent" || name == "accentunder" ? CssHintReach_Readers : CssHintReach_None;
        case PrDomTag_Mo:
            // The hints of the parts of an munder, an mover or an munderover read whether its core operator is an
            // accent, which these decide.
            return name == "accent" || name == "form" ? CssHintReach_Readers : CssHintReach_None;
        case PrDomTag_Mtr:
        case PrDomTag_Mlabeledtr:
        case PrDomTag_Mtable:
            if (name == "width")
                return element->tag == PrDomTag_Mtable ? CssHintReach_Self : CssHintReach_None;
            if (name == "frame" || name == "framespacing")
                return element->tag == PrDomTag_Mtable ? CssHintReach_Readers : CssHintReach_None;
            for (PrStringView table : kTable) {
                if (name == table)
                    return CssHintReach_Readers;
            }
            return CssHintReach_None;
        default: return CssHintReach_None;
    }
}

// Returns true if an attribute of the element may map to a hint.
bool hasOwn(const PrDomElement* element) {
    if (element->tag == PrDomTag_Mtd || element->tag == PrDomTag_Mglyph || isStackGroup(element) ||
            (element->tag == PrDomTag_Mtable && hasLabels(element)) || isUnderOver(domNodeParentElement(element)))
        return true;
    for (const PrDomNode* node = element->firstAttribute; node != nullptr; node = node->next) {
        const PrDomAttr* attribute = static_cast<const PrDomAttr*>(node);
        if (attribute->space == PrDomNamespace_None && mapped(element, prRawStringView(&attribute->name)) != 0)
            return true;
    }
    return false;
}

// Calls `fn` with the cells of the row `row`.
void eachCell(PrDomElement* row, void (*fn)(void* PR_NULLABLE context, PrDomElement* reader), void* PR_NULLABLE context) {
    for (PrDomNode* node = static_cast<PrDomNode*>(row)->first; node != nullptr; node = node->next) {
        PrDomElement* cell = domElementFromNode(node);
        if (mathMlIs(cell, PrDomTag_Mtd))
            fn(context, cell);
    }
}

// Calls `fn` with the cells of the rows of `table` from `from` on, or with every cell if `from` is null.
void eachCellFrom(PrDomElement* table, PrDomNode* PR_NULLABLE from,
        void (*fn)(void* PR_NULLABLE context, PrDomElement* reader), void* PR_NULLABLE context) {
    for (PrDomNode* node = from != nullptr ? from : static_cast<PrDomNode*>(table)->first; node != nullptr;
            node = node->next) {
        if (isRow(node))
            eachCell(domElementFromNode(node), fn, context);
    }
}

// Returns the element that precedes `node` among its siblings, or null.
PrDomNode* PR_NULLABLE previousElement(PrDomNode* node) {
    PrDomNode* at = node->previous;
    while (at != nullptr && at->type != PrDomNodeType_Element)
        at = at->previous;
    return at;
}

// Returns the parent of `node` if it is an element, or null.
PrDomElement* PR_NULLABLE parentOf(PrDomNode* node) {
    return node->parent != nullptr ? domElementFromNode(node->parent) : nullptr;
}

// Returns true if the core operator of the element may come from its descendants (mathMlCoreOperator).
bool passesCore(const PrDomElement* element) {
    return element->space == PrDomNamespace_MathMl &&
        (element->tag == PrDomTag_Mo || mathMlIsScriptedElement(element) || element->tag == PrDomTag_Mfrac ||
            mathMlIsGroupingElement(element) || element->tag == PrDomTag_Mpadded);
}

// Calls `fn` with the elements whose hints read the core operator of `element`, which a change to its subtree or to its
// own attributes may have changed: the parts of each munder, mover or munderover that has a script whose core operator
// the change reaches, whose accents follow from that operator (addDefaultAccent).
void eachCoreReader(PrDomElement* element, void (*fn)(void* PR_NULLABLE context, PrDomElement* reader),
        void* PR_NULLABLE context) {
    for (PrDomElement* at = element; at != nullptr && passesCore(at); at = parentOf(at)) {
        PrDomElement* parent = parentOf(at);
        if (!isUnderOver(parent))
            continue;
        for (PrDomNode* child = static_cast<PrDomNode*>(parent)->first; child != nullptr; child = child->next) {
            if (PrDomElement* part = domElementFromNode(child))
                fn(context, part);
        }
    }
}

// The hints of a cell read the attributes of its row and its table (addCell), those of the children of an munder, an
// mover or an munderover read whether their parent's accent attributes are present, and whether the core operator of a
// script is an accent, which the operator's attributes decide (addDefaultAccent). The hints of the elements under an
// mstyle or a math element read the defaults that it gives (kInheritedReads), as do the parts of an munder, an mover or
// an munderover whose script's core operator an mstyle holds.
void readers(PrDomElement* element, void (*fn)(void* PR_NULLABLE context, PrDomElement* reader), void* PR_NULLABLE context) {
    PrDomNode* node = element;
    if (element->tag == PrDomTag_Mstyle || element->tag == PrDomTag_Math) {
        for (PrDomNode* at = node->first; at != nullptr; at = domNodeNext(node, at)) {
            if (mathMlIsElement(at))
                fn(context, domElementFromNode(at));
        }
        eachCoreReader(element, fn, context);
    } else if (isUnderOver(element)) {
        for (PrDomNode* child = node->first; child != nullptr; child = child->next) {
            if (PrDomElement* part = domElementFromNode(child))
                fn(context, part);
        }
    } else if (element->tag == PrDomTag_Mtr || element->tag == PrDomTag_Mlabeledtr) {
        eachCell(element, fn, context);
    } else if (element->tag == PrDomTag_Mtable) {
        eachCellFrom(element, nullptr, fn, context);
    } else if (element->tag == PrDomTag_Mo) {
        eachCoreReader(element, fn, context);
    }
}

// The hints of a MathML element read its parent and its grandparent: a cell its row and table, a part of an munder, an
// mover or an munderover its parent, a row of a stack its stack and an mglyph its token. The hints of some elements
// also read the children of their parent, or of themselves: a cell its column and its row's place among the table's
// rows, a table whether it has a labeled row, a part of an munder, an mover or an munderover which part it is and
// whether the core operator of a script is an accent, and a child of an mlongdiv its place among the children.
void structureReaders(PrDomNode* node, void (*fn)(void* PR_NULLABLE context, PrDomElement* reader), void* PR_NULLABLE context) {
    if (mathMlIsElement(node)) {
        fn(context, domElementFromNode(node));
        for (PrDomNode* child = node->first; child != nullptr; child = child->next) {
            if (mathMlIsElement(child))
                fn(context, domElementFromNode(child));
        }
    }
    PrDomElement* parent = parentOf(node);
    if (parent == nullptr || parent->space != PrDomNamespace_MathMl)
        return;
    eachCoreReader(parent, fn, context);
    switch (parent->tag) {
        case PrDomTag_Mtr:
        case PrDomTag_Mlabeledtr: {
            // The cells after `node` move to other columns, and the one before it may stop being the last. In a labeled
            // row, the first cell is the label. A table whose columns have fit widths shares its width among as many
            // columns as its longest row spans.
            PrDomElement* table = const_cast<PrDomElement*>(tableOf(parent));
            if (table != nullptr && givesTable(table, "columnwidth")) {
                eachCellFrom(table, nullptr, fn, context);
            } else if (parent->tag == PrDomTag_Mlabeledtr) {
                eachCell(parent, fn, context);
            } else {
                PrDomNode* from = previousElement(node);
                for (PrDomNode* at = from != nullptr ? from : node->next; at != nullptr; at = at->next) {
                    if (mathMlIs(domElementFromNode(at), PrDomTag_Mtd))
                        fn(context, domElementFromNode(at));
                }
            }
            break;
        }
        case PrDomTag_Mtable: {
            // The rows after `node` move to other places, and the one before it may stop being the last.
            fn(context, parent);
            PrDomNode* from = previousElement(node);
            eachCellFrom(parent,
                givesTable(parent, "columnwidth") ? nullptr : (from != nullptr ? from : node->next), fn, context);
            break;
        }
        case PrDomTag_Munder:
        case PrDomTag_Mover:
        case PrDomTag_Munderover:
            for (PrDomNode* child = static_cast<PrDomNode*>(parent)->first; child != nullptr; child = child->next) {
                if (PrDomElement* part = domElementFromNode(child))
                    fn(context, part);
            }
            break;
        case PrDomTag_Mlongdiv:
            // The children after `node` move to other places, which decides whether they pass their children to the
            // stack, as do the mstyle children of an msrow.
            for (PrDomNode* at = node->next; at != nullptr; at = at->next) {
                PrDomElement* child = domElementFromNode(at);
                if (child == nullptr)
                    continue;
                fn(context, child);
                for (PrDomNode* inner = at->first; inner != nullptr; inner = inner->next) {
                    if (mathMlIs(domElementFromNode(inner), PrDomTag_Mstyle))
                        fn(context, domElementFromNode(inner));
                }
            }
            break;
        default: break;
    }
}

constexpr CssHintProvider kHints = {
    .hasOwn = hasOwn,
    .own = addMathMl,
    .mapped = mapped,
    .readers = readers,
    .structureReaders = structureReaders,
};

} // namespace

const CssHintProvider* mathMlHints() {
    return &kHints;
}

} // namespace pr_impl
