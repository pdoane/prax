// Rendering documents: the computed style of each element, projected onto a layout tree.
//
// css-cascade-5 - https://drafts.csswg.org/css-cascade-5/
// css-display-4 - https://drafts.csswg.org/css-display-4/
// css-backgrounds-3 - https://drafts.csswg.org/css-backgrounds-3/
// css-transforms-1 - https://drafts.csswg.org/css-transforms-1/
// css-fonts-4 - https://drafts.csswg.org/css-fonts-4/

#include "pr/css_presentation.h"
#include "css/src/css_presentation.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/color_space.h"
#include "pr/count_of.h"
#include "pr/draw_list.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/locale_common.h"
#include "pr/mem.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/text_break.h"
#include "pr/text_buffer.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/url.h"

#include "css/src/css_animation.h"
#include "css/src/css_boxes.h"
#include "css/src/css_cascade.h"
#include "css/src/css_color_value.h"
#include "css/src/css_computed.h"
#include "css/src/css_counter_pass.h"
#include "css/src/css_filter.h"
#include "css/src/css_font_face.h"
#include "css/src/css_gradient.h"
#include "css/src/css_grid.h"
#include "css/src/css_hints.h"
#include "css/src/css_inline_style.h"
#include "css/src/css_keyword.h"
#include "css/src/css_math.h"
#include "css/src/css_media.h"
#include "css/src/css_object.h"
#include "css/src/css_presentation_hooks.h"
#include "css/src/css_root.h"
#include "css/src/css_serialize.h"
#include "css/src/css_shape.h"
#include "css/src/css_unit.h"
#include "dom/src/dom_attribute.h"
#include "dom/src/dom_class.h"
#include "dom/src/dom_control.h"
#include "dom/src/dom_element_state.h"
#include "dom/src/dom_flat.h"
#include "dom/src/dom_microsyntax.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_resource.h"
#include "dom/src/dom_slot.h"
#include "dom/src/dom_use.h"

namespace pr_impl {

namespace {

bool hasKeyword(CssValueRun run, PrCssKeyword keyword) {
    for (uint32_t i = 0; i < run.count; ++i) {
        if (cssValueIsKeyword(&run.values[i], keyword))
            return true;
    }
    return false;
}

// Appends the node at `index` of a computed calculation tree, and the nodes of its operands, to `out` as layout
// calculation nodes. Returns false for a node that layout calculations cannot hold.
bool calcNodes(CssValueRun run, uint32_t index, Array<PrLayoutCalcNode>& out) {
    const CssValueNode* value = &run.values[index];
    auto leaf = [&](PrLayoutCalcLeaf kind, double number) {
        out.push(prLayoutCalcLeaf(kind, number));
        return true;
    };
    // Appends an operation, then the operands among the node's children, skipping commas and keywords.
    auto operation = [&](PrLayoutCalcOp op) {
        uint32_t at = out.count;
        out.push(prLayoutCalcOperation(op, 0));
        uint32_t operands = 0;
        CssValueRun children = cssValueChildren(run, index);
        for (uint32_t i = 0; i < children.count; i = i + children.values[i].next) {
            const CssValueNode& child = children.values[i];
            if (child.kind == PrCssValueKind_Literal ||
                    (child.kind == PrCssValueKind_Keyword && cssValueKeyword(&child) != PrCssKeyword_None))
                continue;
            if (!calcNodes(children, i, out))
                return false;
            ++operands;
        }
        out.data[at] = prLayoutCalcOperation(op, operands);
        return true;
    };
    switch (value->kind) {
        case PrCssValueKind_Math:
            return calcNodes(run, index + 1, out);
        case PrCssValueKind_Number:
        case PrCssValueKind_Integer:
            return leaf(PrLayoutCalcLeaf_Number, value->number);
        case PrCssValueKind_Percentage:
            return leaf(PrLayoutCalcLeaf_Percent, value->number);
        case PrCssValueKind_Dimension: {
            double scale = cssUnitCategory(value->unit) == CssUnitCategory_Length ? cssUnitScale(value->unit) : 0;
            return scale != 0 && leaf(PrLayoutCalcLeaf_Px, value->number * scale);
        }
        case PrCssValueKind_Keyword:
            return cssValueKeyword(value) == PrCssKeyword_None && leaf(PrLayoutCalcLeaf_None, 0);
        case PrCssValueKind_Calc:
            switch (value->aux & CssValueAuxFlag_CalcOperator) {
                case PrCssCalc_Sum:     return operation(PrLayoutCalcOp_Sum);
                case PrCssCalc_Product: return operation(PrLayoutCalcOp_Product);
                case PrCssCalc_Negate:  return operation(PrLayoutCalcOp_Negate);
                default:                      return operation(PrLayoutCalcOp_Invert);
            }
        case PrCssValueKind_Function: {
            PrCssKeyword strategy = PrCssKeyword_Nearest;
            CssValueRun children = cssValueChildren(run, index);
            for (uint32_t i = 0; i < children.count; i = i + children.values[i].next) {
                if (children.values[i].kind == PrCssValueKind_Keyword)
                    strategy = cssValueKeyword(&children.values[i]);
            }
            switch (cssMathFunction(cssValueKeyword(value))) {
                case CssValueDef_MinFn:     return operation(PrLayoutCalcOp_Min);
                case CssValueDef_MaxFn:     return operation(PrLayoutCalcOp_Max);
                case CssValueDef_ClampFn:   return operation(PrLayoutCalcOp_Clamp);
                case CssValueDef_HypotFn:   return operation(PrLayoutCalcOp_Hypot);
                case CssValueDef_ModFn:     return operation(PrLayoutCalcOp_Mod);
                case CssValueDef_RemFn:     return operation(PrLayoutCalcOp_Rem);
                case CssValueDef_AbsFn:     return operation(PrLayoutCalcOp_Abs);
                case CssValueDef_SignFn:    return operation(PrLayoutCalcOp_Sign);
                case CssValueDef_RoundFn:
                    if (strategy == PrCssKeyword_Up)
                        return operation(PrLayoutCalcOp_RoundUp);
                    if (strategy == PrCssKeyword_Down)
                        return operation(PrLayoutCalcOp_RoundDown);
                    if (strategy == PrCssKeyword_ToZero)
                        return operation(PrLayoutCalcOp_RoundToZero);
                    return operation(PrLayoutCalcOp_RoundNearest);
                default:
                    return false;
            }
        }
        default:
            return false;
    }
}

// Returns a length in the form that layout takes: pixels, a percentage, a calculation of both, auto, none, stretch,
// min-content, max-content or fit-content. A calculation that layout calculations cannot hold becomes auto, and so does
// a sizing value that layout does not support, such as fit-content().
PrLayoutLength lengthOf(PrCssPresentation* r, CssValueRun run) {
    PrCssKeyword keyword = cssValueRunFirstKeyword(run);
    if (keyword == PrCssKeyword_None)
        return prLayoutLengthNone();
    if (keyword == PrCssKeyword_Stretch)
        return prLayoutLengthStretch();
    if (keyword == PrCssKeyword_MinContent)
        return prLayoutLengthMinContent();
    if (keyword == PrCssKeyword_MaxContent)
        return prLayoutLengthMaxContent();
    if (keyword == PrCssKeyword_FitContent)
        return prLayoutLengthFitContent();
    if (keyword != PrCssKeyword_Count || run.count == 0)
        return prLayoutLengthAuto();
    const CssValueNode* value = &run.values[0];
    if (value->kind == PrCssValueKind_Percentage)
        return prLayoutLengthPercent(float(value->number));
    if (value->kind != PrCssValueKind_Math) {
        double px = 0;
        return cssResolveLength(run, 0, 0, &px) ? prLayoutLengthPx(float(px)) : prLayoutLengthAuto();
    }
    r->calc.clear();
    if (!calcNodes(run, 0, r->calc))
        return prLayoutLengthAuto();
    return prLayoutLengthIntern(r->tree, r->calc.data, r->calc.count);
}

double pxOf(CssValueRun run) {
    double px = 0;
    cssResolveLength(run, 0, 0, &px);
    return px;
}

// The effects that a will-change value asks for ahead of a change (css-will-change-1/#will-change): a property that it
// names makes the containing block and the stacking context that a value of that property other than its initial one
// would.
struct WillChange {
    bool transforms;                // a transform or a perspective: a containing block for all descendants
    bool composites;                // opacity: a stacking context that composites as a layer, and a backdrop root
    bool filters;                   // a filter or a backdrop filter: a containing block for all descendants, and a
                                    // backdrop root
    bool groups;                    // a clip path, a mask or a blend mode: a backdrop root
};

WillChange willChangeOf(CssValueRun run) {
    WillChange will{};
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        PrStringView name;
        if (value->kind == PrCssValueKind_Keyword)
            name = cssKeywordName(cssValueKeyword(value));
        else if (value->kind == PrCssValueKind_Ident)
            name = cssValueText(run, value);
        else
            name = PrStringView{};
        const PrStringView transforms[] = {"transform", "translate", "rotate", "scale", "perspective"};
        for (PrStringView property : transforms)
            will.transforms = will.transforms || prAsciiEqualsFold(name, property);
        will.composites = will.composites || prAsciiEqualsFold(name, "opacity");
        will.filters = will.filters || prAsciiEqualsFold(name, "filter") || prAsciiEqualsFold(name, "backdrop-filter");
        const PrStringView groups[] = {"clip-path", "mask", "mask-image", "mask-border", "mix-blend-mode"};
        for (PrStringView property : groups)
            will.groups = will.groups || prAsciiEqualsFold(name, property);
    }
    return will;
}

// Returns true if the display generates a ruby box (css-ruby-1/#ruby-display). Every ruby box is inline-level.
bool rubyDisplay(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Ruby:
        case PrCssKeyword_RubyBase:
        case PrCssKeyword_RubyText:
        case PrCssKeyword_RubyBaseContainer:
        case PrCssKeyword_RubyTextContainer:
            return true;
        default:
            return false;
    }
}

// Returns the kind of box that a computed display value generates. The parser holds the two-keyword forms as their
// legacy keywords. The engine lays out every inner display other than flow, flow-root, flex, grid and table as flow. It
// lays out an inline-level box of another inner display as an inline box. A ruby box is such a box.
PrLayoutDisplay displayOf(CssValueRun run) {
    if (hasKeyword(run, PrCssKeyword_None))
        return PrLayoutDisplay_None;
    if (hasKeyword(run, PrCssKeyword_InlineBlock))
        return PrLayoutDisplay_InlineBlock;
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_InlineFlex: return PrLayoutDisplay_InlineFlex;
        case PrCssKeyword_Flex: return PrLayoutDisplay_Flex;
        case PrCssKeyword_InlineGrid: return PrLayoutDisplay_InlineGrid;
        case PrCssKeyword_Grid: return PrLayoutDisplay_Grid;
        case PrCssKeyword_Table: return PrLayoutDisplay_Table;
        case PrCssKeyword_InlineTable: return PrLayoutDisplay_InlineTable;
        case PrCssKeyword_TableRowGroup: return PrLayoutDisplay_TableRowGroup;
        case PrCssKeyword_TableHeaderGroup: return PrLayoutDisplay_TableHeaderGroup;
        case PrCssKeyword_TableFooterGroup: return PrLayoutDisplay_TableFooterGroup;
        case PrCssKeyword_TableRow: return PrLayoutDisplay_TableRow;
        case PrCssKeyword_TableColumnGroup: return PrLayoutDisplay_TableColumnGroup;
        case PrCssKeyword_TableColumn: return PrLayoutDisplay_TableColumn;
        case PrCssKeyword_TableCell: return PrLayoutDisplay_TableCell;
        case PrCssKeyword_TableCaption: return PrLayoutDisplay_TableCaption;
        default: break;
    }
    if (hasKeyword(run, PrCssKeyword_Inline) || hasKeyword(run, PrCssKeyword_Contents) || rubyDisplay(run))
        return PrLayoutDisplay_Inline;
    if (hasKeyword(run, PrCssKeyword_FlowRoot))
        return PrLayoutDisplay_FlowRoot;
    return PrLayoutDisplay_Block;
}

// Returns the alignment that a computed alignment property names (css-align-3/#alignment-values), with its overflow
// position. Returns `otherwise` if the keywords name no alignment that a flex container reads.
PrLayoutAlignment alignmentOf(CssValueRun run, PrLayoutAlignment otherwise) {
    PrLayoutAlignment value = otherwise;
    uint32_t flags = 0;
    bool last = false;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (run.values[i].kind != PrCssValueKind_Keyword)
            continue;
        switch (cssValueKeyword(&run.values[i])) {
            case PrCssKeyword_Safe: flags |= PrLayoutAlignmentFlag_Safe; break;
            case PrCssKeyword_Unsafe: flags |= PrLayoutAlignmentFlag_Unsafe; break;
            case PrCssKeyword_Last: last = true; break;
            case PrCssKeyword_Baseline:
                value = last ? PrLayoutAlignment_LastBaseline : PrLayoutAlignment_Baseline;
                break;
            case PrCssKeyword_Auto: value = PrLayoutAlignment_Auto; break;
            case PrCssKeyword_Normal: value = PrLayoutAlignment_Normal; break;
            case PrCssKeyword_Stretch: value = PrLayoutAlignment_Stretch; break;
            case PrCssKeyword_Center: value = PrLayoutAlignment_Center; break;
            case PrCssKeyword_Start: value = PrLayoutAlignment_Start; break;
            case PrCssKeyword_End: value = PrLayoutAlignment_End; break;
            case PrCssKeyword_SelfStart: value = PrLayoutAlignment_SelfStart; break;
            case PrCssKeyword_SelfEnd: value = PrLayoutAlignment_SelfEnd; break;
            case PrCssKeyword_FlexStart: value = PrLayoutAlignment_FlexStart; break;
            case PrCssKeyword_FlexEnd: value = PrLayoutAlignment_FlexEnd; break;
            case PrCssKeyword_Left: value = PrLayoutAlignment_Left; break;
            case PrCssKeyword_Right: value = PrLayoutAlignment_Right; break;
            case PrCssKeyword_SpaceBetween: value = PrLayoutAlignment_SpaceBetween; break;
            case PrCssKeyword_SpaceAround: value = PrLayoutAlignment_SpaceAround; break;
            case PrCssKeyword_SpaceEvenly: value = PrLayoutAlignment_SpaceEvenly; break;
            default: break;
        }
    }
    return PrLayoutAlignment(value | flags);
}

// Returns the number that a computed flex-grow or flex-shrink holds.
float factorOf(CssValueRun run, float otherwise) {
    bool number = run.count == 1 &&
        (run.values[0].kind == PrCssValueKind_Number || run.values[0].kind == PrCssValueKind_Integer);
    return number ? float(run.values[0].number) : otherwise;
}

// Sets the preferred aspect ratio of `box` from a computed aspect-ratio (css-sizing-4/#aspect-ratio). The ratio is the
// width over the height, or zero for auto or a degenerate ratio. Also records whether auto was given with the ratio.
void aspectRatioOf(CssValueRun run, PrLayoutBoxStyle& box) {
    double terms[2] = {0, 0};
    uint32_t count = 0;
    for (uint32_t i = 0; i < run.count && count < 2; i = i + run.values[i].next) {
        const CssValueNode& value = run.values[i];
        if (value.kind == PrCssValueKind_Number || value.kind == PrCssValueKind_Integer)
            terms[count++] = value.number;
    }
    double ratio = count == 2 && terms[0] > 0 && terms[1] > 0 ? terms[0] / terms[1] : 0;
    box.aspectRatio = prF64IsFinite(ratio) && float(ratio) > 0 && prF32IsFinite(float(ratio)) ? float(ratio) : 0;
    box.aspectRatioAuto = box.aspectRatio > 0 && hasKeyword(run, PrCssKeyword_Auto);
}

// Returns the table group that a computed style projects onto (css-tables-3), with the spans `columnSpan` and `rowSpan`
// that the element's attributes give.
PrLayoutTableStyle tableOf(const CssComputedStyle* style, uint16_t columnSpan, uint16_t rowSpan) {
    PrLayoutTableStyle table = prLayoutTableStyleDefault();
    // The computed value is two absolute lengths, horizontal then vertical.
    CssValueRun spacing = cssComputedValue(style, PrCssProperty_BorderSpacing);
    if (spacing.count != 0) {
        double horizontal = pxOf(cssValueSubtree(spacing, 0));
        uint32_t second = spacing.values[0].next;
        double vertical = second < spacing.count ? pxOf(cssValueSubtree(spacing, second)) : horizontal;
        table.borderSpacing[0] = prLayoutUnitFromPxDouble(horizontal > 0 ? horizontal : 0);
        table.borderSpacing[1] = prLayoutUnitFromPxDouble(vertical > 0 ? vertical : 0);
    }
    if (cssComputedFirstKeyword(style, PrCssProperty_BorderCollapse) == PrCssKeyword_Collapse)
        table.borderCollapse = PrLayoutBorderCollapse_Collapse;
    if (cssComputedFirstKeyword(style, PrCssProperty_CaptionSide) == PrCssKeyword_Bottom)
        table.captionSide = PrLayoutCaptionSide_Bottom;
    if (cssComputedFirstKeyword(style, PrCssProperty_TableLayout) == PrCssKeyword_Fixed)
        table.tableLayout = PrLayoutTableLayout_Fixed;
    table.columnSpan = columnSpan;
    table.rowSpan = rowSpan;
    return table;
}

// Returns the breadth of a track sizing function that the computed value `run` holds (css-grid-1/#track-sizes).
PrLayoutTrackBreadth breadthOf(PrCssPresentation* r, CssValueRun run) {
    if (run.count == 0)
        return prLayoutTrackBreadthAuto();
    const CssValueNode& value = run.values[0];
    if (value.kind == PrCssValueKind_Dimension && value.unit == PrCssUnit_Fr)
        return prLayoutTrackBreadthFlex(float(value.number > 0 ? value.number : 0));
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_MinContent: return prLayoutTrackBreadthMinContent();
        case PrCssKeyword_MaxContent: return prLayoutTrackBreadthMaxContent();
        case PrCssKeyword_Count: break;
        default: return prLayoutTrackBreadthAuto();
    }
    PrLayoutLength length = lengthOf(r, run);
    bool fixed = prLayoutWordKind(length.bits) != PrLayoutWordKind_Keyword;
    return fixed ? prLayoutTrackBreadthLength(length) : prLayoutTrackBreadthAuto();
}

// Returns the track sizing function at `index` of `run`: a breadth, minmax(), or fit-content()
// (css-grid-1/#track-sizes).
PrLayoutGridTrack trackOf(PrCssPresentation* r, CssValueRun run, uint32_t index) {
    const CssValueNode* value = &run.values[index];
    if (cssValueIsFunction(value, PrCssKeyword_Minmax)) {
        CssValueRun args = cssValueChildren(run, index);
        uint32_t second = args.values[0].next;
        if (second < args.count && args.values[second].kind == PrCssValueKind_Literal)
            second += args.values[second].next;
        PrLayoutTrackBreadth max;
        if (second < args.count)
            max = breadthOf(r, cssValueSubtree(args, second));
        else
            max = prLayoutTrackBreadthAuto();
        return PrLayoutGridTrack{breadthOf(r, cssValueSubtree(args, 0)), max, prLayoutLengthPx(0)};
    }
    if (cssValueIsFunction(value, PrCssKeyword_FitContent)) {
        CssValueRun args = cssValueChildren(run, index);
        PrLayoutLength limit = args.count != 0 ? lengthOf(r, cssValueSubtree(args, 0)) : prLayoutLengthPx(0);
        bool fixed = prLayoutWordKind(limit.bits) != PrLayoutWordKind_Keyword;
        return PrLayoutGridTrack{
            prLayoutTrackBreadthAuto(), prLayoutTrackBreadthFitContent(),
            fixed ? limit : prLayoutLengthPx(0),
        };
    }
    PrLayoutTrackBreadth breadth = breadthOf(r, cssValueSubtree(run, index));
    bool flexible = prLayoutWordKind(breadth.bits) == PrLayoutWordKind_Number;
    return PrLayoutGridTrack{flexible ? prLayoutTrackBreadthAuto() : breadth, breadth, prLayoutLengthPx(0)};
}

// Adds the names of the <line-names> block at `*at` of `run`, which starts with its '[', to `names` at `line`, and
// moves `*at` past its ']'.
void lineNamesOf(PrCssPresentation* r, CssValueRun run, uint32_t* at, uint32_t line, Array<PrLayoutGridLineName>& names) {
    uint32_t i = *at + run.values[*at].next;
    for (; i < run.count && !cssValueIsLiteral(&run.values[i], ']'); i += run.values[i].next) {
        if (run.values[i].kind != PrCssValueKind_Ident)
            continue;
        PrLayoutGridName name = prLayoutGridNameIntern(r->tree, cssValueText(run, &run.values[i]));
        names.push(PrLayoutGridLineName{line, name});
    }
    *at = i < run.count ? i + run.values[i].next : i;
}

// Adds the line names and tracks of the list `run`, the content of a repeat() after its count or of a whole track list,
// to the presentation's scratch, with each name at the line before the next track, counted from `first`.
void trackItemsOf(PrCssPresentation* r, CssValueRun run, uint32_t first, Array<PrLayoutGridLineName>& names) {
    for (uint32_t i = 0; i < run.count;) {
        const CssValueNode* value = &run.values[i];
        if (cssValueIsLiteral(value, '[')) {
            lineNamesOf(r, run, &i, r->gridTracks.count - first, names);
            continue;
        }
        if (value->kind != PrCssValueKind_Literal && r->gridTracks.count < kPrLayoutGridMaxLine)
            r->gridTracks.push(trackOf(r, run, i));
        i += value->next;
    }
}

// Returns the track list that a computed grid-template-columns, grid-template-rows or implicit track size holds,
// interned (css-grid-1/#track-sizing). A repetition by a count is expanded, and an automatic repetition is kept as one
// repetition with its names apart. None, and subgrid, which the engine does not build, have no tracks.
PrLayoutGridTracksIndex trackListOf(PrCssPresentation* r, CssValueRun run) {
    PrCssKeyword keyword = cssValueRunFirstKeyword(run);
    if (run.count == 0 || keyword == PrCssKeyword_None || hasKeyword(run, PrCssKeyword_Subgrid))
        return kPrLayoutGridTracksNone;
    r->gridTracks.clear();
    r->gridNames.clear();
    r->gridRepeatNames.clear();
    PrLayoutGridTrackList list{};
    for (uint32_t i = 0; i < run.count;) {
        const CssValueNode* value = &run.values[i];
        if (cssValueIsLiteral(value, '[')) {
            lineNamesOf(r, run, &i, r->gridTracks.count, r->gridNames);
            continue;
        }
        if (!cssValueIsFunction(value, PrCssKeyword_Repeat)) {
            if (value->kind != PrCssValueKind_Literal && r->gridTracks.count < kPrLayoutGridMaxLine)
                r->gridTracks.push(trackOf(r, run, i));
            i += value->next;
            continue;
        }
        // repeat(count, list): the count, a ',' and the list.
        CssValueRun args = cssValueChildren(run, i);
        i += value->next;
        if (args.count == 0)
            continue;
        uint32_t listStart = args.values[0].next;
        if (listStart < args.count && cssValueIsLiteral(&args.values[listStart], ','))
            listStart += args.values[listStart].next;
        CssValueRun items{args.values + listStart, args.count - listStart, args.text};
        PrCssKeyword countKeyword = cssValueKeyword(&args.values[0]);
        bool automatic = args.values[0].kind == PrCssValueKind_Keyword &&
            (countKeyword == PrCssKeyword_AutoFill || countKeyword == PrCssKeyword_AutoFit);
        if (automatic && list.repeat == PrLayoutGridRepeat_None) {
            list.repeat =
                countKeyword == PrCssKeyword_AutoFill ? PrLayoutGridRepeat_AutoFill : PrLayoutGridRepeat_AutoFit;
            list.repeatStart = r->gridTracks.count;
            trackItemsOf(r, items, list.repeatStart, r->gridRepeatNames);
            list.repeatCount = r->gridTracks.count - list.repeatStart;
            if (list.repeatCount == 0)
                list.repeat = PrLayoutGridRepeat_None;
            continue;
        }
        double count = args.values[0].kind == PrCssValueKind_Integer ? args.values[0].number : 1;
        for (double k = 0; k < count && r->gridTracks.count < kPrLayoutGridMaxLine; ++k)
            trackItemsOf(r, items, 0, r->gridNames);
    }
    list.tracks = r->gridTracks.data;
    list.trackCount = r->gridTracks.count;
    list.names = r->gridNames.data;
    list.nameCount = r->gridNames.count;
    list.repeatNames = r->gridRepeatNames.data;
    list.repeatNameCount = r->gridRepeatNames.count;
    return prLayoutGridTracksIntern(r->tree, &list);
}

// Returns the areas that a computed grid-template-areas names, interned (css-grid-1/#grid-template-areas-property). Its
// strings form a template, which parsing checked (cssGridAreasFold): rows of an equal number of cells, in which each
// name's cells fill a rectangle.
PrLayoutGridAreasIndex areasOf(PrCssPresentation* r, CssValueRun run) {
    if (run.count == 0 || run.values[0].kind != PrCssValueKind_String)
        return kPrLayoutGridAreasNone;
    r->gridAreas.clear();
    uint32_t rows = 0;
    uint32_t columns = 0;
    for (uint32_t i = 0; i < run.count && rows < kPrLayoutGridMaxLine; i += run.values[i].next) {
        if (run.values[i].kind != PrCssValueKind_String)
            continue;
        uint32_t column = 0;
        cssGridAreaCells(cssValueText(run, &run.values[i]), [&](PrStringView text) {
            if (text.size != 0 && column < kPrLayoutGridMaxLine) {
                PrLayoutGridName name = prLayoutGridNameIntern(r->tree, text);
                uint32_t at = 0;
                while (at < r->gridAreas.count && r->gridAreas.data[at].name != name)
                    ++at;
                if (at == r->gridAreas.count)
                    r->gridAreas.push(PrLayoutGridArea{
                        name, uint16_t(rows), uint16_t(rows + 1), uint16_t(column),
                        uint16_t(column + 1),
                    });
                PrLayoutGridArea& area = r->gridAreas.data[at];
                area.rowEnd = uint16_t(rows + 1);
                area.columnEnd = uint16_t(column + 1 > area.columnEnd ? column + 1 : area.columnEnd);
            }
            ++column;
        });
        columns = column < kPrLayoutGridMaxLine ? column : kPrLayoutGridMaxLine;
        ++rows;
    }
    PrLayoutGridAreas areas{
        .rowCount = rows,
        .columnCount = columns,
        .areas = r->gridAreas.data,
        .areaCount = r->gridAreas.count,
    };
    return prLayoutGridAreasIntern(r->tree, &areas);
}

// Returns the value of a grid-placement property that a computed <grid-line> holds (css-grid-1/#line-placement), with
// its integer clamped to the lines that the implicit grid can hold.
PrLayoutGridLine gridLineOf(PrCssPresentation* r, CssValueRun run) {
    PrLayoutGridLine line{};
    bool spans = false;
    double integer = 0;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode& value = run.values[i];
        if (cssValueIsKeyword(&value, PrCssKeyword_Span))
            spans = true;
        else if (value.kind == PrCssValueKind_Integer)
            integer = value.number;
        else if (value.kind == PrCssValueKind_Ident)
            line.name = prLayoutGridNameIntern(r->tree, cssValueText(run, &value));
    }
    double most = double(kPrLayoutGridMaxLine);
    if (spans) {
        line.kind = PrLayoutGridLineKind_Span;
        line.integer = int16_t(integer >= 1 ? (integer < most ? integer : most) : 1);
    } else if (integer != 0 || line.name != kPrLayoutGridNameNone) {
        line.kind = PrLayoutGridLineKind_Line;
        line.integer = int16_t(integer < -most ? -most : integer > most ? most : integer);
    }
    return line;
}

// Returns the grid group that a computed style projects onto (css-grid-1).
PrLayoutGridStyle gridOf(PrCssPresentation* r, const CssComputedStyle* style) {
    PrLayoutGridStyle grid = prLayoutGridStyleDefault();
    grid.templateColumns = trackListOf(r, cssComputedValue(style, PrCssProperty_GridTemplateColumns));
    grid.templateRows = trackListOf(r, cssComputedValue(style, PrCssProperty_GridTemplateRows));
    grid.templateAreas = areasOf(r, cssComputedValue(style, PrCssProperty_GridTemplateAreas));
    // An implicit track size of auto alone is the default list.
    CssValueRun autoColumns = cssComputedValue(style, PrCssProperty_GridAutoColumns);
    CssValueRun autoRows = cssComputedValue(style, PrCssProperty_GridAutoRows);
    if (!(autoColumns.count == 1 && cssValueRunFirstKeyword(autoColumns) == PrCssKeyword_Auto))
        grid.autoColumns = trackListOf(r, autoColumns);
    if (!(autoRows.count == 1 && cssValueRunFirstKeyword(autoRows) == PrCssKeyword_Auto))
        grid.autoRows = trackListOf(r, autoRows);
    grid.columnStart = gridLineOf(r, cssComputedValue(style, PrCssProperty_GridColumnStart));
    grid.columnEnd = gridLineOf(r, cssComputedValue(style, PrCssProperty_GridColumnEnd));
    grid.rowStart = gridLineOf(r, cssComputedValue(style, PrCssProperty_GridRowStart));
    grid.rowEnd = gridLineOf(r, cssComputedValue(style, PrCssProperty_GridRowEnd));
    CssValueRun flow = cssComputedValue(style, PrCssProperty_GridAutoFlow);
    if (hasKeyword(flow, PrCssKeyword_Column))
        grid.autoFlow |= PrLayoutGridAutoFlowFlag_Column;
    if (hasKeyword(flow, PrCssKeyword_Dense))
        grid.autoFlow |= PrLayoutGridAutoFlowFlag_Dense;
    return grid;
}

// Returns the flex group that a computed style projects onto (css-flexbox-2, css-align-3, css-gaps-1).
PrLayoutFlexStyle flexOf(PrCssPresentation* r, const CssComputedStyle* style) {
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    switch (cssComputedFirstKeyword(style, PrCssProperty_FlexDirection)) {
        case PrCssKeyword_RowReverse: flex.direction = PrLayoutFlexDirection_RowReverse; break;
        case PrCssKeyword_Column: flex.direction = PrLayoutFlexDirection_Column; break;
        case PrCssKeyword_ColumnReverse: flex.direction = PrLayoutFlexDirection_ColumnReverse; break;
        default: flex.direction = PrLayoutFlexDirection_Row; break;
    }
    switch (cssComputedFirstKeyword(style, PrCssProperty_FlexWrap)) {
        case PrCssKeyword_Wrap: flex.wrap = PrLayoutFlexWrap_Wrap; break;
        case PrCssKeyword_WrapReverse: flex.wrap = PrLayoutFlexWrap_WrapReverse; break;
        default: flex.wrap = PrLayoutFlexWrap_NoWrap; break;
    }
    flex.grow = factorOf(cssComputedValue(style, PrCssProperty_FlexGrow), 0);
    flex.shrink = factorOf(cssComputedValue(style, PrCssProperty_FlexShrink), 1);
    CssValueRun basis = cssComputedValue(style, PrCssProperty_FlexBasis);
    flex.basis = cssValueRunFirstKeyword(basis) == PrCssKeyword_Content ? prLayoutLengthContent() : lengthOf(r, basis);
    CssValueRun order = cssComputedValue(style, PrCssProperty_Order);
    if (order.count == 1 && order.values[0].kind == PrCssValueKind_Integer)
        flex.order = int32_t(order.values[0].number);
    // A gap of normal is zero in a flex container (css-gaps-1/#valdef-row-gap-normal).
    auto gapOf = [&](CssValueRun run) {
        return cssValueRunFirstKeyword(run) == PrCssKeyword_Normal ? prLayoutLengthNormal() : lengthOf(r, run);
    };
    flex.rowGap = gapOf(cssComputedValue(style, PrCssProperty_RowGap));
    flex.columnGap = gapOf(cssComputedValue(style, PrCssProperty_ColumnGap));
    flex.justifyContent = alignmentOf(cssComputedValue(style, PrCssProperty_JustifyContent), PrLayoutAlignment_Normal);
    flex.alignItems = alignmentOf(cssComputedValue(style, PrCssProperty_AlignItems), PrLayoutAlignment_Normal);
    flex.alignContent = alignmentOf(cssComputedValue(style, PrCssProperty_AlignContent), PrLayoutAlignment_Normal);
    flex.alignSelf = alignmentOf(cssComputedValue(style, PrCssProperty_AlignSelf), PrLayoutAlignment_Auto);
    flex.justifyItems = alignmentOf(cssComputedValue(style, PrCssProperty_JustifyItems), PrLayoutAlignment_Normal);
    flex.justifySelf = alignmentOf(cssComputedValue(style, PrCssProperty_JustifySelf), PrLayoutAlignment_Auto);
    return flex;
}

PrLayoutOverflow overflowOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Hidden:
            return PrLayoutOverflow_Hidden;
        case PrCssKeyword_Clip:
            return PrLayoutOverflow_Clip;
        case PrCssKeyword_Scroll:
        case PrCssKeyword_Auto:
            return PrLayoutOverflow_Scroll;
        default:
            return PrLayoutOverflow_Visible;
    }
}

PrLayoutOverscroll overscrollOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Contain:
            return PrLayoutOverscroll_Contain;
        case PrCssKeyword_None:
            return PrLayoutOverscroll_None;
        case PrCssKeyword_Chain:
            return PrLayoutOverscroll_Chain;
        default:
            return PrLayoutOverscroll_Auto;
    }
}

PrLayoutPosition positionOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Relative: return PrLayoutPosition_Relative;
        case PrCssKeyword_Absolute: return PrLayoutPosition_Absolute;
        case PrCssKeyword_Fixed: return PrLayoutPosition_Fixed;
        case PrCssKeyword_Sticky: return PrLayoutPosition_Sticky;
        default: return PrLayoutPosition_Static;
    }
}

PrLayoutFloat floatOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Left: return PrLayoutFloat_Left;
        case PrCssKeyword_Right: return PrLayoutFloat_Right;
        case PrCssKeyword_InlineStart: return PrLayoutFloat_InlineStart;
        case PrCssKeyword_InlineEnd: return PrLayoutFloat_InlineEnd;
        default: return PrLayoutFloat_None;
    }
}

PrLayoutClear clearOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Left: return PrLayoutClear_Left;
        case PrCssKeyword_Right: return PrLayoutClear_Right;
        case PrCssKeyword_Both: return PrLayoutClear_Both;
        case PrCssKeyword_InlineStart: return PrLayoutClear_InlineStart;
        case PrCssKeyword_InlineEnd: return PrLayoutClear_InlineEnd;
        default: return PrLayoutClear_None;
    }
}

PrDrawBorderStyle borderStyleOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Solid: return PrDrawBorderStyle_Solid;
        case PrCssKeyword_Double: return PrDrawBorderStyle_Double;
        case PrCssKeyword_Dotted: return PrDrawBorderStyle_Dotted;
        case PrCssKeyword_Dashed: return PrDrawBorderStyle_Dashed;
        case PrCssKeyword_Groove: return PrDrawBorderStyle_Groove;
        case PrCssKeyword_Ridge: return PrDrawBorderStyle_Ridge;
        case PrCssKeyword_Inset: return PrDrawBorderStyle_Inset;
        case PrCssKeyword_Outset: return PrDrawBorderStyle_Outset;
        case PrCssKeyword_Hidden: return PrDrawBorderStyle_Hidden;
        default: return PrDrawBorderStyle_None;
    }
}

// The margin and padding properties of each side, by PrRectSide.
constexpr PrCssProperty kMargins[] = {
    PrCssProperty_MarginTop, PrCssProperty_MarginRight, PrCssProperty_MarginBottom,
    PrCssProperty_MarginLeft,
};
constexpr PrCssProperty kPaddings[] = {
    PrCssProperty_PaddingTop, PrCssProperty_PaddingRight,
    PrCssProperty_PaddingBottom, PrCssProperty_PaddingLeft,
};

// The border properties of each side, by PrRectSide.
constexpr PrCssProperty kBorderWidths[] = {
    PrCssProperty_BorderTopWidth, PrCssProperty_BorderRightWidth,
    PrCssProperty_BorderBottomWidth, PrCssProperty_BorderLeftWidth,
};
constexpr PrCssProperty kBorderStyles[] = {
    PrCssProperty_BorderTopStyle, PrCssProperty_BorderRightStyle,
    PrCssProperty_BorderBottomStyle, PrCssProperty_BorderLeftStyle,
};

// Returns true if the border at `side` has a style. Without one, its used width is 0 (css-backgrounds-3/#border-width).
bool borderStyled(const CssComputedStyle* style, uint32_t side) {
    PrCssKeyword border = cssComputedFirstKeyword(style, kBorderStyles[side]);
    return border != PrCssKeyword_None && border != PrCssKeyword_Hidden;
}

// Returns a computed baseline-shift: a keyword, or a raise projected in the same way as a length.
PrLayoutBaselineShift baselineShiftOf(PrCssPresentation* r, CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Sub: return prLayoutBaselineShiftKeyword(PrLayoutKeyword_Sub);
        case PrCssKeyword_Super: return prLayoutBaselineShiftKeyword(PrLayoutKeyword_Super);
        case PrCssKeyword_Top: return prLayoutBaselineShiftKeyword(PrLayoutKeyword_Top);
        case PrCssKeyword_Center: return prLayoutBaselineShiftKeyword(PrLayoutKeyword_Center);
        case PrCssKeyword_Bottom: return prLayoutBaselineShiftKeyword(PrLayoutKeyword_Bottom);
        default: break;
    }
    PrLayoutLength raise = lengthOf(r, run);
    if (prLayoutWordKind(raise.bits) == PrLayoutWordKind_Keyword)
        return prLayoutBaselineShiftPx(0);
    return prLayoutBaselineShiftLength(raise);
}

PrLayoutAlignmentBaseline alignmentBaselineOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_TextBottom: return PrLayoutAlignmentBaseline_TextBottom;
        case PrCssKeyword_Alphabetic: return PrLayoutAlignmentBaseline_Alphabetic;
        case PrCssKeyword_Ideographic: return PrLayoutAlignmentBaseline_Ideographic;
        case PrCssKeyword_Middle: return PrLayoutAlignmentBaseline_Middle;
        case PrCssKeyword_Central: return PrLayoutAlignmentBaseline_Central;
        case PrCssKeyword_Mathematical: return PrLayoutAlignmentBaseline_Mathematical;
        case PrCssKeyword_TextTop: return PrLayoutAlignmentBaseline_TextTop;
        case PrCssKeyword_Hanging: return PrLayoutAlignmentBaseline_Hanging;
        default: return PrLayoutAlignmentBaseline_Baseline;
    }
}

PrLayoutDominantBaseline dominantBaselineOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_TextBottom: return PrLayoutDominantBaseline_TextBottom;
        case PrCssKeyword_Alphabetic: return PrLayoutDominantBaseline_Alphabetic;
        case PrCssKeyword_Ideographic: return PrLayoutDominantBaseline_Ideographic;
        case PrCssKeyword_Middle: return PrLayoutDominantBaseline_Middle;
        case PrCssKeyword_Central: return PrLayoutDominantBaseline_Central;
        case PrCssKeyword_Mathematical: return PrLayoutDominantBaseline_Mathematical;
        case PrCssKeyword_TextTop: return PrLayoutDominantBaseline_TextTop;
        case PrCssKeyword_Hanging: return PrLayoutDominantBaseline_Hanging;
        default: return PrLayoutDominantBaseline_Auto;
    }
}

PrLayoutWritingMode writingModeOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_VerticalRl: return PrLayoutWritingMode_VerticalRl;
        case PrCssKeyword_VerticalLr: return PrLayoutWritingMode_VerticalLr;
        case PrCssKeyword_SidewaysRl: return PrLayoutWritingMode_SidewaysRl;
        case PrCssKeyword_SidewaysLr: return PrLayoutWritingMode_SidewaysLr;
        default: return PrLayoutWritingMode_HorizontalTb;
    }
}

PrLayoutTextOrientation textOrientationOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Upright: return PrLayoutTextOrientation_Upright;
        case PrCssKeyword_Sideways: return PrLayoutTextOrientation_Sideways;
        default: return PrLayoutTextOrientation_Mixed;
    }
}

PrTextBidi unicodeBidiOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Embed: return PrTextBidi_Embed;
        case PrCssKeyword_Isolate: return PrTextBidi_Isolate;
        case PrCssKeyword_BidiOverride: return PrTextBidi_BidiOverride;
        case PrCssKeyword_IsolateOverride: return PrTextBidi_IsolateOverride;
        case PrCssKeyword_Plaintext: return PrTextBidi_Plaintext;
        default: return PrTextBidi_Normal;
    }
}

PrLayoutBaselineSource baselineSourceOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_First: return PrLayoutBaselineSource_First;
        case PrCssKeyword_Last: return PrLayoutBaselineSource_Last;
        default: return PrLayoutBaselineSource_Auto;
    }
}

// Returns a component of a transform origin: a length, a percentage, or a keyword of the axis.
PrLayoutLength originOf(PrCssPresentation* r, CssValueRun run, uint32_t index) {
    const CssValueNode* value = &run.values[index];
    if (value->kind == PrCssValueKind_Keyword) {
        switch (cssValueKeyword(value)) {
            case PrCssKeyword_Left:
            case PrCssKeyword_Top:
                return prLayoutLengthPercent(0);
            case PrCssKeyword_Right:
            case PrCssKeyword_Bottom:
                return prLayoutLengthPercent(100);
            default:
                return prLayoutLengthPercent(50);
        }
    }
    return lengthOf(r, cssValueSubtree(run, index));
}

// Returns the length of a <position> component on one axis: an edge keyword, with the offset at `offset` from that edge
// if `offset` is not kArrayMissing, or a length or percentage measured from the start edge.
PrLayoutLength positionComponentOf(PrCssPresentation* r, CssValueRun run, uint32_t index, uint32_t offset) {
    const CssValueNode* value = &run.values[index];
    if (value->kind != PrCssValueKind_Keyword)
        return lengthOf(r, cssValueSubtree(run, index));
    PrCssKeyword keyword = cssValueKeyword(value);
    bool far = keyword == PrCssKeyword_Right || keyword == PrCssKeyword_Bottom;
    if (keyword == PrCssKeyword_Center)
        return prLayoutLengthPercent(50);
    if (offset == kArrayMissing)
        return prLayoutLengthPercent(far ? 100 : 0);
    if (!far)
        return lengthOf(r, cssValueSubtree(run, offset));
    // An offset from the far edge is 100% minus the offset.
    const CssValueNode* by = &run.values[offset];
    if (by->kind == PrCssValueKind_Percentage)
        return prLayoutLengthPercent(float(100 - by->number));
    r->calc.clear();
    r->calc.push(prLayoutCalcOperation(PrLayoutCalcOp_Sum, 2));
    r->calc.push(prLayoutCalcLeaf(PrLayoutCalcLeaf_Percent, 100));
    r->calc.push(prLayoutCalcOperation(PrLayoutCalcOp_Negate, 1));
    if (!calcNodes(run, offset, r->calc))
        return prLayoutLengthPercent(100);
    return prLayoutLengthIntern(r->tree, r->calc.data, r->calc.count);
}

// Sets `*x` and `*y` to the components of a computed <position>.
void positionOf(PrCssPresentation* r, CssValueRun run, PrLayoutLength* x, PrLayoutLength* y) {
    *x = prLayoutLengthPercent(50);
    *y = prLayoutLengthPercent(50);
    cssPositionAxes(run, [&](bool vertical, uint32_t index, uint32_t offset) {
        *(vertical ? y : x) = positionComponentOf(r, run, index, offset);
    });
}

// Writes the pixels of a computed <position> from the top left of a box of `extent` to `out`.
void positionPx(CssValueRun run, PrVec2 extent, double out[2]) {
    out[0] = double(extent.x) / 2;
    out[1] = double(extent.y) / 2;
    cssPositionAxes(run, [&](bool vertical, uint32_t index, uint32_t offset) {
        double size = double(vertical ? extent.y : extent.x);
        auto lengthAt = [&](uint32_t at) {
            const CssValueNode* value = &run.values[at];
            if (value->kind == PrCssValueKind_Percentage)
                return size * value->number / 100;
            double px = 0;
            cssResolveLength(run, at, size, &px);
            return px;
        };
        const CssValueNode* value = &run.values[index];
        double px = 0;
        if (value->kind != PrCssValueKind_Keyword) {
            px = lengthAt(index);
        } else {
            PrCssKeyword keyword = cssValueKeyword(value);
            bool far = keyword == PrCssKeyword_Right || keyword == PrCssKeyword_Bottom;
            double by = offset != kArrayMissing ? lengthAt(offset) : 0;
            px = keyword == PrCssKeyword_Center ? size / 2 : far ? size - by : by;
        }
        out[vertical ? 1 : 0] = px;
    });
}

// Returns the reference box of a computed transform-box (css-transforms-1/#transform-box).
PrLayoutTransformBox transformBoxOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_ContentBox: return PrLayoutTransformBox_ContentBox;
        case PrCssKeyword_BorderBox: return PrLayoutTransformBox_BorderBox;
        case PrCssKeyword_FillBox: return PrLayoutTransformBox_FillBox;
        case PrCssKeyword_StrokeBox: return PrLayoutTransformBox_StrokeBox;
        default: return PrLayoutTransformBox_ViewBox;
    }
}

// Returns the item of a computed background list property that applies to the bottom layer. The background color paints
// under that layer (css-backgrounds-3/#background-color). There is a layer for each item of background-image, and the
// items of the other properties repeat as needed (css-backgrounds-3/#layering).
CssValueRun bottomLayerItem(const CssComputedStyle* style, PrCssProperty property) {
    CssListItems layers = cssListItems(cssComputedValue(style, PrCssProperty_BackgroundImage));
    CssValueRun run = cssComputedValue(style, property);
    CssListItems items = cssListItems(run);
    if (items.count == 0)
        return run;
    uint32_t bottom = layers.count != 0 ? layers.count - 1 : 0;
    return cssListItem(run, items, bottom % items.count);
}

// Returns the box that the background color paints within: the bottom layer's clip.
PrLayoutBackgroundBox backgroundClipOf(const CssComputedStyle* style) {
    CssValueRun clip = bottomLayerItem(style, PrCssProperty_BackgroundClip);
    if (hasKeyword(clip, PrCssKeyword_ContentBox))
        return PrLayoutBackgroundBox_ContentBox;
    if (hasKeyword(clip, PrCssKeyword_PaddingBox))
        return PrLayoutBackgroundBox_PaddingBox;
    return PrLayoutBackgroundBox_BorderBox;
}

// Returns what the background color is attached to: the bottom layer's attachment.
PrLayoutBackgroundAttachment backgroundAttachmentOf(const CssComputedStyle* style) {
    CssValueRun attachment = bottomLayerItem(style, PrCssProperty_BackgroundAttachment);
    if (hasKeyword(attachment, PrCssKeyword_Local))
        return PrLayoutBackgroundAttachment_Local;
    if (hasKeyword(attachment, PrCssKeyword_Fixed))
        return PrLayoutBackgroundAttachment_Fixed;
    return PrLayoutBackgroundAttachment_Scroll;
}

// Returns how a computed object-fit fits a replaced box's content (css-images-4/#the-object-fit). scale-down combined
// with contain or cover behaves the same as scale-down alone.
PrLayoutObjectFit objectFitOf(CssValueRun run) {
    if (hasKeyword(run, PrCssKeyword_ScaleDown))
        return PrLayoutObjectFit_ScaleDown;
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Contain: return PrLayoutObjectFit_Contain;
        case PrCssKeyword_Cover: return PrLayoutObjectFit_Cover;
        case PrCssKeyword_None: return PrLayoutObjectFit_None;
        default: return PrLayoutObjectFit_Fill;
    }
}

// Sets the slope that a font-style value asks for: normal, italic, or oblique by its angle, which is 14deg if it gives
// none (css-fonts-4/#font-style-prop).
void setSlope(PrFontStyle& style, CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Italic:
            style.slope = PrFontSlope_Italic;
            break;
        case PrCssKeyword_Oblique: {
            double degrees = double(kPrFontSetObliqueAngle);
            if (run.count > 1)
                cssMathTo(run, run.values[0].next, PrCssUnit_Deg, &degrees);
            style.slope = PrFontSlope_Oblique;
            style.obliqueAngle = float(degrees);
            break;
        }
        default:
            style.slope = PrFontSlope_Normal;
            break;
    }
}

// Appends `size` bytes at `bytes` to a text font's key.
void appendKey(Array<char>& key, const void* bytes, size_t size) {
    key.append(static_cast<const char*>(bytes), uint32_t(size));
}

// Returns the text font of `fonts`, in the font set `set`, that a computed style sets its text in. `fonts` makes one
// text font for each family list, style and locale.
PrTextFont* textFontIn(PrFontSet* set, CssPresentationTextFonts& fonts, const CssComputedStyle* style) {
    cssFontFamilies(cssComputedValue(style, PrCssProperty_FontFamily), &fonts.families, &fonts.familyViews);
    PrFontStyle font = prFontStyleDefault();
    font.weight = float(cssStyleFontWeight(style));
    font.width = float(cssStyleFontWidth(style));
    setSlope(font, cssComputedValue(style, PrCssProperty_FontStyle));
    // The size is in whole layout units, which is how layout measures every length.
    font.size = prLayoutUnitToPx(prLayoutUnitFromPxDouble(cssStyleFontSize(style)));
    Array<char>& key = fonts.key;
    key.clear();
    for (uint32_t i = 0; i < fonts.familyViews.count; ++i) {
        const PrFontFamily& family = fonts.familyViews.data[i];
        uint32_t size = uint32_t(family.name.size);
        appendKey(key, &family.generic, sizeof(family.generic));
        appendKey(key, &size, sizeof(size));
        appendKey(key, family.name.data, family.name.size);
    }
    float values[] = {font.weight, font.width, font.obliqueAngle, font.size};
    appendKey(key, values, sizeof(values));
    appendKey(key, &font.slope, sizeof(font.slope));
    appendKey(key, &font.flags, sizeof(font.flags));
    appendKey(key, &style->locale, sizeof(style->locale));
    uint32_t hash = uint32_t(prHashBytes(key.data, key.count, 0x7e3a91c5b2d4f608ull));
    uint32_t found = fonts.index.find(hash, [&](uint32_t at) {
        const CssPresentationTextFont& entry = fonts.fonts.data[at];
        return entry.keyLength == key.count && prMemEquals(fonts.keys.data + entry.keyStart, key.data, key.count);
    });
    if (found != kHashMissing)
        return fonts.fonts.data[found].font;
    PrTextFontDesc desc{
        .set = set,
        .families = fonts.familyViews.data,
        .familyCount = fonts.familyViews.count,
        .style = font,
        .locale = style->locale,
    };
    CssPresentationTextFont entry{fonts.keys.count, key.count, prTextFontCreate(&desc)};
    fonts.keys.append(key.data, key.count);
    fonts.index.insert(hash, fonts.fonts.count);
    fonts.fonts.push(entry);
    return entry.font;
}

// Releases the text fonts and frees the memory.
void textFontsReset(CssPresentationTextFonts& fonts) {
    for (uint32_t i = 0; i < fonts.fonts.count; ++i)
        prTextFontRelease(fonts.fonts.data[i].font);
    fonts.fonts.reset();
    fonts.keys.reset();
    fonts.index.reset();
    fonts.key.reset();
    prTextBufferReset(&fonts.families);
    fonts.familyViews.reset();
}

// Returns the text font, in the document's fonts, that a computed style sets its text in, so that equal fonts are one
// text font and project to one font group.
PrTextFont* textFontOf(PrCssPresentation* r, const CssComputedStyle* style) {
    return textFontIn(r->fonts, r->textFonts, style);
}

// Measures a metric of the font that `style` sets, for the cascade (CssFontMeasure), in the presentation's measuring
// text fonts, brought up to date with the document's fonts. The x-height and cap height are those of the first
// available face, and a face with no cap height measures its ascent (css-values-4/#cap). An advance is that of the
// glyph in the face that sets the character.
bool measureFont(void* PR_NULLABLE data, const CssComputedStyle* style, CssFontMetric metric, bool upright, double* ems) {
    PrCssPresentation* r = static_cast<PrCssPresentation*>(data);
    prMutexLock(&r->measuring);
    r->readsMetrics = true;
    PrTextFont* font = textFontIn(r->fonts, r->measured, style);
    prTextFontUpdate(font);
    bool found = false;
    if (metric == CssFontMetric_Ex || metric == CssFontMetric_Cap) {
        uint32_t first = prTextFontFirst(font);
        if (first != kPrTextMatchNone) {
            PrFontMetrics metrics = prFontMetrics(prTextFontMatchedFont(font, first));
            bool cap = (metrics.fromFont & PrFontMetricFlag_CapHeight) != 0;
            *ems = double(metric == CssFontMetric_Ex ? metrics.xHeight : cap ? metrics.capHeight : metrics.ascent);
            found = true;
        }
    } else {
        float advance = 0;
        PrTextAxis axis = upright ? PrTextAxis_Vertical : PrTextAxis_Horizontal;
        if (metric == CssFontMetric_Ch)
            found = prTextFontAdvance(font, '0', PrScript_Zyyy, axis, &advance);
        else
            found = prTextFontAdvance(font, 0x6c34, PrScript_Hani, axis, &advance);
        *ems = double(advance);
    }
    prMutexUnlock(&r->measuring);
    return found;
}

// What an element's record presents apart from its computed style. This covers what the root and the body give the
// canvas and the viewport, which their own records then leave out.
struct Presentation {
    const CssComputedStyle* PR_NULLABLE background; // style whose background the element takes, or null
    bool dropBackground;                            // the element paints no background, because the canvas paints it
    bool dropOverflow;                              // the element's overflow is the viewport's, and the element clips
                                                    // nothing itself
    const CssComputedStyle* PR_NULLABLE overflow;   // style whose overflow the element takes, or null
    const CssComputedStyle* PR_NULLABLE flow;       // style whose writing-mode and direction the element takes, or null
    const CssComputedStyle* PR_NULLABLE bidi;       // style whose unicode-bidi the element takes, or null
    bool buttonLayout;                              // the element uses button layout (CssBoxKind_Button)
    bool replaced;                                  // the element is replaced (CssBoxKind_Replaced)
    bool outsideMarker;                             // the element is an outside marker (isOutsideMarker)

    // The element is a ::marker, whose text no text-align or text-indent places. Neither applies to a marker box
    // (css-lists-3/#marker-properties), and the engine lays out an outside marker's text as a block container's.
    bool marker;

    // The root is an svg element. Its auto width and height are used as 100% of the viewport
    // (SVG2/geometry.html#Sizing).
    bool fillsViewport;

    // A generated image (generatedImage) that an anonymous replaced box shows as its content, painted as its
    // background, or empty.
    CssValueRun image;

    // The columns and rows that a cell, column or column group spans, if `spans` is set, and the inline size of a text
    // field in characters, or 0, as the element's markup gives them (CssBoxTraits).
    bool spans;
    uint16_t columnSpan;
    uint16_t rowSpan;
    uint16_t characters;

    // The element is a foreignObject, whose box is a root that the content of its svg element places and draws: the box
    // fills its containing block, and its margins, insets, position, float, transform and effects, which SVG applies or
    // which have no effect, apply to nothing (SVG2/embedded.html#Placement).
    bool placed;

    bool waitsForWheel;                             // the element has a wheel listener that is not passive
};

bool neverRendered(const PrDomElement* element);

// Returns true if an element is a foreignObject of SVG content, whose box is a root of the layout tree: the content of
// an svg element draws it, at the place and size that its geometry gives (SVG2/embedded.html#ForeignObjectElement). A
// foreignObject inside an element that is never rendered, such as a pattern or a defs, renders nothing, and has no box
// (SVG2/render.html#Rendered-vs-NonRendered).
bool isForeignRoot(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Svg || element->tag != PrDomTag_ForeignObject)
        return false;
    const PrDomNode* flat = domFlatParent(element);
    const PrDomElement* parent = flat != nullptr ? domElementFromNode(flat) : nullptr;
    if (parent == nullptr || parent->space != PrDomNamespace_Svg || parent->tag == PrDomTag_ForeignObject)
        return false;
    for (const PrDomElement* at = parent; at != nullptr && at->space == PrDomNamespace_Svg;
            at = domElementFromNode(domFlatParent(at))) {
        if (neverRendered(at))
            return false;
        if (domIsOuterSvg(at))
            return true;
    }
    return false;
}

// Returns what the element of `node` gives its box beyond its computed style, or nothing for a node that is not an
// element.
CssBoxTraits traitsOf(const PrDomNode* node) {
    const PrDomElement* element = domElementFromNode(node);
    return element != nullptr ? cssBoxTraits(element) : CssBoxTraits{};
}

// Returns true if an element is replaced: the content of its box is its resource, not the boxes of its children.
bool isReplaced(const PrDomElement* element) {
    return cssBoxTraits(element).kind == CssBoxKind_Replaced;
}

// Returns true if `node` is laid out as a line break, as a br is.
bool isLineBreak(const PrDomNode* node) {
    return traitsOf(node).kind == CssBoxKind_LineBreak;
}

// Returns true if `node` is a pseudo-element that the presentation generates.
bool isPseudo(const PrDomNode* node) {
    return node->type == DomTargetType_PseudoElement;
}

// Returns true if a computed contain or container-type applies inline-size containment
// (css-contain-2/#contain-property, css-conditional-5/#container-type). Size containment contains the inline axis too.
bool containsInline(CssValueRun run) {
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        if (cssValueIsKeyword(&run.values[i], PrCssKeyword_Size) ||
                cssValueIsKeyword(&run.values[i], PrCssKeyword_InlineSize) ||
                cssValueIsKeyword(&run.values[i], PrCssKeyword_Strict))
            return true;
    }
    return false;
}

// Returns the length of a computed contain-intrinsic-width or contain-intrinsic-height, or none
// (css-sizing-4/#intrinsic-size-override). An auto before the length, which asks for the last remembered size, is left
// out.
PrLayoutLength containIntrinsicOf(PrCssPresentation* r, CssValueRun run) {
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        if (!cssValueIsKeyword(value, PrCssKeyword_Auto) && !cssValueIsKeyword(value, PrCssKeyword_None))
            return lengthOf(r, cssValueSubtree(run, i));
    }
    return prLayoutLengthNone();
}

// Returns true if `element` is a one-line text control, which has a part that holds its text (::field-content).
bool isTextField(const PrDomElement* element) {
    return cssBoxTraits(element).kind == CssBoxKind_TextField;
}

// Returns true if `node` is an element whose box layout sizes as a semi-replaced element (html/#button-layout): an
// element that uses button layout, or a text field.
bool isSemiReplaced(const PrDomNode* node) {
    CssBoxKind kind = traitsOf(node).kind;
    return kind == CssBoxKind_Button || kind == CssBoxKind_TextField;
}

// Returns what the element is to the rendered text steps.
PrLayoutRenderedFlags renderedFlagsOf(const PrDomElement* element) {
    PrLayoutRenderedFlags flags = cssBoxIsParagraph(element) ? PrLayoutRenderedFlag_Paragraph : 0;
    if ((cssBoxTraits(element).flags & CssBoxTraitFlag_ChildrenAreValue) != 0)
        flags |= PrLayoutRenderedFlag_ChildrenSkipped;
    return flags;
}

// Returns the presentation of an element that is neither the root nor the body.
Presentation presentationOf(const PrDomNode* node) {
    const PrDomElement* element = domElementFromNode(node);
    CssBoxTraits traits = traitsOf(node);
    return Presentation{
        .buttonLayout = traits.kind == CssBoxKind_Button,
        .replaced = traits.kind == CssBoxKind_Replaced,
        .spans = (traits.flags & CssBoxTraitFlag_Spans) != 0,
        .columnSpan = traits.columnSpan,
        .rowSpan = traits.rowSpan,
        .characters = traits.characters,
        .placed = element != nullptr && isForeignRoot(element),
        .waitsForWheel = element != nullptr && domHasBlockingWheelListener(element),
    };
}

// Makes a box fill its containing block, in place of where its own position, margins and sizes would put it, as a
// foreignObject's box fills the positioning rectangle that SVG places.
void placeInContainingBlock(PrLayoutBoxStyle& box) {
    box.display = PrLayoutDisplay_Block;
    box.position = PrLayoutPosition_Static;
    box.floating = PrLayoutFloat_None;
    box.boxSizing = PrLayoutBoxSizing_BorderBox;
    box.width = prLayoutLengthPercent(100);
    box.height = prLayoutLengthPercent(100);
    box.minWidth = prLayoutLengthPx(0);
    box.minHeight = prLayoutLengthPx(0);
    box.maxWidth = prLayoutLengthNone();
    box.maxHeight = prLayoutLengthNone();
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        box.margin[side] = prLayoutLengthPx(0);
        box.inset[side] = prLayoutLengthAuto();
    }
    box.transformed = false;
}

// Makes the inner display of a box flow-root, as button layout does (html/#button-layout) and as a text control's is
// (html/#form-controls): an inline display behaves as inline-block, and a block display as flow-root. A display of flex
// or grid stays.
void applyFlowRootInside(PrLayoutBoxStyle& box) {
    switch (box.display) {
        case PrLayoutDisplay_Inline: box.display = PrLayoutDisplay_InlineBlock; break;
        case PrLayoutDisplay_Block: box.display = PrLayoutDisplay_FlowRoot; break;
        default: break;
    }
}

// Returns the alignment that a computed text-align-all or text-align-last names. match-parent has already computed to
// another value.
PrLayoutTextAlign textAlignOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_End: return PrLayoutTextAlign_End;
        case PrCssKeyword_Left: return PrLayoutTextAlign_Left;
        case PrCssKeyword_Right: return PrLayoutTextAlign_Right;
        case PrCssKeyword_Center: return PrLayoutTextAlign_Center;
        case PrCssKeyword_Justify: return PrLayoutTextAlign_Justify;
        case PrCssKeyword_Auto: return PrLayoutTextAlign_Auto;
        default: return PrLayoutTextAlign_Start;
    }
}

PrLayoutTextJustify textJustifyOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_None: return PrLayoutTextJustify_None;
        case PrCssKeyword_InterWord: return PrLayoutTextJustify_InterWord;
        case PrCssKeyword_InterCharacter: return PrLayoutTextJustify_InterCharacter;
        default: return PrLayoutTextJustify_Auto;
    }
}

// Sets the font's indent from a computed text-indent: its length or percentage, and its keywords.
void setTextIndent(PrCssPresentation* r, PrLayoutFontStyle& font, CssValueRun run) {
    font.textIndentFlags = 0;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        if (cssValueIsKeyword(value, PrCssKeyword_Hanging))
            font.textIndentFlags |= PrLayoutTextIndentFlag_Hanging;
        else if (cssValueIsKeyword(value, PrCssKeyword_EachLine))
            font.textIndentFlags |= PrLayoutTextIndentFlag_EachLine;
        else
            font.textIndent = lengthOf(r, cssValueSubtree(run, i));
    }
}

// Returns the transform that a computed text-transform names (css-text-4/#text-transform-property): none, math-auto, or
// a case transform with full-width and full-size-kana, in any order.
PrTextTransform textTransformOf(CssValueRun run) {
    uint32_t transform = PrTextTransform_None;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        switch (run.values[i].kind == PrCssValueKind_Keyword ? cssValueKeyword(&run.values[i]) : PrCssKeyword_None) {
            case PrCssKeyword_Capitalize: transform |= PrTextTransform_Capitalize; break;
            case PrCssKeyword_Uppercase: transform |= PrTextTransform_Uppercase; break;
            case PrCssKeyword_Lowercase: transform |= PrTextTransform_Lowercase; break;
            case PrCssKeyword_MathAuto: transform |= PrTextTransform_MathAuto; break;
            case PrCssKeyword_FullWidth: transform |= PrTextTransformFlag_FullWidth; break;
            case PrCssKeyword_FullSizeKana: transform |= PrTextTransformFlag_FullSizeKana; break;
            default: break;
        }
    }
    return PrTextTransform(transform);
}

PrLayoutWhiteSpaceCollapse whiteSpaceCollapseOf(CssValueRun run) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_Discard: return PrLayoutWhiteSpaceCollapse_Discard;
        case PrCssKeyword_Preserve: return PrLayoutWhiteSpaceCollapse_Preserve;
        case PrCssKeyword_PreserveBreaks: return PrLayoutWhiteSpaceCollapse_PreserveBreaks;
        case PrCssKeyword_PreserveSpaces: return PrLayoutWhiteSpaceCollapse_PreserveSpaces;
        case PrCssKeyword_BreakSpaces: return PrLayoutWhiteSpaceCollapse_BreakSpaces;
        default: return PrLayoutWhiteSpaceCollapse_Collapse;
    }
}

// The headroom in stops that `constrained` allows (css-color-hdr-1/#valdef-dynamic-range-limit-constrained), one stop
// past SDR white, which the specification leaves to the engine.
constexpr float kConstrainedHeadroom = 1;

// Returns the headroom in stops that the dynamic-range-limit at `index` of `run` allows on a display of `headroom`
// stops: none for standard, kConstrainedHeadroom for constrained, the display's for no-limit, and for a mix the mean of
// its limits weighted by their percentages. Sets `readsDisplay` if the result depends on the display's headroom.
float rangeLimitOf(CssValueRun run, uint32_t index, float headroom, bool* readsDisplay) {
    if (index >= run.count) {
        *readsDisplay = true;
        return headroom;
    }
    const CssValueNode* value = &run.values[index];
    if (value->kind == PrCssValueKind_Keyword) {
        switch (cssValueKeyword(value)) {
            case PrCssKeyword_Standard: return 0;
            case PrCssKeyword_Constrained: return kConstrainedHeadroom < headroom ? kConstrainedHeadroom : headroom;
            default: *readsDisplay = true; return headroom;
        }
    }
    if (value->kind != PrCssValueKind_Function || cssValueKeyword(value) != PrCssKeyword_DynamicRangeLimitMix) {
        *readsDisplay = true;
        return headroom;
    }
    float sum = 0;
    float weights = 0;
    uint32_t limit = kArrayMissing;
    for (uint32_t at = index + 1; at < index + value->next; at += run.values[at].next) {
        const CssValueNode* item = &run.values[at];
        if (item->kind == PrCssValueKind_Percentage) {
            float weight = float(item->number);
            if (limit != kArrayMissing)
                sum += weight * rangeLimitOf(run, limit, headroom, readsDisplay);
            weights += weight;
            limit = kArrayMissing;
        } else if (item->kind != PrCssValueKind_Literal) {
            limit = at;
        }
    }
    if (!(weights > 0)) {
        *readsDisplay = true;
        return headroom;
    }
    return sum / weights;
}

// Returns the headroom in stops that an element's dynamic-range-limit allows its colors and images, and writes the cap
// on them, as a multiple of SDR white, to `cap`: zero if it allows the display's whole headroom. Sets `readsOutside` if
// a cap depends on the display's headroom.
float rangeCapOf(const PrCssPresentation* r, const CssComputedStyle* style, float* cap, bool* readsOutside) {
    bool readsDisplay = false;
    CssValueRun limit = cssComputedValue(style, PrCssProperty_DynamicRangeLimit);
    float allowed = rangeLimitOf(limit, 0, r->headroom, &readsDisplay);
    *cap = allowed < r->headroom ? prF32Pow(2, allowed) : 0;
    if (*cap > 0 && readsDisplay)
        *readsOutside = true;
    return allowed;
}

// Returns how an element with `style` draws its images. The angle and flip forms of image-orientation, which are
// deprecated and optional (css-images-3/#the-image-orientation), apply the image's orientation as from-image does.
CssPresentationImageDrawing imageDrawingOf(const PrCssPresentation* r, const CssComputedStyle* style) {
    CssPresentationImageDrawing drawing{.rendering = PrLayoutImageRendering_Auto};
    bool reads = false;
    rangeCapOf(r, style, &drawing.limit, &reads);
    switch (cssComputedFirstKeyword(style, PrCssProperty_ImageRendering)) {
        case PrCssKeyword_CrispEdges: drawing.rendering = PrLayoutImageRendering_CrispEdges; break;
        case PrCssKeyword_Pixelated: drawing.rendering = PrLayoutImageRendering_Pixelated; break;
        default: break;
    }
    drawing.oriented = cssComputedFirstKeyword(style, PrCssProperty_ImageOrientation) != PrCssKeyword_None;
    return drawing;
}

// Returns the image that an element shows of `image`, which was arranged from its stored pixels by `orientation`:
// `image` itself if `oriented` is true, and otherwise the stored arrangement, which is made once and kept in `stored`.
const PrDrawImage* orientedImage(const PrDrawImage* image, PrImageOrientation orientation, bool oriented,
        const PrDrawImage* PR_NULLABLE* PR_NONNULL stored) {
    if (oriented || orientation == PrImageOrientation_Identity)
        return image;
    if (*stored == nullptr)
        *stored = prDrawImageCreateOriented(image, prImageOrientationInverse(orientation));
    return *stored;
}

// Returns the natural size `size` of an image that its orientation displays, as an element shows it: unchanged if
// `oriented` is true, and otherwise with its width and height exchanged if the orientation exchanged them.
PrLayoutNaturalSize orientedSize(PrLayoutNaturalSize size, PrImageOrientation orientation, bool oriented) {
    if (oriented || !prImageOrientationSwapsAxes(orientation))
        return size;
    float ratio = size.ratio != 0 ? 1 / size.ratio : 0;
    return PrLayoutNaturalSize{.width = size.height, .height = size.width, .ratio = ratio};
}

// Returns the environment that colors resolve in for a display of `headroom` stops, with the user's preferred scheme
// and the page's supported schemes that the presentation projects for.
CssColorEnvironment colorEnvironmentOf(const PrCssPresentation* r, float headroom, bool* PR_NULLABLE readsOutside) {
    return CssColorEnvironment{
        .headroom = headroom,
        .prefersDark = r->prefersDark,
        .pageSchemes = r->pageSchemes,
        .readsOutside = readsOutside,
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

void mark(PrDomNode* node, DomStyleFlags flags);
void svgNote(PrCssPresentation* r, const PrDomElement* element, CssPresentationSvgNoteKind kind);
const CssPresentationSvg* PR_NULLABLE svgFind(const PrCssPresentation* r, const PrDomElement* element);

// Receives the result of fetching an image that a style names, and decodes it. The boxes whose records named the image
// while it loaded project again at the next update.
void imageFetched(void* PR_NULLABLE data, const DomFetchResult* result) {
    CssPresentationImageFetch* fetch = static_cast<CssPresentationImageFetch*>(data);
    PrCssPresentation* r = fetch->presentation;
    if (r == nullptr)
        return;
    CssPresentationImage& image = r->images.data[fetch->image];
    image.state = CssPresentationImageState_Broken;
    const DomDocumentHost* host = r->document->host;
    DomDecodedImage decoded{};
    PrByteView bytes{reinterpret_cast<const uint8_t*>(result->body.data), result->body.size};
    if (result->status == PrStatus_Ok && host != nullptr && host->decodeImage != nullptr &&
            host->decodeImage(host->context, bytes, domUrlFragment(prStringView(&image.url)), &decoded)) {
        image.state = CssPresentationImageState_Loaded;
        image.image = decoded.image;
        image.orientation = decoded.orientation;
        image.vector = decoded.vector;
        image.natural = decoded.natural;
    }
    for (uint32_t i = 0; i < image.waiting.count; ++i) {
        uint32_t at = cssPresentationNodeEntry(r, image.waiting.data[i]);
        if (at == kHashMissing || r->entries.data[at].node == nullptr)
            continue;
        r->entries.data[at].reprojectPending = true;
        mark(const_cast<PrDomNode*>(image.waiting.data[i]), DomStyleFlag_Self);
    }
    image.waiting.reset();
    r->changed = true;
}

// Forgets a fetch once its result arrived or the browsing context dropped it.
void dropImageFetch(void* PR_NULLABLE data) {
    CssPresentationImageFetch* fetch = static_cast<CssPresentationImageFetch*>(data);
    if (PrCssPresentation* r = fetch->presentation) {
        for (uint32_t i = 0; i < r->imageFetches.count; ++i) {
            if (r->imageFetches.data[i] == fetch) {
                r->imageFetches.erase(i);
                break;
            }
        }
    }
    prMemoryFree(fetch);
}

void imageFetchedAndDropped(void* PR_NULLABLE data, const DomFetchResult* result) {
    imageFetched(data, result);
    dropImageFetch(data);
}

// Writes the URL that a computed <url> names, written as url() with or without quotes, or as src()
// (css-values-4/#urls). Returns false for any other value.
bool urlOf(CssValueRun run, PrStringView* out) {
    const CssValueNode* value = run.values;
    if (value->kind == PrCssValueKind_Url) {
        *out = cssValueText(run, value);
        return true;
    }
    bool function = value->kind == PrCssValueKind_Function &&
        (cssValueKeyword(value) == PrCssKeyword_Url || cssValueKeyword(value) == PrCssKeyword_Src);
    if (!function || value->next < 2 || value[1].kind != PrCssValueKind_String)
        return false;
    *out = cssValueText(run, &value[1]);
    return true;
}

// Returns the image that the url() `reference` names, resolved against the document's base URL, or null if it does not
// parse. Starts the image's fetch the first time a style names it.
CssPresentationImage* PR_NULLABLE imageOf(PrCssPresentation* r, PrStringView reference) {
    PrUrl* base = domDocumentBaseUrl(r->document);
    PrUrl* parsed = nullptr;
    bool ok = prUrlCreate(reference, base, PrCharset_Utf8, &parsed) == PrStatus_Ok;
    prUrlDestroy(base);
    if (!ok) {
        prUrlDestroy(parsed);
        return nullptr;
    }
    PrString url = prUrlSerialize(parsed, PrUrlSerialization_Whole);
    prUrlDestroy(parsed);
    PrStringView view = prStringView(&url);
    uint32_t hash = uint32_t(prHashBytes(view.data, view.size, 0x5be0cd19137e2179ull));
    uint32_t found = r->imageIndex.find(hash, [&](uint32_t at) { return prStringView(&r->images.data[at].url) == view; });
    if (found != kHashMissing) {
        prStringReset(&url);
        return &r->images.data[found];
    }
    uint32_t at = r->images.count;
    r->images.push(CssPresentationImage{.url = url, .state = CssPresentationImageState_Loading});
    r->imageIndex.insert(hash, at);
    const DomDocumentHost* host = r->document->host;
    if (host == nullptr || host->decodeImage == nullptr) {
        r->images.data[at].state = CssPresentationImageState_Broken;
        return &r->images.data[at];
    }
    CssPresentationImageFetch* fetch = allocZeroed<CssPresentationImageFetch>();
    fetch->presentation = r;
    fetch->image = at;
    r->imageFetches.push(fetch);
    // The fetch is of the URL without its fragment, which decoding reads.
    PrStringView fragment = domUrlFragment(view);
    PrStringView request{view.data, fragment.data != nullptr ? size_t(fragment.data - 1 - view.data) : view.size};
    DomFetchRequest asked{request, DomFetchDestination_Image, DomCorsSettings_NoCors, DomFetchInitiator_Css};
    host->fetch(host->context, &asked, imageFetchedAndDropped, fetch, dropImageFetch);
    return &r->images.data[at];
}

// ---------------------------------------------------------------------------------------------------------------------
// Backgrounds

// Returns the run of values from `start` up to `end`.
CssValueRun runBetween(CssValueRun run, uint32_t start, uint32_t end) {
    return CssValueRun{run.values + start, end - start, run.text};
}

// What a projection resolves colors for: the element's color context, and the cap of its dynamic-range-limit, or zero
// for none.
struct ColorTarget {
    CssColorContext context;
    float cap;
};

// Returns the box of a computed <visual-box>.
PrLayoutBackgroundBox backgroundBoxOf(CssValueRun run, PrLayoutBackgroundBox fallback) {
    if (hasKeyword(run, PrCssKeyword_ContentBox))
        return PrLayoutBackgroundBox_ContentBox;
    if (hasKeyword(run, PrCssKeyword_PaddingBox))
        return PrLayoutBackgroundBox_PaddingBox;
    if (hasKeyword(run, PrCssKeyword_BorderBox))
        return PrLayoutBackgroundBox_BorderBox;
    return fallback;
}

PrLayoutBackgroundRepeat repeatOf(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_Space: return PrLayoutBackgroundRepeat_Space;
        case PrCssKeyword_Round: return PrLayoutBackgroundRepeat_Round;
        case PrCssKeyword_NoRepeat: return PrLayoutBackgroundRepeat_NoRepeat;
        default: return PrLayoutBackgroundRepeat_Repeat;
    }
}

// Sets how a layer repeats from a computed <repeat-style> (css-backgrounds-3/#background-repeat).
void repeatStyleOf(CssValueRun run, PrLayoutBackgroundLayer& layer) {
    PrCssKeyword first = cssValueRunFirstKeyword(run);
    if (first == PrCssKeyword_RepeatX) {
        layer.repeatX = PrLayoutBackgroundRepeat_Repeat;
        layer.repeatY = PrLayoutBackgroundRepeat_NoRepeat;
    } else if (first == PrCssKeyword_RepeatY) {
        layer.repeatX = PrLayoutBackgroundRepeat_NoRepeat;
        layer.repeatY = PrLayoutBackgroundRepeat_Repeat;
    } else if (run.count != 0) {
        layer.repeatX = repeatOf(first);
        layer.repeatY =
            run.values[0].next < run.count ? repeatOf(cssValueKeyword(&run.values[run.values[0].next])) : layer.repeatX;
    }
}

// Sets how a layer is sized from a computed <bg-size> (css-backgrounds-3/#background-size).
void sizeOf(PrCssPresentation* r, CssValueRun run, PrLayoutBackgroundLayer& layer) {
    PrCssKeyword keyword = cssValueRunFirstKeyword(run);
    if (keyword == PrCssKeyword_Cover || keyword == PrCssKeyword_Contain) {
        layer.size = keyword == PrCssKeyword_Cover ? PrLayoutBackgroundSize_Cover : PrLayoutBackgroundSize_Contain;
        return;
    }
    if (run.count == 0)
        return;
    uint32_t second = run.values[0].next;
    layer.sizeX = lengthOf(r, cssValueSubtree(run, 0));
    layer.sizeY = second < run.count ? lengthOf(r, cssValueSubtree(run, second)) : prLayoutLengthAuto();
}

// Returns true if a computed background-image has no images: none in every layer.
bool hasNoImages(CssValueRun run) {
    CssListItems items = cssListItems(run);
    for (uint32_t i = 0; i < items.count; ++i) {
        if (cssValueRunFirstKeyword(cssListItem(run, items, i)) != PrCssKeyword_None)
            return false;
    }
    return true;
}

// Returns the image of a background-image item. For light-dark(), it returns the image or none that the element's used
// color scheme picks (css-color-5/#light-dark), and the choice may read the user's preference. Any other item is
// returned as it is.
CssValueRun schemeImageOf(CssValueRun image, const ColorTarget& target) {
    if (image.count == 0 || image.values[0].kind != PrCssValueKind_Function ||
            cssValueKeyword(image.values) != PrCssKeyword_LightDark)
        return image;
    uint32_t end = image.values[0].next;
    uint32_t comma = 1;
    while (comma < end && !cssValueIsLiteral(&image.values[comma], ','))
        comma += image.values[comma].next;
    if (cssUsesDarkScheme(target.context.style, target.context.environment))
        return runBetween(image, comma + 1, end);
    return runBetween(image, 1, comma);
}

// Returns a length of a computed value as the presentation's layout tree holds it. The tree interns a compound
// calculation.
PrLayoutLength treeLength(void* PR_NULLABLE context, CssValueRun run) {
    return lengthOf(static_cast<PrCssPresentation*>(context), run);
}

// Returns an axis of a computed <position> as the presentation's layout tree holds it.
PrLayoutLength treePosition(void* PR_NULLABLE context, CssValueRun run, uint32_t index, uint32_t offset) {
    return positionComponentOf(static_cast<PrCssPresentation*>(context), run, index, offset);
}

// Writes the gradient of the computed <gradient> at `index` to `out`, with its stops appended to `stops`, for the
// presentation's layout tree.
void gradientOf(PrCssPresentation* r, CssValueRun run, uint32_t index, const ColorTarget& target,
        Array<PrLayoutColorStop>& stops, PrLayoutGradient* out) {
    cssGradientFromValue(run, index, target.context, target.cap, CssLayoutLengths{treeLength, treePosition, r}, stops, out);
}

bool imageLayerOf(PrCssPresentation* r, CssValueRun image, const ColorTarget& target,
    CssPresentationImageDrawing drawing, PrLayoutBackgroundLayer& layer);
bool filterItemsOf(PrCssPresentation* r, CssValueRun run, const CssColorContext& colors, Array<PrLayoutFilter>& out,
    Array<PrLayoutSource>& sources);
void releaseSource(const PrLayoutSource& source);

// Returns where the gradient stops and filter items of the image that is resolved next start in the presentation's
// scratch arrays.
CssPresentationImageSpans imageSpansOf(const PrCssPresentation* r) {
    return CssPresentationImageSpans{.stops = r->backgroundStops.count, .filters = r->imageFilters.count};
}

// Points the gradient stops and the filter items of `layer` into the presentation's scratch arrays, where `spans` says
// they start.
void pointImage(PrCssPresentation* r, PrLayoutBackgroundLayer& layer, CssPresentationImageSpans spans) {
    if (layer.kind == PrLayoutImageKind_Gradient)
        layer.gradient.stops = r->backgroundStops.data + spans.stops;
    layer.filters = layer.filterCount != 0 ? r->imageFilters.data + spans.filters : nullptr;
}

// Empties the presentation's scratch arrays of images: gradient stops, filter items and the sources that they name.
void clearImages(PrCssPresentation* r) {
    r->backgroundStops.clear();
    r->imageFilters.clear();
    PR_ASSERT(r->imageSources.count == 0);
}

// Releases the presentation's references to the sources that the filter items of images name, once the tree holds its
// own.
void releaseImageSources(PrCssPresentation* r) {
    for (uint32_t i = 0; i < r->imageSources.count; ++i)
        releaseSource(r->imageSources.data[i]);
    r->imageSources.clear();
}

// Sets the image of `layer` from the image that the url() `reference` names. Returns false if the image has not loaded
// or failed to load. A box that names an image that is loading projects again when it loads.
bool loadedImageOf(PrCssPresentation* r, PrStringView reference, CssPresentationImageDrawing drawing,
        PrLayoutBackgroundLayer& layer) {
    CssPresentationImage* loaded = imageOf(r, reference);
    if (loaded != nullptr && loaded->state == CssPresentationImageState_Loading && r->projecting != nullptr)
        loaded->waiting.push(r->projecting);
    if (loaded == nullptr || loaded->state != CssPresentationImageState_Loaded)
        return false;
    layer.natural = orientedSize(loaded->natural, loaded->orientation, drawing.oriented);
    layer.rendering = drawing.rendering;
    if (loaded->image != nullptr) {
        layer.kind = PrLayoutImageKind_Image;
        layer.image = orientedImage(loaded->image, loaded->orientation, drawing.oriented, &loaded->stored);
    } else {
        layer.kind = PrLayoutImageKind_Painter;
        layer.painter = loaded->vector;
    }
    return true;
}

// Returns true if `image` is a computed filter() (filter-effects-1/#FilterCSSImageValue).
bool isFilterImage(CssValueRun image) {
    return image.count != 0 && cssValueIsFunction(image.values, PrCssKeyword_Filter);
}

// Returns the position of the comma that ends the input of the computed filter() `image`.
uint32_t filterInputEnd(CssValueRun image) {
    uint32_t end = image.values[0].next;
    uint32_t comma = 1;
    while (comma < end && !cssValueIsLiteral(&image.values[comma], ','))
        comma += image.values[comma].next;
    return comma;
}

// Sets the image of `layer` from the computed filter() `image`: its input, an image or a string that is the URL of one,
// with the items of its filter list appended to the input's. A URL in the list that names no filter element leaves the
// list filtering nothing, as it does in the filter property (filter-effects-1/#FilterProperty). Returns false if the
// input cannot be displayed.
bool filteredImageOf(PrCssPresentation* r, CssValueRun image, const ColorTarget& target,
        CssPresentationImageDrawing drawing, PrLayoutBackgroundLayer& layer) {
    uint32_t comma = filterInputEnd(image);
    CssValueRun input = runBetween(image, 1, comma);
    bool shown;
    if (input.count != 0 && input.values[0].kind == PrCssValueKind_String)
        shown = loadedImageOf(r, cssValueText(input, input.values), drawing, layer);
    else
        shown = imageLayerOf(r, input, target, drawing, layer);
    if (!shown)
        return false;
    uint32_t mark = r->imageFilters.count;
    if (filterItemsOf(r, runBetween(image, comma + 1, image.values[0].next), target.context, r->imageFilters,
            r->imageSources))
        layer.filterCount += r->imageFilters.count - mark;
    return true;
}

// Sets the image of `layer` from a computed <image>: the URL of an image, a gradient, or filter() of one. Appends a
// gradient's stops and a filter list's items to the presentation's scratch arrays, which pointImage points the layer
// at, and the sources that the items name to `imageSources`, which releaseImageSources releases. Returns false if the
// image cannot be displayed, now or ever: an image that has not loaded, that failed to load, or of a kind that the
// engine does not draw. When the image loads, the box projects again.
bool imageLayerOf(PrCssPresentation* r, CssValueRun image, const ColorTarget& target,
        CssPresentationImageDrawing drawing, PrLayoutBackgroundLayer& layer) {
    PrStringView reference{};
    if (image.count != 0 && urlOf(image, &reference))
        return loadedImageOf(r, reference, drawing, layer);
    if (cssIsGradient(image)) {
        layer.kind = PrLayoutImageKind_Gradient;
        gradientOf(r, image, 0, target, r->backgroundStops, &layer.gradient);
        return true;
    }
    if (isFilterImage(image))
        return filteredImageOf(r, image, target, drawing, layer);
    return false;
}

// Returns true if a computed <image> is drawn by the engine rather than decoded: a gradient, or filter().
bool generatedImage(CssValueRun image) {
    return cssIsGradient(image) || isFilterImage(image);
}

// Returns true if a computed <image> shows, or may once it loads: a gradient, an image that has not failed to load, or
// filter() of one.
bool imageShows(PrCssPresentation* r, CssValueRun image) {
    PrStringView reference{};
    if (urlOf(image, &reference)) {
        CssPresentationImage* loaded = imageOf(r, reference);
        return loaded != nullptr && loaded->state != CssPresentationImageState_Broken;
    }
    if (isFilterImage(image)) {
        CssValueRun input = runBetween(image, 1, filterInputEnd(image));
        if (input.count != 0 && input.values[0].kind == PrCssValueKind_String) {
            CssPresentationImage* loaded = imageOf(r, cssValueText(input, input.values));
            return loaded != nullptr && loaded->state != CssPresentationImageState_Broken;
        }
        return imageShows(r, input);
    }
    return cssIsGradient(image);
}

// Returns the natural dimensions of a generated image: none for a gradient, and its input's for filter()
// (filter-effects-1/#FilterCSSImageValue).
PrLayoutNaturalSize generatedNaturalOf(PrCssPresentation* r, CssValueRun image, CssPresentationImageDrawing drawing) {
    if (!isFilterImage(image))
        return prLayoutNaturalSizeNone();
    CssValueRun input = runBetween(image, 1, filterInputEnd(image));
    PrStringView reference{};
    bool string = input.count != 0 && input.values[0].kind == PrCssValueKind_String;
    if (string)
        reference = cssValueText(input, input.values);
    if (!string && !urlOf(input, &reference))
        return generatedNaturalOf(r, input, drawing);
    CssPresentationImage* loaded = imageOf(r, reference);
    if (loaded == nullptr || loaded->state != CssPresentationImageState_Loaded)
        return prLayoutNaturalSizeNone();
    return orientedSize(loaded->natural, loaded->orientation, drawing.oriented);
}

// Returns the background of an anonymous box that shows the generated image `image` as its content: one layer, the
// image stretched over its padding box, which is its content box.
PrLayoutBackgroundIndex generatedBackgroundOf(PrCssPresentation* r, CssValueRun image, const ColorTarget& target,
        CssPresentationImageDrawing drawing) {
    PrLayoutBackgroundLayer layer = prLayoutBackgroundLayerDefault();
    layer.sizeX = prLayoutLengthPercent(100);
    layer.sizeY = prLayoutLengthPercent(100);
    layer.repeatX = PrLayoutBackgroundRepeat_NoRepeat;
    layer.repeatY = PrLayoutBackgroundRepeat_NoRepeat;
    clearImages(r);
    if (!imageLayerOf(r, image, target, drawing, layer)) {
        releaseImageSources(r);
        return kPrLayoutBackgroundNone;
    }
    layer.limit = target.cap;
    pointImage(r, layer, CssPresentationImageSpans{});
    PrLayoutBackgroundIndex index = prLayoutBackgroundIntern(r->tree, &layer, 1);
    releaseImageSources(r);
    return index;
}

// Sets how a layer composites from a computed <'mix-blend-mode'> (compositing-2/#mix-blend-mode): a blend mode, or
// plus-lighter, which is an operator (compositing-2/#porterduffcompositingoperators_plus_lighter).
void mixBlendOf(PrCssKeyword keyword, PrDrawBlendMode* blendMode, PrDrawComposite* composite) {
    if (keyword == PrCssKeyword_PlusLighter)
        *composite = PrDrawComposite_PlusLighter;
    else
        cssBlendModeFromKeyword(keyword, blendMode);
}

// Returns the background layers of a computed style, interned by the tree, or kPrLayoutBackgroundNone if every layer is
// none. Each layer takes its image's item of background-image and the same item of each other background list property,
// whose items repeat as needed (css-backgrounds-3/#layering). A layer whose image cannot be displayed draws nothing.
PrLayoutBackgroundIndex backgroundLayersOf(PrCssPresentation* r, const CssComputedStyle* style, const ColorTarget& target) {
    CssValueRun images = cssComputedValue(style, PrCssProperty_BackgroundImage);
    if (hasNoImages(images))
        return kPrLayoutBackgroundNone;
    CssValueRun positions = cssComputedValue(style, PrCssProperty_BackgroundPosition);
    CssValueRun sizes = cssComputedValue(style, PrCssProperty_BackgroundSize);
    CssValueRun repeats = cssComputedValue(style, PrCssProperty_BackgroundRepeat);
    CssValueRun attachments = cssComputedValue(style, PrCssProperty_BackgroundAttachment);
    CssValueRun origins = cssComputedValue(style, PrCssProperty_BackgroundOrigin);
    CssValueRun clips = cssComputedValue(style, PrCssProperty_BackgroundClip);
    CssValueRun blends = cssComputedValue(style, PrCssProperty_BackgroundBlendMode);
    CssListItems imageItems = cssListItems(images);
    CssListItems positionItems = cssListItems(positions);
    CssListItems sizeItems = cssListItems(sizes);
    CssListItems repeatItems = cssListItems(repeats);
    CssListItems attachmentItems = cssListItems(attachments);
    CssListItems originItems = cssListItems(origins);
    CssListItems clipItems = cssListItems(clips);
    CssListItems blendItems = cssListItems(blends);
    CssPresentationImageDrawing drawing = imageDrawingOf(r, style);
    r->backgroundLayers.clear();
    clearImages(r);
    Array<CssPresentationImageSpans>& spans = r->backgroundSpans;
    spans.clear();
    for (uint32_t i = 0; i < imageItems.count; ++i) {
        PrLayoutBackgroundLayer layer = prLayoutBackgroundLayerDefault();
        CssValueRun image = schemeImageOf(cssListItem(images, imageItems, i), target);
        spans.push(imageSpansOf(r));
        if (cssValueRunFirstKeyword(image) != PrCssKeyword_None && imageLayerOf(r, image, target, drawing, layer))
            layer.limit = target.cap;
        if (positionItems.count != 0) {
            layer.positionX = prLayoutLengthPercent(0);
            layer.positionY = prLayoutLengthPercent(0);
            positionOf(r, cssListItem(positions, positionItems, i), &layer.positionX, &layer.positionY);
        }
        if (sizeItems.count != 0)
            sizeOf(r, cssListItem(sizes, sizeItems, i), layer);
        if (repeatItems.count != 0)
            repeatStyleOf(cssListItem(repeats, repeatItems, i), layer);
        if (attachmentItems.count != 0) {
            PrCssKeyword attachment = cssValueRunFirstKeyword(cssListItem(attachments, attachmentItems, i));
            if (attachment == PrCssKeyword_Fixed)
                layer.attachment = PrLayoutBackgroundAttachment_Fixed;
            else if (attachment == PrCssKeyword_Local)
                layer.attachment = PrLayoutBackgroundAttachment_Local;
            else
                layer.attachment = PrLayoutBackgroundAttachment_Scroll;
        }
        if (originItems.count != 0)
            layer.origin = backgroundBoxOf(cssListItem(origins, originItems, i), PrLayoutBackgroundBox_PaddingBox);
        if (clipItems.count != 0)
            layer.clip = backgroundBoxOf(cssListItem(clips, clipItems, i), PrLayoutBackgroundBox_BorderBox);
        if (blendItems.count != 0)
            mixBlendOf(cssValueRunFirstKeyword(cssListItem(blends, blendItems, i)), &layer.blendMode, &layer.composite);
        r->backgroundLayers.push(layer);
    }
    for (uint32_t i = 0; i < r->backgroundLayers.count; ++i)
        pointImage(r, r->backgroundLayers.data[i], spans.data[i]);
    PrLayoutBackgroundIndex index =
        prLayoutBackgroundIntern(r->tree, r->backgroundLayers.data, r->backgroundLayers.count);
    releaseImageSources(r);
    return index;
}

// ---------------------------------------------------------------------------------------------------------------------
// Compositing

// Notes that the node being projected names a source of the references, so that it takes its sources again when what
// they draw may have changed.
void noteReferencing(PrCssPresentation* r) {
    const PrDomNode* node = r->projecting;
    if (node == nullptr)
        return;
    for (uint32_t i = 0; i < r->referencing.count; ++i) {
        if (r->referencing.data[i] == node)
            return;
    }
    r->referencing.push(node);
}

// Writes to `out` the source of the element of `kind` that `url` names in a property of the node being projected. The
// caller owns a reference to the source. Returns false if `url` names no such element. An element of an external
// document counts as none.
bool referenceOf(PrCssPresentation* r, PrStringView url, CssPresentationReferenceKind kind, PrLayoutSource* out) {
    *out = PrLayoutSource{};
    const PrDomNode* node = r->projecting;
    // A pseudo-element's URLs are its originating element's.
    while (node != nullptr && isPseudo(node))
        node = node->parent;
    const PrDomElement* element = node != nullptr ? domElementFromNode(node) : nullptr;
    if (element == nullptr || r->references == nullptr)
        return false;
    noteReferencing(r);
    return r->references->reference(r->references->context, element, url, kind, out);
}

// Releases the caller's reference to a source that referenceOf wrote, once the tree holds its own.
void releaseSource(const PrLayoutSource& source) {
    if (source.functions != nullptr && source.functions->release != nullptr)
        source.functions->release(source.context);
}

// Appends to `out` the items of a computed <filter-value-list>, `run`, with drop-shadow()'s color resolved in `colors`
// and each URL resolved to the filter element that it names, and appends each source to `sources`. Returns false, and
// appends nothing, if a URL names no filter element.
bool filterItemsOf(PrCssPresentation* r, CssValueRun run, const CssColorContext& colors, Array<PrLayoutFilter>& out,
        Array<PrLayoutSource>& sources) {
    Array<CssFilterFunction> functions;
    cssFilterFunctions(run, colors, &functions);
    uint32_t mark = out.count;
    uint32_t sourceMark = sources.count;
    bool found = true;
    for (uint32_t i = 0; i < functions.count && found; ++i) {
        PrLayoutFilter filter = functions.data[i].filter;
        if (filter.kind == PrLayoutFilterKind_Source) {
            found = referenceOf(r, functions.data[i].url, CssPresentationReferenceKind_Filter, &filter.source);
            if (!found)
                break;
            sources.push(filter.source);
        }
        out.push(filter);
    }
    functions.reset();
    if (!found) {
        while (sources.count > sourceMark) {
            releaseSource(sources.last());
            sources.pop();
        }
        out.resize(mark);
    }
    return found;
}

PrLayoutLength shapeLength(void* PR_NULLABLE context, CssValueRun run, CssShapeAxis) {
    return lengthOf(static_cast<PrCssPresentation*>(context), run);
}

PrLayoutLength shapePosition(void* PR_NULLABLE context, CssValueRun run, uint32_t index, uint32_t offset, CssShapeAxis) {
    return positionComponentOf(static_cast<PrCssPresentation*>(context), run, index, offset);
}

// Returns the box that a <geometry-box> keyword names on a box with CSS layout (css-masking-1/#the-clip-path): fill-box
// is the content box, and stroke-box and view-box are the border box.
PrLayoutGeometryBox geometryBoxOf(PrCssKeyword keyword, PrLayoutGeometryBox fallback) {
    switch (keyword) {
        case PrCssKeyword_ContentBox:
        case PrCssKeyword_FillBox: return PrLayoutGeometryBox_ContentBox;
        case PrCssKeyword_PaddingBox: return PrLayoutGeometryBox_PaddingBox;
        case PrCssKeyword_MarginBox: return PrLayoutGeometryBox_MarginBox;
        case PrCssKeyword_BorderBox:
        case PrCssKeyword_StrokeBox:
        case PrCssKeyword_ViewBox: return PrLayoutGeometryBox_BorderBox;
        default: return fallback;
    }
}

// Returns the first <geometry-box> keyword among the top-level values of `run`, or PrCssKeyword_Count.
PrCssKeyword geometryKeywordOf(CssValueRun run) {
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode* value = &run.values[i];
        if (value->kind != PrCssValueKind_Keyword)
            continue;
        switch (cssValueKeyword(value)) {
            case PrCssKeyword_ContentBox:
            case PrCssKeyword_PaddingBox:
            case PrCssKeyword_BorderBox:
            case PrCssKeyword_MarginBox:
            case PrCssKeyword_FillBox:
            case PrCssKeyword_StrokeBox:
            case PrCssKeyword_ViewBox: return cssValueKeyword(value);
            default: break;
        }
    }
    return PrCssKeyword_Count;
}

// Sets the clip path of `c` from a computed clip-path (css-masking-1/#the-clip-path). A URL that names no clipPath, and
// a shape that the engine does not draw, clip nothing. Like any value but none, they still make the box a group.
void clipPathOf(PrCssPresentation* r, CssValueRun clip, PrLayoutCompositingStyle* c, Array<PrLayoutSource>& sources) {
    if (clip.count == 0 || cssValueRunFirstKeyword(clip) == PrCssKeyword_None)
        return;
    PrStringView url{};
    if (urlOf(clip, &url)) {
        c->clipKind = PrLayoutClipKind_Source;
        if (referenceOf(r, url, CssPresentationReferenceKind_ClipPath, &c->clipSource))
            sources.push(c->clipSource);
        return;
    }
    PrCssKeyword box = geometryKeywordOf(clip);
    c->clipKind = PrLayoutClipKind_Box;
    c->clipBox = geometryBoxOf(box, PrLayoutGeometryBox_BorderBox);
    for (uint32_t i = 0; i < clip.count; i += clip.values[i].next) {
        if (!cssIsBasicShape(clip, i))
            continue;
        r->shapePoints.clear();
        PrPath* path = prPathCreate();
        PrLayoutShape shape{};
        c->clipKind = PrLayoutClipKind_Source;
        if (cssShapeFromValue(clip, i, CssShapeLengths{shapeLength, shapePosition, r}, r->shapePoints, path, &shape)) {
            shape.points = r->shapePoints.data;
            shape.path = shape.kind == PrLayoutShapeKind_Path ? path : nullptr;
            c->clipKind = PrLayoutClipKind_Shape;
            c->clipShape = prLayoutShapeIntern(r->tree, &shape);
        }
        prPathDestroy(path);
        break;
    }
}

// Sets the corners of `decoration` from the computed border radii (css-backgrounds-3/#border-radius), each a horizontal
// radius and an optional vertical one, and the computed corner shapes (css-borders-4/#corner-shape).
void cornersOf(PrCssPresentation* r, const CssComputedStyle* style, PrLayoutDecorationStyle* decoration) {
    static const PrCssProperty kRadii[PrRectCorner_Count] = {
        PrCssProperty_BorderTopLeftRadius, PrCssProperty_BorderTopRightRadius, PrCssProperty_BorderBottomRightRadius,
        PrCssProperty_BorderBottomLeftRadius,
    };
    static const PrCssProperty kShapes[PrRectCorner_Count] = {
        PrCssProperty_CornerTopLeftShape, PrCssProperty_CornerTopRightShape, PrCssProperty_CornerBottomRightShape,
        PrCssProperty_CornerBottomLeftShape,
    };
    bool rounded = false;
    for (uint32_t corner = 0; corner < PrRectCorner_Count; ++corner) {
        CssValueRun run = cssComputedValue(style, kRadii[corner]);
        if (run.count == 0)
            continue;
        uint32_t second = run.values[0].next;
        decoration->radii[2 * corner] = lengthOf(r, cssValueSubtree(run, 0));
        decoration->radii[2 * corner + 1] =
            second < run.count ? lengthOf(r, cssValueSubtree(run, second)) : decoration->radii[2 * corner];
        rounded = rounded || decoration->radii[2 * corner].bits != prLayoutLengthPx(0).bits ||
            decoration->radii[2 * corner + 1].bits != prLayoutLengthPx(0).bits;
    }
    // A shape matters only at a corner with a radius, so boxes with square corners share a group whatever their shapes.
    for (uint32_t corner = 0; rounded && corner < PrRectCorner_Count; ++corner)
        decoration->cornerShapes[corner] = float(cssCornerShapeParameter(cssComputedValue(style, kShapes[corner])));
}

// Sets the rectangle of `c` from a computed clip (css-masking-1/#clip-property): rect() with an edge or auto for each
// side, top, right, bottom and left.
void clipRectOf(PrCssPresentation* r, CssValueRun clip, PrLayoutCompositingStyle* c) {
    if (clip.count == 0 || clip.values[0].kind != PrCssValueKind_Function)
        return;
    uint32_t side = 0;
    for (uint32_t i = 1; i < clip.count && side < PrRectSide_Count; i += clip.values[i].next) {
        if (cssValueIsLiteral(&clip.values[i], ','))
            continue;
        c->clip[side++] = lengthOf(r, cssValueSubtree(clip, i));
    }
    c->clips = side == PrRectSide_Count;
}

// Returns the box that a keyword of `property`, mask-origin or mask-clip, names on a box with CSS layout. fill-box is
// the content box for mask-clip (css-masking-1/#the-mask-clip), and the initial value, the border box, for mask-origin
// (css-masking-1/#the-mask-origin). stroke-box and view-box are the border box for both.
PrLayoutBackgroundBox maskBoxOf(CssValueRun run, PrCssProperty property) {
    switch (cssValueRunFirstKeyword(run)) {
        case PrCssKeyword_ContentBox: return PrLayoutBackgroundBox_ContentBox;
        case PrCssKeyword_FillBox:
            if (property == PrCssProperty_MaskClip)
                return PrLayoutBackgroundBox_ContentBox;
            return PrLayoutBackgroundBox_BorderBox;
        case PrCssKeyword_PaddingBox: return PrLayoutBackgroundBox_PaddingBox;
        default: return PrLayoutBackgroundBox_BorderBox;
    }
}

// Sets the sides of a mask border's width or outset from a computed value of one to four sides, each a length, a
// percentage, a number or auto (css-backgrounds-3/#border-image-width).
void borderSidesOf(PrCssPresentation* r, CssValueRun run, PrLayoutBorderImageKind* kinds, PrLayoutLength* lengths,
        float* numbers) {
    uint32_t items[4] = {};
    uint32_t count = 0;
    for (uint32_t i = 0; i < run.count && count < 4; i += run.values[i].next)
        items[count++] = i;
    if (count == 0)
        return;
    if (count < 2)
        items[1] = items[0];
    if (count < 3)
        items[2] = items[0];
    if (count < 4)
        items[3] = items[1];
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        const CssValueNode* value = &run.values[items[side]];
        if (value->kind == PrCssValueKind_Keyword) {
            kinds[side] = PrLayoutBorderImageKind_Auto;
        } else if (value->kind == PrCssValueKind_Number || value->kind == PrCssValueKind_Integer) {
            kinds[side] = PrLayoutBorderImageKind_Number;
            numbers[side] = float(value->number);
        } else {
            kinds[side] = PrLayoutBorderImageKind_Length;
            lengths[side] = lengthOf(r, cssValueSubtree(run, items[side]));
        }
    }
}

PrDrawSliceMode sliceModeOf(PrCssKeyword keyword) {
    switch (keyword) {
        case PrCssKeyword_Repeat: return PrDrawSliceMode_Repeat;
        case PrCssKeyword_Round: return PrDrawSliceMode_Round;
        case PrCssKeyword_Space: return PrDrawSliceMode_Space;
        default: return PrDrawSliceMode_Stretch;
    }
}

// Sets `border` from the computed mask border properties (css-masking-1/#mask-borders). A border whose image cannot be
// displayed masks nothing.
void maskBorderOf(PrCssPresentation* r, const CssComputedStyle* style, const ColorTarget& target,
        CssPresentationImageDrawing drawing, PrLayoutMaskBorder* border) {
    CssValueRun source = cssComputedValue(style, PrCssProperty_MaskBorderSource);
    if (source.count == 0 || cssValueRunFirstKeyword(source) == PrCssKeyword_None)
        return;
    if (!imageLayerOf(r, source, target, drawing, border->image)) {
        border->image.kind = PrLayoutImageKind_None;
        border->image.filterCount = 0;
        return;
    }
    CssValueRun slice = cssComputedValue(style, PrCssProperty_MaskBorderSlice);
    uint32_t items[4] = {};
    uint32_t count = 0;
    for (uint32_t i = 0; i < slice.count; i += slice.values[i].next) {
        if (cssValueIsKeyword(&slice.values[i], PrCssKeyword_Fill))
            border->fill = true;
        else if (count < 4)
            items[count++] = i;
    }
    if (count != 0) {
        if (count < 2)
            items[1] = items[0];
        if (count < 3)
            items[2] = items[0];
        if (count < 4)
            items[3] = items[1];
        // A number is pixels of the image, and a percentage is of its size.
        for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
            const CssValueNode* value = &slice.values[items[side]];
            if (value->kind == PrCssValueKind_Percentage)
                border->slice[side] = prLayoutLengthPercent(float(value->number));
            else
                border->slice[side] = prLayoutLengthPx(float(value->number));
        }
    }
    borderSidesOf(r, cssComputedValue(style, PrCssProperty_MaskBorderWidth), border->widthKind, border->width,
        border->widthNumber);
    borderSidesOf(r, cssComputedValue(style, PrCssProperty_MaskBorderOutset), border->outsetKind, border->outset,
        border->outsetNumber);
    CssValueRun repeat = cssComputedValue(style, PrCssProperty_MaskBorderRepeat);
    if (repeat.count != 0) {
        border->horizontal = sliceModeOf(cssValueKeyword(repeat.values));
        uint32_t second = repeat.values[0].next;
        if (second < repeat.count)
            border->vertical = sliceModeOf(cssValueKeyword(&repeat.values[second]));
        else
            border->vertical = border->horizontal;
    }
    bool luminance = cssComputedFirstKeyword(style, PrCssProperty_MaskBorderMode) == PrCssKeyword_Luminance;
    border->mode = luminance ? PrDrawMaskMode_Luminance : PrDrawMaskMode_Alpha;
}

// Returns the mask of a computed style, interned by the tree (css-masking-1/#positioned-masks): a layer for each item
// of mask-image, which takes the same item of each other mask list property, and the mask border. A layer of none is
// transparent. A layer that references no mask element, or whose image cannot be displayed, is left out, so that it
// masks nothing.
PrLayoutMaskIndex maskOf(PrCssPresentation* r, const CssComputedStyle* style, const ColorTarget& target,
        Array<PrLayoutSource>& sources) {
    PrLayoutMask mask{.border = prLayoutMaskBorderDefault()};
    CssPresentationImageDrawing drawing = imageDrawingOf(r, style);
    r->maskLayers.clear();
    r->maskSpans.clear();
    clearImages(r);
    CssValueRun images = cssComputedValue(style, PrCssProperty_MaskImage);
    if (!hasNoImages(images)) {
        CssValueRun modes = cssComputedValue(style, PrCssProperty_MaskMode);
        CssValueRun repeats = cssComputedValue(style, PrCssProperty_MaskRepeat);
        CssValueRun positions = cssComputedValue(style, PrCssProperty_MaskPosition);
        CssValueRun sizes = cssComputedValue(style, PrCssProperty_MaskSize);
        CssValueRun origins = cssComputedValue(style, PrCssProperty_MaskOrigin);
        CssValueRun clips = cssComputedValue(style, PrCssProperty_MaskClip);
        CssValueRun composites = cssComputedValue(style, PrCssProperty_MaskComposite);
        CssListItems imageItems = cssListItems(images);
        auto itemOf = [](CssValueRun run, uint32_t i) {
            CssListItems items = cssListItems(run);
            return items.count != 0 ? cssListItem(run, items, i) : CssValueRun{run.values, 0, run.text};
        };
        for (uint32_t i = 0; i < imageItems.count; ++i) {
            PrLayoutMaskLayer layer = prLayoutMaskLayerDefault();
            CssValueRun image = schemeImageOf(cssListItem(images, imageItems, i), target);
            CssPresentationImageSpans spans = imageSpansOf(r);
            PrStringView url{};
            bool fragment = image.count != 0 && urlOf(image, &url) && url.size != 0 && url.data[0] == '#';
            if (fragment) {
                if (!referenceOf(r, url, CssPresentationReferenceKind_Mask, &layer.source))
                    continue;
                sources.push(layer.source);
            } else if (cssValueRunFirstKeyword(image) != PrCssKeyword_None &&
                    !imageLayerOf(r, image, target, drawing, layer.image)) {
                continue;
            }
            PrCssKeyword mode = cssValueRunFirstKeyword(itemOf(modes, i));
            if (mode == PrCssKeyword_Alpha)
                layer.mode = PrLayoutMaskMode_Alpha;
            else if (mode == PrCssKeyword_Luminance)
                layer.mode = PrLayoutMaskMode_Luminance;
            else
                layer.mode = PrLayoutMaskMode_MatchSource;
            CssValueRun repeat = itemOf(repeats, i);
            if (repeat.count != 0)
                repeatStyleOf(repeat, layer.image);
            CssValueRun position = itemOf(positions, i);
            if (position.count != 0) {
                layer.image.positionX = prLayoutLengthPercent(0);
                layer.image.positionY = prLayoutLengthPercent(0);
                positionOf(r, position, &layer.image.positionX, &layer.image.positionY);
            }
            CssValueRun size = itemOf(sizes, i);
            if (size.count != 0)
                sizeOf(r, size, layer.image);
            CssValueRun origin = itemOf(origins, i);
            if (origin.count != 0)
                layer.image.origin = maskBoxOf(origin, PrCssProperty_MaskOrigin);
            CssValueRun clip = itemOf(clips, i);
            if (clip.count != 0) {
                layer.unclipped = cssValueRunFirstKeyword(clip) == PrCssKeyword_NoClip;
                layer.image.clip = maskBoxOf(clip, PrCssProperty_MaskClip);
            }
            CssValueRun composite = itemOf(composites, i);
            if (composite.count != 0)
                layer.composite = cssMaskCompositeOperator(cssValueRunFirstKeyword(composite));
            r->maskLayers.push(layer);
            r->maskSpans.push(spans);
        }
        for (uint32_t i = 0; i < r->maskLayers.count; ++i)
            pointImage(r, r->maskLayers.data[i].image, r->maskSpans.data[i]);
        mask.layerCount = r->maskLayers.count;
        mask.layers = r->maskLayers.data;
    }
    CssPresentationImageSpans borderSpans = imageSpansOf(r);
    maskBorderOf(r, style, target, drawing, &mask.border);
    pointImage(r, mask.border.image, borderSpans);
    PrLayoutMaskIndex index = prLayoutMaskIntern(r->tree, &mask);
    releaseImageSources(r);
    return index;
}

// Returns the filter list of a computed filter or backdrop-filter, interned by the tree
// (filter-effects-1/#FilterProperty). If a URL names no filter element, the whole list is ignored and filters nothing,
// though the box keeps its group.
PrLayoutFilterIndex filterListOf(PrCssPresentation* r, CssValueRun run, const CssColorContext& colors,
        Array<PrLayoutSource>& sources) {
    if (run.count == 0 || cssValueRunFirstKeyword(run) == PrCssKeyword_None)
        return kPrLayoutFilterNone;
    r->filters.clear();
    filterItemsOf(r, run, colors, r->filters, sources);
    // A list that filters nothing, because a URL names no filter element or the engine draws none of its functions,
    // still filters, as the identity.
    if (r->filters.count == 0)
        r->filters.push(PrLayoutFilter{.kind = PrLayoutFilterKind_Source});
    return prLayoutFilterIntern(r->tree, r->filters.data, r->filters.count);
}

// Returns the compositing group of a computed style (css-masking-1, filter-effects-1, filter-effects-2, compositing-1),
// interned by the tree, and sets `filtered` if its filter or backdrop filter is not none, or `will` names either. The
// default group is index 0, which most styles take without reading the properties.
PrLayoutGroupIndex compositingOf(PrCssPresentation* r, const CssComputedStyle* style, const ColorTarget& target,
        const WillChange& will, bool* filtered) {
    *filtered = will.filters;
    bool backdropRoot = will.composites || will.filters || will.groups;
    if (style->groups[CssStyleGroup_Compositing] == r->styles.initial[CssStyleGroup_Compositing] && !backdropRoot)
        return 0;
    Array<PrLayoutSource> sources;
    PrLayoutCompositingStyle c = prLayoutCompositingStyleDefault();
    clipPathOf(r, cssComputedValue(style, PrCssProperty_ClipPath), &c, sources);
    clipRectOf(r, cssComputedValue(style, PrCssProperty_Clip), &c);
    c.mask = maskOf(r, style, target, sources);
    CssValueRun filter = cssComputedValue(style, PrCssProperty_Filter);
    CssValueRun backdrop = cssComputedValue(style, PrCssProperty_BackdropFilter);
    c.filter = filterListOf(r, filter, target.context, sources);
    c.backdrop = filterListOf(r, backdrop, target.context, sources);
    *filtered = *filtered || c.filter != kPrLayoutFilterNone || c.backdrop != kPrLayoutFilterNone;
    c.backdropRoot = backdropRoot;
    mixBlendOf(cssComputedFirstKeyword(style, PrCssProperty_MixBlendMode), &c.blendMode, &c.composite);
    c.isolated = cssComputedFirstKeyword(style, PrCssProperty_Isolation) == PrCssKeyword_Isolate;
    PrLayoutGroupIndex index = prLayoutCompositingStyleIntern(r->tree, &c);
    for (uint32_t i = 0; i < sources.count; ++i)
        releaseSource(sources.data[i]);
    sources.reset();
    return index;
}

// Returns what the font properties of `style`, whose typography group is not the initial one, ask of shaping, as that
// group gives it (CssPresentation::featureSets), or null for the shaper's defaults. The presentation holds the
// reference.
const PrTextFeatureSet* PR_NULLABLE featureSetOf(PrCssPresentation* r, const CssComputedStyle* style) {
    const CssStyleValues* group = style->groups[CssStyleGroup_Typography];
    uint32_t hash = KeyIndex::hashOf(uint64_t(reinterpret_cast<uintptr_t>(group)));
    uint32_t at = r->featureSetIndex.find(hash, [&](uint32_t i) { return r->featureSets.data[i].group == group; });
    if (at != kHashMissing)
        return r->featureSets.data[at].set;
    cssStyleValuesRetain(group);
    r->featureSetIndex.insert(hash, r->featureSets.count);
    r->featureSets.push(CssPresentationFeatures{group, cssFontFeatureSet(style)});
    return r->featureSets.last().set;
}

// Returns the layout record that a computed style projects onto in `presentation`. The record has a layer of its own if
// a compositor animates its effects. Sets `readsOutside` if a color resolved for the display's headroom or the user's
// preferred scheme.
PrLayoutStyleIndex project(PrCssPresentation* r, const CssComputedStyle* style, const Presentation& presentation,
        bool layer, bool* readsOutside) {

    PrLayoutStyle record = prLayoutStyleDefault();

    PrLayoutFontStyle font = prLayoutFontStyleDefault();
    font.font = textFontOf(r, style);
    CssValueRun lineHeight = cssComputedValue(style, PrCssProperty_LineHeight);
    if (lineHeight.count == 1 && (lineHeight.values[0].kind == PrCssValueKind_Number ||
            lineHeight.values[0].kind == PrCssValueKind_Integer))
        font.lineHeight = prLayoutLineHeightNumber(float(lineHeight.values[0].number));
    else if (cssValueRunFirstKeyword(lineHeight) != PrCssKeyword_Normal)
        // A length is rounded to the nearest unit. A number is multiplied by the font size in layout, and the result is
        // floored there.
        font.lineHeight = prLayoutLineHeightUnits(prLayoutUnitFromPxRoundDouble(pxOf(lineHeight)));
    const CssComputedStyle* flow = presentation.flow != nullptr ? presentation.flow : style;
    font.writingMode = writingModeOf(cssComputedValue(flow, PrCssProperty_WritingMode));
    bool rtl = cssComputedFirstKeyword(flow, PrCssProperty_Direction) == PrCssKeyword_Rtl;
    font.direction = rtl ? PrTextDirection_Rtl : PrTextDirection_Ltr;
    font.textOrientation = textOrientationOf(cssComputedValue(style, PrCssProperty_TextOrientation));
    font.dominantBaseline = dominantBaselineOf(cssComputedValue(style, PrCssProperty_DominantBaseline));
    font.whiteSpaceCollapse = whiteSpaceCollapseOf(cssComputedValue(style, PrCssProperty_WhiteSpaceCollapse));
    bool nowrap = cssComputedFirstKeyword(style, PrCssProperty_TextWrapMode) == PrCssKeyword_Nowrap;
    font.textWrapMode = nowrap ? PrLayoutTextWrapMode_Nowrap : PrLayoutTextWrapMode_Wrap;
    if (!presentation.marker) {
        setTextIndent(r, font, cssComputedValue(style, PrCssProperty_TextIndent));
        font.textAlign = textAlignOf(cssComputedValue(style, PrCssProperty_TextAlignAll));
        font.textAlignLast = textAlignOf(cssComputedValue(style, PrCssProperty_TextAlignLast));
    }
    font.textJustify = textJustifyOf(cssComputedValue(style, PrCssProperty_TextJustify));
    font.textTransform = textTransformOf(cssComputedValue(style, PrCssProperty_TextTransform));
    // Text that declares no typography properties, as most text does, has no font features and no tailoring of line
    // breaks.
    if (style->groups[CssStyleGroup_Typography] != r->styles.initial[CssStyleGroup_Typography]) {
        font.features = featureSetOf(r, style);
        PrTextBreakStyle breaks = cssTextBreakStyle(style);
        font.wordBreak = breaks.wordBreak;
        font.overflowWrap = breaks.overflowWrap;
    }
    record.groups[PrLayoutGroup_Font] = prLayoutFontStyleIntern(r->tree, &font);

    // Colors resolve for the headroom that the element's dynamic-range-limit allows, and are capped at it if it is
    // below the display's. Only an element with such a limit pays for the cap.
    float cap = 0;
    float allowed = rangeCapOf(r, style, &cap, readsOutside);
    auto capped = [&](PrColor c) { return cap > 0 ? prColorLimit(c, cap) : c; };
    CssColorEnvironment environment = colorEnvironmentOf(r, allowed, readsOutside);
    CssColorContext colors = cssStyleColorContext(style, environment);
    colors.current = capped(colors.current);
    PrColor currentColor = colors.current;
    PrLayoutColorStyle color = prLayoutColorStyleDefault();
    color.color = prColor16FromColor(currentColor);
    bool visible = cssComputedFirstKeyword(style, PrCssProperty_Visibility) == PrCssKeyword_Visible;
    color.visibility = visible ? PrLayoutVisibility_Visible : PrLayoutVisibility_Hidden;
    bool hideCells = cssComputedFirstKeyword(style, PrCssProperty_EmptyCells) == PrCssKeyword_Hide;
    color.emptyCells = hideCells ? PrLayoutEmptyCells_Hide : PrLayoutEmptyCells_Show;
    // On a CSS box, SVG's values of pointer-events take the pointer as auto does. A replaced element whose
    // pointer-events is none still lets the content that it draws take the pointer. The descendants of an outermost svg
    // element may take it, as the descendants of a CSS box may (SVG2/interact.html#PointerEventsProp). An inert box,
    // with what it draws, hit tests as if its pointer-events were none (html/#inert-subtrees).
    bool inert = cssComputedFirstKeyword(style, PrCssProperty_Interactivity) == PrCssKeyword_Inert;
    bool none = cssComputedFirstKeyword(style, PrCssProperty_PointerEvents) == PrCssKeyword_None;
    if (inert)
        color.pointerEvents = PrLayoutPointerEvents_None;
    else if (!none)
        color.pointerEvents = PrLayoutPointerEvents_Auto;
    else if (presentation.replaced)
        color.pointerEvents = PrLayoutPointerEvents_Content;
    else
        color.pointerEvents = PrLayoutPointerEvents_None;
    color.wheelEvents = presentation.waitsForWheel ? PrLayoutWheelEvents_Blocking : PrLayoutWheelEvents_Passive;
    record.groups[PrLayoutGroup_Color] = prLayoutColorStyleIntern(r->tree, &color);

    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = displayOf(cssComputedValue(style, PrCssProperty_Display));
    bool borderBox = cssComputedFirstKeyword(style, PrCssProperty_BoxSizing) == PrCssKeyword_BorderBox;
    box.boxSizing = borderBox ? PrLayoutBoxSizing_BorderBox : PrLayoutBoxSizing_ContentBox;
    box.position = positionOf(cssComputedValue(style, PrCssProperty_Position));
    box.staticInline = style->staticInline;
    box.floating = floatOf(cssComputedValue(style, PrCssProperty_Float));
    box.clear = clearOf(cssComputedValue(style, PrCssProperty_Clear));
    box.baselineShift = baselineShiftOf(r, cssComputedValue(style, PrCssProperty_BaselineShift));
    box.alignmentBaseline = alignmentBaselineOf(cssComputedValue(style, PrCssProperty_AlignmentBaseline));
    box.baselineSource = baselineSourceOf(cssComputedValue(style, PrCssProperty_BaselineSource));
    const CssComputedStyle* bidi = presentation.bidi != nullptr ? presentation.bidi : style;
    box.unicodeBidi = unicodeBidiOf(cssComputedValue(bidi, PrCssProperty_UnicodeBidi));
    box.width = lengthOf(r, cssComputedValue(style, PrCssProperty_Width));
    box.height = lengthOf(r, cssComputedValue(style, PrCssProperty_Height));
    if (presentation.fillsViewport && box.width.bits == prLayoutLengthAuto().bits)
        box.width = prLayoutLengthPercent(100);
    if (presentation.fillsViewport && box.height.bits == prLayoutLengthAuto().bits)
        box.height = prLayoutLengthPercent(100);
    box.minWidth = lengthOf(r, cssComputedValue(style, PrCssProperty_MinWidth));
    box.minHeight = lengthOf(r, cssComputedValue(style, PrCssProperty_MinHeight));
    box.maxWidth = lengthOf(r, cssComputedValue(style, PrCssProperty_MaxWidth));
    box.maxHeight = lengthOf(r, cssComputedValue(style, PrCssProperty_MaxHeight));
    const PrCssProperty insets[] = {PrCssProperty_Top, PrCssProperty_Right, PrCssProperty_Bottom, PrCssProperty_Left};
    const PrCssProperty borderColors[] = {
        PrCssProperty_BorderTopColor, PrCssProperty_BorderRightColor,
        PrCssProperty_BorderBottomColor, PrCssProperty_BorderLeftColor,
    };
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        box.margin[side] = lengthOf(r, cssComputedValue(style, kMargins[side]));
        box.padding[side] = lengthOf(r, cssComputedValue(style, kPaddings[side]));
        box.inset[side] = lengthOf(r, cssComputedValue(style, insets[side]));
        double border = borderStyled(style, side) ? pxOf(cssComputedValue(style, kBorderWidths[side])) : 0;
        box.border[side] = prLayoutUnitFromPxDouble(border);
    }
    const CssComputedStyle* overflow = presentation.overflow != nullptr ? presentation.overflow : style;
    box.overflowX = overflowOf(cssComputedValue(overflow, PrCssProperty_OverflowX));
    box.overflowY = overflowOf(cssComputedValue(overflow, PrCssProperty_OverflowY));
    if (presentation.dropOverflow) {
        box.overflowX = PrLayoutOverflow_Visible;
        box.overflowY = PrLayoutOverflow_Visible;
    }
    // The root's own overscroll behavior is the viewport's, whichever element's overflow the viewport takes.
    box.overscrollX = overscrollOf(cssComputedValue(style, PrCssProperty_OverscrollBehaviorX));
    box.overscrollY = overscrollOf(cssComputedValue(style, PrCssProperty_OverscrollBehaviorY));
    // Layout reads whether the box has a transform. Only compositing reads its matrix. Most elements have no transform,
    // and skip making the function list.
    CssValueRun transforms[4] = {
        cssComputedValue(style, PrCssProperty_Translate), cssComputedValue(style, PrCssProperty_Rotate),
        cssComputedValue(style, PrCssProperty_Scale), cssComputedValue(style, PrCssProperty_Transform),
    };
    bool anyTransform = false;
    for (CssValueRun run : transforms)
        anyTransform = anyTransform || cssValueRunFirstKeyword(run) != PrCssKeyword_None;
    // A perspective makes a containing block for all descendants, as a transform does
    // (css-transforms-2/#perspective-property).
    CssValueRun perspective = cssComputedValue(style, PrCssProperty_Perspective);
    bool perspectives = perspective.count != 0 && cssValueRunFirstKeyword(perspective) != PrCssKeyword_None;
    WillChange will = willChangeOf(cssComputedValue(style, PrCssProperty_WillChange));
    box.transformed = perspectives || will.transforms;
    layer = layer || will.composites;
    PrLayoutTransform transform = prLayoutTransformIdentity();
    if (anyTransform) {
        PrLayoutTransformList functions =
            cssTransformFunctions(transforms[0], transforms[1], transforms[2], transforms[3]);
        box.transformed = box.transformed || functions.count != 0;
        // A translation by a percentage before a perspective counts as zero (prLayoutTransformListTransform).
        transform = prLayoutTransformListTransform(&functions);
    }
    // Layout reads whether the box's z-index is auto. Only painting reads its integer.
    CssValueRun zIndex = cssComputedValue(style, PrCssProperty_ZIndex);
    box.zIndexed = zIndex.count == 1 && zIndex.values[0].kind == PrCssValueKind_Integer;
    // Layout reads whether the box's visibility is collapse. Painting reads whether it is visible.
    box.collapsed = cssComputedFirstKeyword(style, PrCssProperty_Visibility) == PrCssKeyword_Collapse;
    bool unanchored = cssComputedFirstKeyword(style, PrCssProperty_OverflowAnchor) == PrCssKeyword_None;
    box.overflowAnchor = unanchored ? PrLayoutOverflowAnchor_None : PrLayoutOverflowAnchor_Auto;
    aspectRatioOf(cssComputedValue(style, PrCssProperty_AspectRatio), box);
    // content-visibility: hidden skips the element's contents, which applies size containment
    // (css-contain-2/#content-visibility).
    box.containInline = containsInline(cssComputedValue(style, PrCssProperty_Contain)) ||
        containsInline(cssComputedValue(style, PrCssProperty_ContainerType)) ||
        cssComputedFirstKeyword(style, PrCssProperty_ContentVisibility) == PrCssKeyword_Hidden;
    box.containIntrinsicWidth = containIntrinsicOf(r, cssComputedValue(style, PrCssProperty_ContainIntrinsicWidth));
    box.containIntrinsicHeight = containIntrinsicOf(r, cssComputedValue(style, PrCssProperty_ContainIntrinsicHeight));
    // A text field of fixed field sizing sizes its inline axis in characters, and not by the text it shows
    // (html/#the-input-element-as-a-text-entry-widget).
    if (presentation.characters != 0 &&
            cssComputedFirstKeyword(style, PrCssProperty_FieldSizing) != PrCssKeyword_Content) {
        box.containInline = 1;
        box.intrinsicCharacters = presentation.characters;
    }
    // A replaced element whose display is layout-internal is laid out as inline
    // (css-display-3/#layout-specific-display), and so is a text field, whose box is a control's.
    if ((presentation.replaced || presentation.characters != 0) && prLayoutDisplayIsLayoutInternal(box.display))
        box.display = PrLayoutDisplay_Inline;
    if (presentation.buttonLayout || presentation.characters != 0)
        applyFlowRootInside(box);
    if (presentation.outsideMarker)
        box.display = PrLayoutDisplay_Marker;
    if (presentation.placed)
        placeInContainingBlock(box);
    // SVG applies a foreignObject's effects to its content, as it applies the foreignObject's transform, so a placed
    // box takes no compositing group.
    bool filtered = false;
    PrLayoutGroupIndex compositing =
        presentation.placed ? 0 : compositingOf(r, style, ColorTarget{colors, cap}, will, &filtered);
    box.filtered = filtered ? 1 : 0;
    // The clip rectangle applies to an absolutely positioned box alone (css-masking-1/#clip-property).
    bool absolute = box.position == PrLayoutPosition_Absolute || box.position == PrLayoutPosition_Fixed;
    box.clipped = absolute && compositing != 0 && prLayoutCompositingStyleRecord(r->tree, compositing).clips ? 1 : 0;
    // Size containment has no effect on an internal table box or on an inline box that is not atomic
    // (css-contain-2/#containment-size).
    bool inlineBox = box.display == PrLayoutDisplay_Inline && !presentation.replaced;
    if (prLayoutDisplayIsLayoutInternal(box.display) || inlineBox)
        box.containInline = 0;
    record.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(r->tree, &box);
    // For most elements, the flex group is the default, which is the record's own.
    PrLayoutFlexStyle flex = flexOf(r, style);
    PrLayoutFlexStyle initial = prLayoutFlexStyleDefault();
    if (!prMemEquals(&flex, &initial, sizeof(PrLayoutFlexStyle)))
        record.groups[PrLayoutGroup_Flex] = prLayoutFlexStyleIntern(r->tree, &flex);
    // For most elements, the grid group is the default, which only a grid container or an element placed in a grid
    // declares.
    if (style->groups[CssStyleGroup_Grid] != r->styles.initial[CssStyleGroup_Grid]) {
        PrLayoutGridStyle grid = gridOf(r, style);
        PrLayoutGridStyle initialGrid = prLayoutGridStyleDefault();
        if (!prMemEquals(&grid, &initialGrid, sizeof(PrLayoutGridStyle)))
            record.groups[PrLayoutGroup_Grid] = prLayoutGridStyleIntern(r->tree, &grid);
    }
    // For most elements, the table group is the default too. The spacing, border model and caption side in it are
    // inherited, and have their initial values outside a table. Only a table reads table-layout.
    bool tableLike = box.display == PrLayoutDisplay_Table || box.display == PrLayoutDisplay_InlineTable;
    bool given = style->groups[CssStyleGroup_Table] != r->styles.initial[CssStyleGroup_Table];
    if (presentation.spans || tableLike || given) {
        PrLayoutTableStyle table = tableOf(style, presentation.spans ? presentation.columnSpan : 1,
            presentation.spans ? presentation.rowSpan : 1);
        PrLayoutTableStyle initialTable = prLayoutTableStyleDefault();
        if (!prMemEquals(&table, &initialTable, sizeof(PrLayoutTableStyle)))
            record.groups[PrLayoutGroup_Table] = prLayoutTableStyleIntern(r->tree, &table);
    }

    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
    // The background is the element's own, or the one that it takes from another, which resolves its colors with that
    // element's color.
    const CssComputedStyle* backgroundStyle = presentation.background != nullptr ? presentation.background : style;
    CssColorContext backgroundColors = colors;
    if (backgroundStyle != style) {
        backgroundColors = cssStyleColorContext(backgroundStyle, environment);
        backgroundColors.current = capped(backgroundColors.current);
    }
    if (!presentation.dropBackground) {
        PrColor background{0, 0, 0, 0};
        cssResolveColor(cssComputedValue(backgroundStyle, PrCssProperty_BackgroundColor), 0, backgroundColors,
            &background);
        decoration.background = prColor16FromColor(capped(background));
        if (presentation.image.count != 0)
            decoration.backgroundLayers =
                generatedBackgroundOf(r, presentation.image, ColorTarget{backgroundColors, cap}, imageDrawingOf(r, style));
        else
            decoration.backgroundLayers = backgroundLayersOf(r, backgroundStyle, ColorTarget{backgroundColors, cap});
        decoration.backgroundClip = backgroundClipOf(backgroundStyle);
        decoration.backgroundAttachment = backgroundAttachmentOf(backgroundStyle);
    } else {
        decoration.background = PR_COLOR16(0, 0, 0, 0);
    }
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        PrColor border = prColorFromColor16(decoration.borderColor[side]);
        cssResolveColor(cssComputedValue(style, borderColors[side]), 0, colors, &border);
        decoration.borderColor[side] = prColor16FromColor(capped(border));
        decoration.borderStyle[side] = borderStyleOf(cssComputedValue(style, kBorderStyles[side]));
    }
    cornersOf(r, style, &decoration);
    decoration.objectFit = objectFitOf(cssComputedValue(style, PrCssProperty_ObjectFit));
    CssValueRun objectPosition = cssComputedValue(style, PrCssProperty_ObjectPosition);
    if (objectPosition.count != 0)
        positionOf(r, objectPosition, &decoration.objectPositionX, &decoration.objectPositionY);
    // An outline of style auto is drawn solid. An outline of style none has no width (css-ui-4/#outline-style).
    CssValueRun outlineStyle = cssComputedValue(style, PrCssProperty_OutlineStyle);
    if (cssValueRunFirstKeyword(outlineStyle) == PrCssKeyword_Auto)
        decoration.outlineStyle = PrDrawBorderStyle_Solid;
    else
        decoration.outlineStyle = borderStyleOf(outlineStyle);
    if (decoration.outlineStyle != PrDrawBorderStyle_None) {
        PrColor outline = currentColor;
        cssResolveColor(cssComputedValue(style, PrCssProperty_OutlineColor), 0, colors, &outline);
        decoration.outlineColor = prColor16FromColor(capped(outline));
        decoration.outlineWidth = prLayoutUnitFromPxDouble(pxOf(cssComputedValue(style, PrCssProperty_OutlineWidth)));
        decoration.outlineOffset = prLayoutUnitFromPxDouble(pxOf(cssComputedValue(style, PrCssProperty_OutlineOffset)));
    }
    record.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(r->tree, &decoration);

    PrLayoutEffectStyle effect = prLayoutEffectStyleDefault();
    CssValueRun opacity = cssComputedValue(style, PrCssProperty_Opacity);
    if (opacity.count == 1 && opacity.values[0].kind == PrCssValueKind_Number)
        effect.opacity = prF32Clamp(float(opacity.values[0].number), 0, 1);
    effect.transform = transform;
    CssValueRun origin = cssComputedValue(style, PrCssProperty_TransformOrigin);
    if (origin.count != 0) {
        effect.originX = originOf(r, origin, 0);
        uint32_t second = origin.values[0].next;
        // A single vertical keyword sets the y of the origin, and leaves x at the center.
        bool vertical = cssValueIsKeyword(origin.values, PrCssKeyword_Top) ||
            cssValueIsKeyword(origin.values, PrCssKeyword_Bottom);
        if (second >= origin.count && vertical) {
            effect.originY = effect.originX;
            effect.originX = prLayoutLengthPercent(50);
        }
        if (second < origin.count) {
            effect.originY = originOf(r, origin, second);
            uint32_t third = second + origin.values[second].next;
            if (third < origin.count)
                effect.originZ = float(pxOf(cssValueSubtree(origin, third)));
        }
    }
    effect.transformBox = transformBoxOf(cssComputedValue(style, PrCssProperty_TransformBox));
    // A perspective below 1px renders as 1px (css-transforms-2/#perspective-property). Its origin matters only with it,
    // and a hidden back face only on a box that a transform can turn.
    if (perspectives) {
        effect.perspective = prF32Max(float(pxOf(perspective)), 1);
        CssValueRun perspectiveOrigin = cssComputedValue(style, PrCssProperty_PerspectiveOrigin);
        if (perspectiveOrigin.count != 0)
            positionOf(r, perspectiveOrigin, &effect.perspectiveOriginX, &effect.perspectiveOriginY);
    }
    if (anyTransform || layer)
        effect.backfaceHidden = cssComputedFirstKeyword(style, PrCssProperty_BackfaceVisibility) == PrCssKeyword_Hidden;
    effect.layer = layer;
    if (box.zIndexed)
        effect.zIndex = int32_t(zIndex.values[0].number);
    // The content of a foreignObject's svg element applies the foreignObject's transform and effects.
    if (presentation.placed)
        effect = prLayoutEffectStyleDefault();
    record.groups[PrLayoutGroup_Effect] = prLayoutEffectStyleIntern(r->tree, &effect);
    record.groups[PrLayoutGroup_Compositing] = compositing;
    return prLayoutStyleIntern(r->tree, &record);
}

// Returns true if display: contents makes the element generate no box at all, the same as display: none.
bool unboxesToNothing(const PrDomElement* element) {
    return (cssBoxTraits(element).flags & CssBoxTraitFlag_Unboxes) != 0;
}

// Returns true if the element, styled by `style`, generates a box, assuming that its parent has a box for its
// children's boxes. Neither display none nor display contents generates one.
bool generatesBox(const PrDomElement*, const CssComputedStyle* style) {
    CssValueRun display = cssComputedValue(style, PrCssProperty_Display);
    return displayOf(display) != PrLayoutDisplay_None && cssValueRunFirstKeyword(display) != PrCssKeyword_Contents;
}

// Returns true if the element, styled by `style`, has display contents, so that it generates no box of its own and
// gives its children's boxes to its parent's box in its place (css-display-4/#box-generation). This assumes that its
// parent has a box for its children's boxes. An element whose box is unusual generates no boxes at all
// (css-display-4/#unbox).
bool passesBoxes(const PrDomElement* element, const CssComputedStyle* style) {
    return cssComputedFirstKeyword(style, PrCssProperty_Display) == PrCssKeyword_Contents && !unboxesToNothing(element);
}

// Returns true if the ::marker of a list item styled by `item` is outside, and takes no place in the item's content
// (css-lists-3/#list-style-position-property). The marker of an inline list item is inside whatever its
// list-style-position says.
bool isOutsideMarker(const CssComputedStyle* item) {
    return cssComputedFirstKeyword(item, PrCssProperty_ListStylePosition) != PrCssKeyword_Inside &&
        displayOf(cssComputedValue(item, PrCssProperty_Display)) != PrLayoutDisplay_Inline;
}

// Returns the record of the box of `node`, styled by `style`, for a node that is neither the root nor the body.
//
// A marker's box takes the inherited values of its style, and its unicode-bidi. Of the properties that do not inherit,
// only those apply to a marker box that css-lists-3/#marker-properties lists, and the box takes the initial values of
// the others. The inherited values also reach the marker's text, whose record is the box's. An outside marker's box is
// a marker box, which layout places beside its list item's first line.
PrLayoutStyleIndex recordOf(PrCssPresentation* r, const PrDomNode* node, const CssComputedStyle* style, bool layer,
        bool* readsOutside) {
    if (!isPseudo(node) || !cssPseudoIsMarker(static_cast<const CssPresentationPseudo*>(node)->pseudo))
        return project(r, style, presentationOf(node), layer, readsOutside);
    CssComputedStyle marker{};
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g)
        marker.groups[g] = cssStyleGroupInherited(CssStyleGroup(g)) ? style->groups[g] : r->styles.initial[g];
    marker.custom = style->custom;
    marker.locale = style->locale;
    marker.font = style->font;
    const CssComputedStyle* item = r->entries.data[cssPresentationNodeEntry(r, node->parent)].style;
    return project(r, &marker, Presentation{.bidi = style, .outsideMarker = isOutsideMarker(item), .marker = true},
        layer, readsOutside);
}

// Returns the record of an entry's box, for a node that is neither the root nor the body. projectCanvas sets the
// records of the root and the body.
PrLayoutStyleIndex projectEntry(PrCssPresentation* r, CssPresentationEntry& entry) {
    entry.readsOutside = false;
    r->projecting = entry.node;
    PrLayoutStyleIndex record = recordOf(r, entry.node, entry.style, entry.composited, &entry.readsOutside);
    r->readsOutside = r->readsOutside || entry.readsOutside;
    return record;
}

// Returns true if an element's box holds the boxes of its children. The box of a replaced element does not, because its
// content is its resource, and neither does the box of a br, which is a line break.
bool holdsChildren(const PrDomElement* element) {
    return !isReplaced(element) && !isLineBreak(element);
}

// Returns the content that the element's box embeds, which is the id of the scenes of the content navigable that its
// browsing context shows in it, as in an iframe or an object, or else 0.
uint64_t embedOf(const PrDomElement* element) {
    return element->state != nullptr ? element->state->embed : 0;
}

// Returns the natural dimensions of a replaced element's resource, oriented as `oriented` says: those that its markup
// gives it, or else its image's once the image is completely available. An image that is not completely available has
// nothing to show, and its natural width and height are 0. Embedded content has no natural dimensions, and takes the
// default object size.
PrLayoutNaturalSize naturalOf(PrCssPresentation* r, const PrDomElement* element, bool oriented) {
    const DomElementState* state = element->state;
    PrLayoutNaturalSize size = prLayoutNaturalSizeNone();
    if (embedOf(element) != 0)
        return size;
    CssBoxTraits traits = cssBoxTraits(element);
    if ((traits.flags & CssBoxTraitFlag_Sized) != 0)
        return traits.natural;
    if (domIsOuterSvg(element)) {
        const CssPresentationSvg* svg = svgFind(r, element);
        return svg != nullptr ? svg->natural : size;
    }
    bool available = state != nullptr && state->image == PrDomImageState_CompletelyAvailable;
    // An SVG image's natural dimensions are its own, and it may lack them.
    if (available && state->vector.paint != nullptr)
        return state->vectorNatural;
    size.width = available ? prLayoutUnitFromPxDouble(state->naturalWidth) : PrLayoutUnit{0};
    size.height = available ? prLayoutUnitFromPxDouble(state->naturalHeight) : PrLayoutUnit{0};
    return available ? orientedSize(size, state->orientation, oriented) : size;
}

// Returns the decoded image that a replaced element shows, oriented as `oriented` says, or null for none.
const PrDrawImage* PR_NULLABLE pictureOf(const PrDomElement* element, bool oriented) {
    DomElementState* state = element->state;
    if (state == nullptr || state->picture == nullptr)
        return nullptr;
    return orientedImage(state->picture, state->orientation, oriented, &state->storedPicture);
}

// Returns true if lib/svg keeps state for the presentation, which it does once it has attached to it, and so draws the
// content of outermost svg elements.
bool presentsSvg(const PrCssPresentation* r) {
    return r->svgState != nullptr;
}

// Frees a record of an outermost svg element, with its content.
void freeSvg(CssPresentationSvg* svg) {
    if (svg->functions != nullptr)
        svg->functions->release(svg->content);
    svg->foreignObjects.reset();
    prMemoryFree(svg);
}

// Returns the record of the outermost svg element `element`, or null if it has none.
const CssPresentationSvg* PR_NULLABLE svgFind(const PrCssPresentation* r, const PrDomElement* element) {
    for (uint32_t i = 0; i < r->svgs.count; ++i) {
        if (r->svgs.data[i]->element == element)
            return r->svgs.data[i];
    }
    return nullptr;
}

// Returns the record of the outermost svg element `element`, making it if the presentation has none, or null if nothing
// draws the content of svg elements.
CssPresentationSvg* PR_NULLABLE svgOf(PrCssPresentation* r, const PrDomElement* element) {
    if (const CssPresentationSvg* found = svgFind(r, element))
        return const_cast<CssPresentationSvg*>(found);
    if (!presentsSvg(r))
        return nullptr;
    CssPresentationSvg* svg = allocZeroed<CssPresentationSvg>();
    svg->element = element;
    svg->natural = prLayoutNaturalSizeNone();
    svg->stale = true;
    r->svgs.push(svg);
    return svg;
}

// Projects again the nodes whose records name a source of the references, so that they take their sources again.
void reprojectReferencing(PrCssPresentation* r) {
    for (uint32_t i = 0; i < r->referencing.count; ++i) {
        const PrDomNode* node = r->referencing.data[i];
        uint32_t at = cssPresentationNodeEntry(r, node);
        if (at == kHashMissing || r->entries.data[at].node == nullptr)
            continue;
        r->entries.data[at].reprojectPending = true;
        mark(const_cast<PrDomNode*>(node), DomStyleFlag_Self);
        r->changed = true;
    }
}

// Notes a change at the SVG element `element` for lib/svg, which takes it once the boxes are applied.
void svgNote(PrCssPresentation* r, const PrDomElement* element, CssPresentationSvgNoteKind kind) {
    if (element->space != PrDomNamespace_Svg || !presentsSvg(r))
        return;
    PrDomElement* held = const_cast<PrDomElement*>(element);
    domNodeHold(held);
    r->svgNotes.push(CssPresentationSvgNote{held, kind});
}

// Notes for lib/svg that everything that the content of svg elements reads may have changed: the document's fonts, or
// what colors resolve for.
void svgInvalidate(PrCssPresentation* r) {
    if (!presentsSvg(r))
        return;
    r->svgInvalid = true;
    r->changed = true;
}

// Sets the natural size and content of a replaced element's box from the element. The content is the element's image,
// drawn at its natural size, whether the host decoded it or it is an already drawn SVG image. For an iframe or an
// object that represents its content navigable, the content is the browsing context that it embeds.
void applyContent(PrCssPresentation* r, const PrDomElement* element, PrLayoutNode box) {
    // The image is oriented as the element's image-orientation says, capped as its dynamic-range-limit caps its colors,
    // and resampled as its image-rendering says.
    CssPresentationImageDrawing drawing{.rendering = PrLayoutImageRendering_Auto, .oriented = true};
    uint32_t at = cssPresentationNodeEntry(r, element);
    if (at != kHashMissing && r->entries.data[at].style != nullptr) {
        drawing = imageDrawingOf(r, r->entries.data[at].style);
        r->entries.data[at].imageDrawing = drawing;
    }
    // The box of an outermost svg element is drawn by its content, which lib/svg brings up to date for the new box.
    CssPresentationSvg* svg = domIsOuterSvg(element) ? svgOf(r, element) : nullptr;
    if (svg != nullptr)
        cssPresentationSvgStale(r, svg);
    PrLayoutNaturalSize size = naturalOf(r, element, drawing.oriented);
    prLayoutNodeSetNaturalSize(r->tree, box, size);
    if (domIsOuterSvg(element))
        return;
    const PrDrawImage* picture = pictureOf(element, drawing.oriented);
    PrLayoutContent content{};
    if (embedOf(element) != 0) {
        content.kind = PrLayoutContentKind_Embed;
        content.embed = embedOf(element);
    } else if (picture != nullptr && size.width.raw > 0 && size.height.raw > 0) {
        content.kind = PrLayoutContentKind_Image;
        content.image = picture;
        content.limit = drawing.limit;
        content.rendering = drawing.rendering;
    } else if (element->state != nullptr && element->state->vector.paint != nullptr) {
        content.kind = PrLayoutContentKind_Painter;
        content.painter = element->state->vector;
        content.revision = element->state->vectorRevision;
    }
    if (const CssBoxProvider* provider = cssBoxProviderOf(element)) {
        content.text = provider->replacedText;
        content.textContext = const_cast<PrDomElement*>(element);
    }
    prLayoutNodeSetContent(r->tree, box, &content);
}

// The viewport that a presentation lays out in before its first update, in CSS pixels. A media environment starts with
// the same viewport.
constexpr PrLayoutVec kDefaultExtent = PrLayoutVec{{800 * kPrLayoutUnitsPerPx}, {600 * kPrLayoutUnitsPerPx}};

// Returns the entry of `node`, making one if the presentation has none.
uint32_t entryFor(PrCssPresentation* r, const PrDomNode* node) {
    uint32_t at = cssPresentationNodeEntry(r, node);
    if (at != kHashMissing)
        return at;
    if (r->free.count != 0) {
        at = r->free.last();
        r->free.pop();
    } else {
        at = r->entries.count;
        r->entries.emplace();
    }
    r->entries.data[at] = CssPresentationEntry{.node = node, .layoutNode = kPrLayoutNodeNone};
    node->presentationEntry = at + 1;
    return at;
}

// Returns the node that a query of the document sees for `node`: the originating element of a pseudo-element, which the
// document does not hold, and `node` itself otherwise.
PrDomNode* PR_NULLABLE seen(PrDomNode* PR_NULLABLE node) {
    while (node != nullptr && isPseudo(node))
        node = node->parent;
    return node;
}

// Records `node` as the node that the layout node presents, by making it the layout node's key.
void present(PrCssPresentation* r, PrLayoutNode layoutNode, PrDomNode* PR_NULLABLE node) {
    prLayoutNodeSetKey(r->tree, layoutNode, uint64_t(uintptr_t(node)));
}

// Returns the node of the document that a layout node presents, or null. The presentation must own the tree.
PrDomNode* PR_NULLABLE presented(const PrCssPresentation* r, PrLayoutNode layoutNode) {
    if (!prLayoutNodeIsValid(r->tree, layoutNode))
        return nullptr;
    return reinterpret_cast<PrDomNode*>(uintptr_t(prLayoutNodeKey(r->tree, layoutNode)));
}

// Returns the node of the document that the nearest of a layout node and its ancestors presents, or null. The boxes of
// a pseudo-element's content are anonymous, and the pseudo-element's box presents it. The presentation must own the
// tree.
PrDomNode* PR_NULLABLE presenter(const PrCssPresentation* r, PrLayoutNode layoutNode) {
    for (PrLayoutNode n = layoutNode; n != kPrLayoutNodeNone && prLayoutNodeIsValid(r->tree, n);
            n = prLayoutNodeParent(r->tree, n)) {
        if (PrDomNode* node = presented(r, n))
            return node;
    }
    return nullptr;
}

// Forgets the nodes that a layout node and its subtree present, as those nodes leave the document, so that no query
// returns them. The layout nodes stay in the tree, which queries read as of the last layout, until the next update
// erases them.
void unmap(PrCssPresentation* r, PrLayoutNode box) {
    for (PrLayoutNode n = box; n != kPrLayoutNodeNone;) {
        if (const PrDomNode* node = presented(r, n)) {
            uint32_t entry = cssPresentationNodeEntry(r, node);
            if (entry != kHashMissing && r->entries.data[entry].layoutNode == n) {
                r->entries.data[entry].layoutNode = kPrLayoutNodeNone;
                r->entries.data[entry].boxed = false;
            }
        }
        present(r, n, nullptr);
        PrLayoutNode next = prLayoutNodeFirstChild(r->tree, n);
        while (next == kPrLayoutNodeNone && n != box) {
            next = prLayoutNodeNextSibling(r->tree, n);
            if (next == kPrLayoutNodeNone)
                n = prLayoutNodeParent(r->tree, n);
        }
        n = next;
    }
}

// Clears the boxes that the descendants of `element` and their pseudo-elements were to be given, as the element loses
// its box. Erasing the element's box forgets the boxes that its descendants have. A box that was queued and not made
// would otherwise be made with no parent, and its entry would not be queued again once the element has a box.
void dropPendingBoxes(PrCssPresentation* r, const PrDomElement* element) {
    DomFlatWalk walk;
    for (const PrDomNode* node = domFlatWalkNext(walk, element, element); node != nullptr;
            node = domFlatWalkNext(walk, element, node)) {
        auto drop = [&](const PrDomNode* owner) {
            uint32_t at = cssPresentationNodeEntry(r, owner);
            if (at != kHashMissing && r->entries.data[at].boxed && r->entries.data[at].layoutNode == kPrLayoutNodeNone)
                r->entries.data[at].boxed = false;
            return at;
        };
        uint32_t at = drop(node);
        for (uint32_t which = 0; at != kHashMissing && which < kCssPresentationPseudos; ++which) {
            if (const CssPresentationPseudo* pseudo = r->entries.data[at].pseudos[which])
                drop(pseudo);
        }
    }
    domFlatWalkReset(walk);
}

// Returns true if the agent's layout worker is running a job, and so holds the tree.
bool workerHolds(const PrCssPresentation* r) {
    return r->agent->worker != nullptr && prLayoutWorkerIsBusy(r->agent->worker);
}

// Removes an entry's box from the presentation's layout tree, to be erased at the next update. While the worker holds
// the tree, the nodes that the box presents are forgotten once the worker is done.
void eraseBox(PrCssPresentation* r, uint32_t at) {
    PrLayoutNode box = r->entries.data[at].layoutNode;
    r->entries.data[at].boxed = false;
    if (box == kPrLayoutNodeNone)
        return;
    const PrDomElement* element = domElementFromNode(r->entries.data[at].node);
    if (element != nullptr && isForeignRoot(element)) {
        --r->foreignBoxes;
        svgNote(r, element, CssPresentationSvgNoteKind_Box);
    }
    r->entries.data[at].layoutNode = kPrLayoutNodeNone;
    r->erasing.push(box);
    if (workerHolds(r))
        r->unmapping.push(box);
    else
        unmap(r, box);
}

// The source of a scroll of the tree.
enum {
    CssPresentationScrollBy_Host,            // the host, applying what its compositor already shows
    CssPresentationScrollBy_Document,        // the document, whose offsets the host takes for its compositor
};
typedef uint8_t CssPresentationScrollBy;

// Queues the scroll and scrollend events of the scroll container of layout `node`, or of the document for
// kPrLayoutNodeNone, for the next rendering update. A container is queued once.
void queueScrolled(PrCssPresentation* r, PrLayoutNode node) {
    PrDomNode* target;
    if (node == kPrLayoutNodeNone)
        target = static_cast<PrDomNode*>(r->document);
    else
        target = cssPresentationNodeFromLayout(r, node);
    if (target == nullptr)
        return;
    for (uint32_t i = 0; i < r->scrolled.count; ++i) {
        if (r->scrolled.data[i] == target)
            return;
    }
    domNodeHold(target);
    r->scrolled.push(target);
}

// Scrolls the tree, which the presentation owns, in the same way as cssPresentationScrollTo, on behalf of `by`.
void scrollTree(PrCssPresentation* r, PrLayoutNode node, PrLayoutVec offset, CssPresentationScrollBy by) {
    // A scroll that a compositor made of a box that has since been erased refers to nothing.
    if (node != kPrLayoutNodeNone && !prLayoutNodeIsValid(r->tree, node))
        return;
    PrLayoutScrollInfo info{};
    if (!prLayoutNodeScrollInfo(r->tree, node, &info) || !info.scrollContainer)
        return;
    auto clamp = [](PrLayoutUnit unit, PrLayoutUnit least, PrLayoutUnit greatest) {
        return unit < least ? least : unit > greatest ? greatest : unit;
    };
    PrLayoutVec to{
        clamp(offset.x, info.range.left, info.range.right),
        clamp(offset.y, info.range.top, info.range.bottom),
    };
    prLayoutNodeSetScroll(r->tree, node, to);
    if (to == info.offset)
        return;
    if (by == CssPresentationScrollBy_Document) {
        uint32_t at = 0;
        while (at < r->documentScrolls.count && r->documentScrolls.data[at].node != node)
            ++at;
        if (at == r->documentScrolls.count)
            r->documentScrolls.push(PrLayoutSceneScroll{node, to});
        else
            r->documentScrolls.data[at].offset = to;
    }
    queueScrolled(r, node);
}

// Adds the moves that scroll anchoring made in the tree's last layouts (css-scroll-anchoring-1/#scroll-adjustment) to
// the moves since the last run of the scroll steps, which queues their scroll events. The host's compositor learns of
// the moves from the scene.
void anchorsMoved(PrCssPresentation* r) {
    PrLayoutScrollAdjustment moved[8];
    uint32_t count = 0;
    while ((count = prLayoutTreeTakeScrollAdjustments(r->tree, moved, 8)) != 0) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t at = 0;
            while (at < r->anchorMoves.count && r->anchorMoves.data[at].node != moved[i].node)
                ++at;
            if (at == r->anchorMoves.count)
                r->anchorMoves.push(moved[i]);
            else
                r->anchorMoves.data[at].delta = r->anchorMoves.data[at].delta + moved[i].delta;
        }
    }
}

// Brings up to date at the next update the content of each svg element whose box the last layout resized. Its
// percentages and the layout roots of its foreignObjects may be of the box's extent, and its hit testing and queries
// map through that extent even when a viewBox sizes what it builds. Returns true if any content is stale.
bool svgsResized(PrCssPresentation* r) {
    // A layout that made no box fragment resized no box.
    if (r->svgs.count == 0 || prLayoutTreeStats(r->tree).boxFragmentsMade == 0)
        return false;
    bool stale = false;
    for (uint32_t i = 0; i < r->svgs.count; ++i) {
        CssPresentationSvg* svg = r->svgs.data[i];
        uint32_t at = cssPresentationNodeEntry(r, svg->element);
        if (at == kHashMissing || !r->entries.data[at].boxed || r->entries.data[at].layoutNode == kPrLayoutNodeNone)
            continue;
        if (prLayoutRectExtent(prLayoutNodeReplacedContent(r->tree, r->entries.data[at].layoutNode)) != svg->extent) {
            cssPresentationSvgStale(r, svg);
            stale = true;
        }
    }
    return stale;
}

// Takes the tree back from the worker, waiting if the worker holds it. Then forgets the nodes that the boxes removed in
// the meantime present, draws again the svg elements that the worker's layout resized, and applies the scrolls that the
// host made in the meantime. Returns true if the content of an svg element is stale for its box's new extent.
bool own(PrCssPresentation* r) {
    if (r->agent->worker != nullptr)
        prLayoutWorkerWait(r->agent->worker);
    bool stale = svgsResized(r);
    anchorsMoved(r);
    for (uint32_t i = 0; i < r->unmapping.count; ++i)
        unmap(r, r->unmapping.data[i]);
    r->unmapping.clear();
    if (r->pendingScrolls.count == 0)
        return stale;
    Array<PrLayoutSceneScroll> pending = r->pendingScrolls;
    r->pendingScrolls = {};
    for (uint32_t i = 0; i < pending.count; ++i)
        scrollTree(r, pending.data[i].node, pending.data[i].offset, CssPresentationScrollBy_Host);
    pending.reset();
    return stale;
}

// Erases the boxes removed since the last update. No query reads them after that update.
void eraseTaken(PrCssPresentation* r) {
    for (uint32_t i = 0; i < r->erasing.count; ++i) {
        PrLayoutNode box = r->erasing.data[i];
        if (r->root == box)
            r->root = kPrLayoutNodeNone;
        prLayoutTreeErase(r->tree, box);
        ++r->accrued.boxesErased;
    }
    r->erasing.clear();
}

// Frees a pseudo-element that no entry, queue or box refers to.
void freePseudo(CssPresentationPseudo* PR_NULLABLE pseudo) {
    if (pseudo == nullptr)
        return;
    pseudo->counters.text.reset();
    pseudo->counters.ends.reset();
    prMemoryFree(pseudo);
}

uint32_t firstsHash(const PrDomElement* element) {
    return KeyIndex::hashOf(uint64_t(reinterpret_cast<uintptr_t>(element)));
}

// Returns the index in CssPresentation::firsts of the styles of `element`, or kHashMissing.
uint32_t firstsOf(const PrCssPresentation* r, const PrDomElement* element) {
    return r->firstsIndex.find(firstsHash(element), [&](uint32_t i) { return r->firsts.data[i].element == element; });
}

// Takes from the boxes what the ::first-line and ::first-letter of `firsts` gave them: the records of the first
// formatted line, and the box of the first letter, whose text goes back to the text box it was taken from.
void clearFirsts(PrCssPresentation* r, CssPresentationFirsts& firsts) {
    for (uint32_t i = 0; i < firsts.lined.count; ++i) {
        if (prLayoutNodeIsValid(r->tree, firsts.lined.data[i]))
            prLayoutNodeSetFirstLineStyle(r->tree, firsts.lined.data[i], kPrLayoutStyleNone);
    }
    firsts.lined.clear();
    // A text node of the document holds its data, which may have changed since. A text box of generated content holds
    // the text it held before.
    if (firsts.letterSource != kPrLayoutNodeNone && prLayoutNodeIsValid(r->tree, firsts.letterSource)) {
        PrDomNode* node = presented(r, firsts.letterSource);
        PrStringView text{firsts.letterFull.data, firsts.letterFull.count};
        if (node != nullptr && domIsText(node))
            text = prStringView(&static_cast<PrDomCharacterData*>(node)->data);
        prLayoutNodeSetText(r->tree, firsts.letterSource, text);
    }
    if (firsts.letterBox != kPrLayoutNodeNone && prLayoutNodeIsValid(r->tree, firsts.letterBox)) {
        if (prLayoutNodeParent(r->tree, firsts.letterBox) != kPrLayoutNodeNone)
            prLayoutTreeRemove(r->tree, firsts.letterBox);
        r->erasing.push(firsts.letterBox);
    }
    firsts.letterBox = kPrLayoutNodeNone;
    firsts.letterSource = kPrLayoutNodeNone;
    firsts.letterFull.clear();
}

// Forgets the ::first-line and ::first-letter styles at `i` of CssPresentation::firsts. If `boxes` is set, also takes
// back what they gave the boxes. `boxes` is false once the tree is gone.
void dropFirsts(PrCssPresentation* r, uint32_t i, bool boxes = true) {
    CssPresentationFirsts& firsts = r->firsts.data[i];
    if (boxes)
        clearFirsts(r, firsts);
    firsts.lined.reset();
    firsts.letterFull.reset();
    cssComputedStyleRelease(firsts.line);
    cssComputedStyleRelease(firsts.letter);
    r->firstsIndex.erase(firstsHash(firsts.element), i);
    uint32_t last = r->firsts.count - 1;
    if (i != last) {
        r->firstsIndex.relocate(firstsHash(r->firsts.data[last].element), last, i);
        firsts = r->firsts.data[last];
    }
    r->firsts.pop();
}

// Frees an entry.
void dropEntry(PrCssPresentation* r, uint32_t at) {
    CssPresentationEntry& entry = r->entries.data[at];
    if (entry.originates)
        dropFirsts(r, firstsOf(r, static_cast<const PrDomElement*>(entry.node)));
    // The counters of the nodes after a participant change once it is gone, and a change listed at it has no place.
    if (entry.counts || entry.countsBelow || entry.counterChanged) {
        cssCountersChanged(r, nullptr);
        r->changed = true;
    }
    // An element's pseudo-elements go with it.
    for (CssPresentationPseudo* pseudo : entry.pseudos) {
        if (pseudo == nullptr)
            continue;
        uint32_t own = cssPresentationNodeEntry(r, pseudo);
        if (own != kHashMissing)
            dropEntry(r, own);
        r->retired.push(pseudo);
    }
    for (uint32_t i = 0; i < r->svgs.count; ++i) {
        if (r->svgs.data[i]->element == entry.node) {
            r->retiredSvgs.push(r->svgs.data[i]);
            r->svgs.data[i] = r->svgs.last();
            r->svgs.pop();
            break;
        }
    }
    for (uint32_t i = 0; i < r->referencing.count; ++i) {
        if (r->referencing.data[i] == entry.node) {
            r->referencing.data[i] = r->referencing.last();
            r->referencing.pop();
            break;
        }
    }
    if ((entry.ops & CssPresentationBoxOp_Queued) != 0) {
        domNodeRetain(const_cast<PrDomNode*>(entry.node));
        r->boxQueueDropped.push(entry.node);
    }
    entry.node->presentationEntry = 0;
    if (entry.animated)
        cssAnimateForget(r, entry.node);
    cssComputedStyleRelease(entry.style);
    cssComputedStyleRelease(entry.base);
    entry.matched.reset();
    cssCascadeMemoReset(&entry.memo);
    entry = CssPresentationEntry{};
    r->free.push(at);
}

// Erases the box of `node`, which holds the boxes of its descendants. If `node` is an element of display contents,
// which has no box, erases the boxes of its children in turn.
void eraseBoxes(PrCssPresentation* r, PrDomNode* node) {
    uint32_t at = cssPresentationNodeEntry(r, node);
    if (at == kHashMissing)
        return;
    if (!r->entries.data[at].contents) {
        eraseBox(r, at);
        return;
    }
    DomFlatChildren children = domFlatChildren(node);
    while (PrDomNode* child = domFlatNext(children))
        eraseBoxes(r, child);
    // The boxes of its pseudo-elements are among its children's.
    for (CssPresentationPseudo* pseudo : r->entries.data[at].pseudos) {
        uint32_t own = pseudo != nullptr ? cssPresentationNodeEntry(r, pseudo) : kHashMissing;
        if (own != kHashMissing)
            eraseBox(r, own);
    }
}

// Drops the entries of `node` and its shadow-including descendants, each of which the flat tree may hold.
void dropEntries(PrCssPresentation* r, PrDomNode* node) {
    bool shadows = node->document->slots != nullptr;
    for (PrDomNode* n = node; n != nullptr; n = shadows ? domShadowIncludingNext(node, n) : domNodeNext(node, n)) {
        uint32_t entry = cssPresentationNodeEntry(r, n);
        if (entry != kHashMissing)
            dropEntry(r, entry);
        n->pending = 0;
    }
}

// Forgets `node` and its descendants, as they leave the document or the flat tree. Erases the boxes of `node`, which
// hold the boxes of its descendants, and drops every entry.
void forget(PrCssPresentation* r, PrDomNode* node) {
    eraseBoxes(r, node);
    dropEntries(r, node);
}

// Marks the work that the next update redoes for `node`, and marks its ancestors as having a descendant to visit.
void mark(PrDomNode* node, DomStyleFlags flags) {
    node->pending |= flags;
    bool shadows = node->document->slots != nullptr;
    for (PrDomNode* at = shadows ? domFlatParentShadowed(node) : node->parent; at != nullptr;
            at = shadows ? domFlatParentShadowed(at) : at->parent) {
        if ((at->pending & DomStyleFlag_Descendant) != 0)
            break;
        at->pending |= DomStyleFlag_Descendant;
    }
}

// Returns the first of the element's pending invalidations, or kHashMissing. `hash` is the hash of the element's
// pointer.
uint32_t firstInvalidation(const PrCssPresentation* r, const PrDomElement* element, uint32_t hash) {
    return r->invalidationIndex.find(hash, [&](uint32_t i) { return r->invalidations.data[i].element == element; });
}

// Adds `features` to the features by which the descendants of `element` are restyled at the next update.
void addFeatures(PrCssPresentation* r, PrDomElement* element, const Array<uint32_t>& features) {
    uint32_t hash = prHashPointer(element, 0);
    uint32_t head = firstInvalidation(r, element, hash);
    for (uint32_t f = 0; f < features.count; ++f) {
        uint32_t feature = features.data[f];
        bool known = false;
        for (uint32_t i = head; i != kHashMissing && !known; i = r->invalidations.data[i].next)
            known = r->invalidations.data[i].feature == feature;
        if (known)
            continue;
        uint32_t at = r->invalidations.count;
        if (head == kHashMissing) {
            r->invalidations.push(CssPresentationInvalidation{element, feature, kHashMissing});
            r->invalidationIndex.insert(hash, at);
            head = at;
        } else {
            r->invalidations.push(CssPresentationInvalidation{element, feature, r->invalidations.data[head].next});
            r->invalidations.data[head].next = at;
        }
    }
    mark(element, DomStyleFlag_Features);
}

// Marks the elements that a change to the style of `element` reaches: the element, its siblings, its subtree, or the
// descendants that carry one of `descendants`.
void restyleReach(PrCssPresentation* r, PrDomElement* element, bool itself, bool laterSiblings, bool siblings,
        bool subtree, const Array<uint32_t>& descendants) {
    r->changed = true;
    if (itself)
        mark(element, DomStyleFlag_Self);
    if (subtree)
        mark(element, DomStyleFlag_Subtree);
    else if (descendants.count != 0)
        addFeatures(r, element, descendants);
    if (siblings && element->parent != nullptr) {
        mark(element->parent, DomStyleFlag_Subtree);
    } else if (laterSiblings) {
        for (PrDomNode* sibling = element->next; sibling != nullptr; sibling = sibling->next) {
            if (domElementFromNode(sibling) != nullptr)
                mark(sibling, DomStyleFlag_Self | DomStyleFlag_Subtree);
        }
    }
}

// Returns true if the element carries one of `features`: an ID, a class, its type, or an attribute's name.
bool carries(const PrDomElement* element, const uint32_t* features, uint32_t count) {
    auto any = [&](uint32_t hash) {
        for (uint32_t i = 0; i < count; ++i) {
            if (features[i] == hash)
                return true;
        }
        return false;
    };
    if (any(cssFeatureHash(CssFeature_Type, domElementName(element))))
        return true;
    PrStringView id = domElementId(element);
    if (id.size != 0 && any(cssFeatureHash(CssFeature_Id, id)))
        return true;
    if (element->classes != kDomClassesNone) {
        DomClassList classes = domClassesList(element->document, element->classes);
        for (uint32_t i = 0; i < classes.count; ++i) {
            if (any(cssFeatureHash(CssFeature_Class, domClassesName(element->document, classes.atoms[i]))))
                return true;
        }
    }
    for (const PrDomAttr* a = element->firstAttribute; a != nullptr; a = static_cast<const PrDomAttr*>(a->next)) {
        if (any(cssFeatureHash(CssFeature_Attribute, prStringView(&a->name))))
            return true;
    }
    return false;
}

// Writes the features that the element carries to `out`, each a cssFeatureHash: its type, its ID, its classes, and its
// attributes' names.
void featuresOf(const PrDomElement* element, Array<uint32_t>* out) {
    out->clear();
    out->push(cssFeatureHash(CssFeature_Type, domElementName(element)));
    PrStringView id = domElementId(element);
    if (id.size != 0)
        out->push(cssFeatureHash(CssFeature_Id, id));
    if (element->classes != kDomClassesNone) {
        DomClassList classes = domClassesList(element->document, element->classes);
        for (uint32_t i = 0; i < classes.count; ++i)
            out->push(cssFeatureHash(CssFeature_Class, domClassesName(element->document, classes.atoms[i])));
    }
    for (const PrDomAttr* a = element->firstAttribute; a != nullptr; a = static_cast<const PrDomAttr*>(a->next))
        out->push(cssFeatureHash(CssFeature_Attribute, prStringView(&a->name)));
}

// Marks the anchors whose :has() arguments a change at `node` can make match or stop matching. These are the ancestors
// of `node` that carry an anchor's feature and, if an argument reaches siblings, the earlier siblings of `node` and of
// its ancestors. Each candidate finds the anchors that it can be by its own features (CssHasAnchors).
//
// An earlier sibling needs no work if it already waits to restyle with its subtree, as does every sibling after it, and
// no anchor whose argument reaches later siblings also reaches every sibling. Its anchors would restyle only the
// sibling, its subtree and its later siblings, which all restyle anyway. A parser that appends a page's elements meets
// such siblings at every level, and so does not walk them at the cost of finding their anchors.
void invalidateAnchors(PrCssPresentation* r, PrDomNode* node, const CssHasAnchors& anchors) {
    constexpr DomStyleFlags kWhole = DomStyleFlag_Self | DomStyleFlag_Subtree;
    bool previous = anchors.previous.count != 0;
    bool everySibling = false;
    for (uint32_t i = 0; i < anchors.previous.count; ++i)
        everySibling = everySibling || anchors.anchors.data[anchors.previous.data[i]].siblings;
    Array<uint32_t> features;
    Array<uint32_t> descendants;
    auto restyle = [&](PrDomElement* candidate, bool sibling) {
        featuresOf(candidate, &features);
        cssHasAnchorsEach(anchors, features.data, features.count, sibling, [&](const CssHasAnchor& a) {
            descendants.clear();
            if (a.descendant != 0)
                descendants.push(a.descendant);
            restyleReach(r, candidate, a.self, a.laterSiblings, a.siblings, a.subtree, descendants);
        });
    };
    for (PrDomNode* at = node; at != nullptr; at = at->parent) {
        bool covered = !everySibling && at->next == nullptr && (at->pending & kWhole) == kWhole;
        for (PrDomNode* sibling = previous ? at->previous : nullptr; sibling != nullptr; sibling = sibling->previous) {
            covered = covered && (sibling->pending & kWhole) == kWhole;
            if (covered)
                continue;
            if (PrDomElement* e = domElementFromNode(sibling))
                restyle(e, true);
        }
        if (at != node) {
            if (PrDomElement* e = domElementFromNode(at))
                restyle(e, false);
        }
    }
    features.reset();
    descendants.reset();
}

// Marks the elements that a change to the feature of `element` that hashes to `hash` restyles, according to the rule
// set's invalidation for the feature.
void invalidate(PrCssPresentation* r, PrDomElement* element, uint32_t hash) {
    for (const CssInvalidation* invalidation = cssRuleSetInvalidation(&r->rules, hash); invalidation != nullptr;
            invalidation = invalidation->next != kHashMissing ? &r->rules.invalidations.data[invalidation->next] : nullptr) {
        if (invalidation->qualifier != 0 && !carries(element, &invalidation->qualifier, 1))
            continue;
        restyleReach(r, element, invalidation->self, invalidation->laterSiblings, invalidation->siblings,
            invalidation->subtree, invalidation->descendants);
        if (invalidation->anchors.anchors.count != 0)
            invalidateAnchors(r, element, invalidation->anchors);
    }
}

// Returns true if the element's direction is auto, which means that the text it holds decides it
// (html/#the-directionality).
bool automaticDirection(const PrDomElement* element) {
    PrStringView dir = domAttributeValue(element, "dir");
    return prAsciiEqualsFold(dir, "auto") ||
        (dir.data == nullptr && (cssBoxTraits(element).flags & CssBoxTraitFlag_AutoDirection) != 0);
}

// Restyles what :dir() reads after a change to the text or children under `node`. A change can affect the direction of
// the nearest enclosing element whose direction is auto, which the text it holds decides. If the element's entry noted
// its directionality, the element is queued instead, and the next restyle restyles what reads it only if the
// directionality changed (queuedDirections).
void invalidateDirection(PrCssPresentation* r, PrDomNode* node) {
    for (PrDomNode* at = node->parent; at != nullptr; at = at->parent) {
        PrDomElement* element = domElementFromNode(at);
        if (element == nullptr)
            continue;
        if (automaticDirection(element)) {
            uint32_t entry = cssPresentationNodeEntry(r, element);
            if (entry == kHashMissing || r->entries.data[entry].direction == CssPresentationDirection_Unknown) {
                invalidate(r, element, cssFeatureHash(CssFeature_Attribute, "dir"));
            } else if (!r->entries.data[entry].directionQueued) {
                r->entries.data[entry].directionQueued = true;
                r->directionQueue.push(entry);
                r->changed = true;
            }
            return;
        }
        // An element with a direction of its own ends the text that decides the direction of its ancestors.
        if (domAttributeValue(element, "dir").data != nullptr)
            return;
    }
}

// Restyles what :dir() reads under each queued element whose directionality differs from the one that it was last
// styled with. An entry that has since been dropped is no longer queued.
void queuedDirections(PrCssPresentation* r, bool all) {
    for (uint32_t i = 0; i < r->directionQueue.count; ++i) {
        CssPresentationEntry& entry = r->entries.data[r->directionQueue.data[i]];
        if (!entry.directionQueued)
            continue;
        entry.directionQueued = false;
        PrDomElement* element = const_cast<PrDomElement*>(domElementFromNode(entry.node));
        bool rtl = domElementRtl(element);
        if (!all && (entry.direction == CssPresentationDirection_Rtl) != rtl)
            invalidate(r, element, cssFeatureHash(CssFeature_Attribute, "dir"));
    }
    r->directionQueue.clear();
}

// Returns true if a change to the tree should be passed to the rule set's invalidations. Returns false if every element
// is restyled anyway. Also returns false if a selector can reach any element from the change, in which case every
// element is restyled.
bool invalidates(PrCssPresentation* r) {
    if (r->restyleAll)
        return false;
    if (r->rules.reads.global) {
        cssPresentationChanged(r);
        return false;
    }
    return true;
}

// Returns true if an attribute can decide a state that a pseudo-class reads (html/#pseudo-classes). Classes, IDs,
// inline style, and data and ARIA attributes decide none.
bool decidesStates(PrStringView name) {
    if (name == "class" || name == "id" || name == "style")
        return false;
    bool data = name.size > 5 && prAsciiStartsWithFold(name, "data-");
    bool aria = name.size > 5 && prAsciiStartsWithFold(name, "aria-");
    return !data && !aria;
}

// Calls `visit` with each of the whitespace-separated tokens of a class attribute's value.
template <typename Visit>
void eachClass(PrStringView value, Visit visit) {
    size_t i = 0;
    while (i < value.size) {
        while (i < value.size && prAsciiIsSpace(value.data[i]))
            ++i;
        size_t start = i;
        while (i < value.size && !prAsciiIsSpace(value.data[i]))
            ++i;
        if (i != start)
            visit(PrStringView{value.data + start, i - start});
    }
}

bool hasClass(PrStringView value, PrStringView name) {
    bool found = false;
    eachClass(value, [&](PrStringView token) { found = found || token == name; });
    return found;
}

// Returns true if a style read whether `parent` has children (:empty). A parent with no entry is assumed to have been
// read.
bool readsEmpty(const PrCssPresentation* r, const PrDomNode* parent) {
    uint32_t at = cssPresentationNodeEntry(r, parent);
    return at == kHashMissing || (r->entries.data[at].structure & DomStructureFlag_Empty) != 0;
}

// Marks `child`, in its place among the children of its parent, as added or about to be removed. If styles read the
// structure of the children, the change also reaches the parent and the other children.
void markChildren(PrCssPresentation* r, PrDomNode* child) {
    PrDomNode* parent = child->parent;
    r->changed = true;
    if (r->rules.reads.global) {
        cssPresentationChanged(r);
        return;
    }
    mark(parent, DomStyleFlag_Children);
    if (!r->rules.reads.structure)
        return;
    // A parent with no entry, which is not an element, is assumed to have had every kind of structure read.
    uint32_t at = cssPresentationNodeEntry(r, parent);
    DomStructureFlags read = at != kHashMissing ? r->entries.data[at].structure : DomStructureFlags(0xff);
    if (read == 0)
        return;
    if ((read & DomStructureFlag_Empty) != 0)
        mark(parent, DomStyleFlag_Self);
    // Restyling a sibling also restyles its subtree if a selector read the structure from below the sibling.
    DomStyleFlags flags = DomStyleFlag_Self;
    if ((read & DomStructureFlag_Descendants) != 0)
        flags |= DomStyleFlag_Subtree;
    auto restyle = [&](PrDomNode* PR_NULLABLE from, bool after, bool all) {
        for (PrDomNode* at = from; at != nullptr; at = after ? at->next : at->previous) {
            if (at->type != PrDomNodeType_Element)
                continue;
            mark(at, flags);
            if (!all)
                return;
        }
    };
    if ((read & (DomStructureFlag_Next | DomStructureFlag_Forward)) != 0)
        restyle(child->next, true, (read & DomStructureFlag_Forward) != 0);
    if ((read & (DomStructureFlag_Previous | DomStructureFlag_Backward)) != 0)
        restyle(child->previous, false, (read & DomStructureFlag_Backward) != 0);
}

// Adds the features by which the descendants of `element` are restyled to the update's features.
void pushFeatures(PrCssPresentation* r, const PrDomElement* element) {
    uint32_t hash = prHashPointer(element, 0);
    uint32_t head = firstInvalidation(r, element, hash);
    for (uint32_t i = head; i != kHashMissing; i = r->invalidations.data[i].next)
        r->features.push(r->invalidations.data[i].feature);
}

// Returns true if two styles have the same groups, locale, used appearance and flags, given whether they have the same
// custom properties.
bool sameStyle(const CssComputedStyle* a, const CssComputedStyle* b, bool sameCustom) {
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g) {
        if (a->groups[g] != b->groups[g])
            return false;
    }
    return sameCustom && a->locale == b->locale && a->appearance == b->appearance &&
        a->staticInline == b->staticInline && a->childrenAreItems == b->childrenAreItems;
}

// How a parent's style changed. This decides which of its children are restyled under it.
enum {
    ParentChange_None,
    ParentChange_Reset,             // only properties that do not inherit, which a child takes if it asks to
    ParentChange_Inherited,         // properties that inherit, which every child takes
};
typedef uint8_t ParentChange;

// Returns how a parent's style changed from `old` to `style`. `sameCustom` says whether the styles have the same custom
// properties.
ParentChange changeOf(const CssComputedStyle* PR_NULLABLE old, const CssComputedStyle* style, bool sameCustom) {
    // Whether the children are items decides their display when their own styles compute it.
    if (old == nullptr || !sameCustom || old->locale != style->locale ||
            old->childrenAreItems != style->childrenAreItems)
        return ParentChange_Inherited;
    ParentChange change = ParentChange_None;
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g) {
        if (old->groups[g] == style->groups[g])
            continue;
        if (cssStyleGroupInherited(CssStyleGroup(g)))
            return ParentChange_Inherited;
        change = ParentChange_Reset;
    }
    return change;
}

// Returns the viewport that styles are computed in: the presentation's extent.
CssViewport viewportOf(const PrCssPresentation* r) {
    return CssViewport{prLayoutUnitToPx(r->extent.x), prLayoutUnitToPx(r->extent.y)};
}

// The minimum number of elements that a restyle gives each thread that it wakes, so that the cost of waking a thread is
// small compared to its share.
constexpr uint32_t kPrefetchPerThread = 256;

// Creates the state with which each of the agent's restyle threads computes the presentation's styles, the first time a
// restyle asks for it.
void startThreads(PrCssPresentation* r) {
    if (r->threads.count != 0)
        return;
    uint32_t count = domAgentRestyleWidth(r->agent);
    for (uint32_t i = 0; i < count; ++i) {
        CssPresentationThread& thread = r->threads.emplace();
        thread = CssPresentationThread{};
        thread.builder.table = &r->styles;
        thread.builder.measure = &r->fontMeasure;
    }
}

// The pseudo-elements that may generate a box, before cssCascadeGenerating finds them.
constexpr uint32_t kGeneratingUnknown = UINT32_MAX;

// Records the entries that the scratch's last style was computed from as the matches of the element at `at`.
void keepMatches(PrCssPresentation* r, uint32_t at, const CssCascadeScratch& scratch) {
    CssPresentationEntry& entry = r->entries.data[at];
    entry.matched.clear();
    entry.matched.append(scratch.candidates.data, scratch.candidates.count);
    entry.matchedGeneration = r->rules.generation;
    entry.generating = kGeneratingUnknown;
}

// Returns what the element at `at` matched in the current rule set when its style was last computed, or null.
const Array<uint32_t>* PR_NULLABLE cachedMatches(const PrCssPresentation* r, uint32_t at) {
    const CssPresentationEntry& entry = r->entries.data[at];
    bool current = entry.matchedGeneration != 0 && entry.matchedGeneration == r->rules.generation;
    return current ? &entry.matched : nullptr;
}

// Returns the element's computed style, computed on the presentation's own thread. If `matched` is not null, the style
// is computed from what the element matched last. A fresh style is computed from the declarations, bypassing the
// matched properties cache and the element's memo, which verification needs. With the element's entry `at`, the style
// may come from the entry's memo, which the cascade updates.
const CssComputedStyle* cascade(PrCssPresentation* r, PrDomElement* element, const CssComputedStyle* PR_NULLABLE parent,
        CssFontBases rootFont, bool fresh = false, const Array<uint32_t>* PR_NULLABLE matched = nullptr,
        uint32_t at = kHashMissing) {
    cssStylePrepare(element);
    startThreads(r);
    CssCascadeInput input{
        .element = element,
        .parent = parent,
        .rootFont = rootFont,
        .viewport = viewportOf(r),
        .fresh = fresh,
        .matched = matched,
        .pseudo = CssPseudo_None,
    };
    if (at != kHashMissing && !fresh)
        input.memo = &r->entries.data[at].memo;
    return cssCascadeStyle(&r->rules, &r->threads.data[0].builder, &r->threads.data[0].scratch, input);
}

// Returns the style of the ::first-line or ::first-letter `pseudo` of `element` under `parent`. The caller owns a
// reference to the style.
const CssComputedStyle* cascadeFirst(PrCssPresentation* r, const PrDomElement* element, CssPseudo pseudo,
        const CssComputedStyle* PR_NULLABLE parent) {
    startThreads(r);
    CssCascadeInput input{
        .element = element,
        .parent = parent,
        .rootFont = r->rootFont,
        .viewport = viewportOf(r),
        .fresh = false,
        .matched = nullptr,
        .pseudo = pseudo,
    };
    return cssCascadeStyle(&r->rules, &r->threads.data[0].builder, &r->threads.data[0].scratch, input);
}

// Returns true if a rule of the presentation's set may style a ::first-line or ::first-letter.
bool stylesFirsts(const PrCssPresentation* r) {
    const CssRuleIndex& line = r->rules.filed[CssPseudo_FirstLine];
    const CssRuleIndex& letter = r->rules.filed[CssPseudo_FirstLetter];
    return line.universal.count != 0 || line.buckets.count != 0 || letter.universal.count != 0 || letter.buckets.count != 0;
}

// Brings the ::first-line and ::first-letter styles of the element at `at` up to date under `style`, its style as
// presented. A change to them is noted for the SVG content that reads them. It also marks the element's boxes, so that
// applyFirsts gives them the new styles.
void restyleFirsts(PrCssPresentation* r, uint32_t at, PrDomElement* element, const CssComputedStyle* style) {
    startThreads(r);
    uint32_t originating = cssCascadeOriginating(&r->rules, &r->threads.data[0].scratch, element);
    const CssComputedStyle* line = nullptr;
    if ((originating & (1u << CssPseudo_FirstLine)) != 0)
        line = cascadeFirst(r, element, CssPseudo_FirstLine, style);
    const CssComputedStyle* letter = nullptr;
    if ((originating & (1u << CssPseudo_FirstLetter)) != 0)
        letter = cascadeFirst(r, element, CssPseudo_FirstLetter, line != nullptr ? line : style);
    uint32_t i = r->entries.data[at].originates ? firstsOf(r, element) : kHashMissing;
    auto same = [](const CssComputedStyle* PR_NULLABLE a, const CssComputedStyle* PR_NULLABLE b) {
        return a == b ||
            (a != nullptr && b != nullptr && sameStyle(a, b, cssCustomPropertiesEqual(a->custom, b->custom)));
    };
    const CssComputedStyle* oldLine = i != kHashMissing ? r->firsts.data[i].line : nullptr;
    const CssComputedStyle* oldLetter = i != kHashMissing ? r->firsts.data[i].letter : nullptr;
    bool changed = !same(oldLine, line) || !same(oldLetter, letter);
    bool originates = line != nullptr || letter != nullptr;
    if (i != kHashMissing && originates) {
        cssComputedStyleRelease(oldLine);
        cssComputedStyleRelease(oldLetter);
        r->firsts.data[i].line = line;
        r->firsts.data[i].letter = letter;
        r->firsts.data[i].stale = true;
    } else if (i != kHashMissing) {
        dropFirsts(r, i);
    } else if (originates) {
        r->firstsIndex.insert(firstsHash(element), r->firsts.count);
        r->firsts.push(CssPresentationFirsts{
            .element = element,
            .line = line,
            .letter = letter,
            .stale = true,
            .letterBox = kPrLayoutNodeNone,
            .letterSource = kPrLayoutNodeNone,
        });
    }
    r->entries.data[at].originates = originates;
    if (changed)
        svgNote(r, element, CssPresentationSvgNoteKind_Style);
}

// Marks the ::first-line and ::first-letter of `node` and of each of its ancestors stale, because something that their
// boxes depend on changed at `node` (CssPresentationFirsts::stale). Costs nothing on a page where no element originates
// one.
void staleFirsts(PrCssPresentation* r, const PrDomNode* node) {
    if (r->firsts.count == 0)
        return;
    for (const PrDomNode* at = node; at != nullptr; at = domFlatParent(at)) {
        const PrDomElement* element = domElementFromNode(at);
        uint32_t entry = element != nullptr ? cssPresentationNodeEntry(r, element) : kHashMissing;
        if (entry == kHashMissing || !r->entries.data[entry].originates)
            continue;
        uint32_t i = firstsOf(r, element);
        if (i != kHashMissing)
            r->firsts.data[i].stale = true;
    }
}

// How a box takes part in the first formatted line of its block container (css-pseudo-4/#first-text-line).
enum {
    FirstFlow_Skip,                 // out of flow, a marker, or no box: neither on the line nor in its way
    FirstFlow_Inline,               // an inline box, whose content is on the line
    FirstFlow_Atomic,               // an atomic inline: content of the line that holds no text of it
    FirstFlow_Block,                // a block-level box in flow, whose first line may be the container's
};
typedef uint8_t FirstFlow;

// Returns the computed style of the node that the box `box` presents, or null for an anonymous box.
const CssComputedStyle* PR_NULLABLE presentedStyle(const PrCssPresentation* r, PrLayoutNode box) {
    PrDomNode* node = presented(r, box);
    uint32_t at = node != nullptr ? cssPresentationNodeEntry(r, node) : kHashMissing;
    return at != kHashMissing ? r->entries.data[at].style : nullptr;
}

// Returns how the element box `box` takes part in the first formatted line of the block container that holds it.
FirstFlow firstFlowOf(const PrCssPresentation* r, PrLayoutNode box) {
    PrDomNode* node = presented(r, box);
    const CssComputedStyle* style = presentedStyle(r, box);
    if (style == nullptr || (isPseudo(node) && cssPseudoIsMarker(static_cast<CssPresentationPseudo*>(node)->pseudo)))
        return FirstFlow_Skip;
    PrCssKeyword position = cssComputedFirstKeyword(style, PrCssProperty_Position);
    if (cssComputedFirstKeyword(style, PrCssProperty_Float) != PrCssKeyword_None ||
            position == PrCssKeyword_Absolute || position == PrCssKeyword_Fixed)
        return FirstFlow_Skip;
    PrLayoutNodeKind kind = prLayoutNodeKind(r->tree, box);
    if (kind != PrLayoutNodeKind_Element)
        return FirstFlow_Atomic;
    switch (cssComputedFirstKeyword(style, PrCssProperty_Display)) {
        case PrCssKeyword_Inline: return FirstFlow_Inline;
        case PrCssKeyword_InlineBlock:
        case PrCssKeyword_InlineFlex:
        case PrCssKeyword_InlineGrid:
        case PrCssKeyword_InlineTable: return FirstFlow_Atomic;
        default: return FirstFlow_Block;
    }
}

// Returns true if the computed style `style` makes a block container, which ::first-line and ::first-letter apply to
// (css-pseudo-4/#first-text-line): one that lays out its content in lines, not a flex, grid or table container.
bool isBlockContainer(const CssComputedStyle* style) {
    switch (cssComputedFirstKeyword(style, PrCssProperty_Display)) {
        case PrCssKeyword_Block:
        case PrCssKeyword_ListItem:
        case PrCssKeyword_InlineBlock:
        case PrCssKeyword_FlowRoot:
        case PrCssKeyword_TableCell:
        case PrCssKeyword_TableCaption: return true;
        default: return false;
    }
}

// Returns true if `text` holds only white space, which collapses away at the start of a line.
bool onlySpace(PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        if (!prAsciiIsSpace(text.data[i]))
            return false;
    }
    return true;
}

// A box on the first formatted line, and its style there, which the presentation holds a reference to while applyFirsts
// runs.
struct LinedBox {
    PrLayoutNode box;
    const CssComputedStyle* style;
};

// Returns the style of the box `box` on the first formatted line. The style inherits from `parent`, the style of the
// box's parent on that line. Custom properties inherit from `own`, the parent's own style, instead
// (css-pseudo-4/#first-line-inheritance). The caller owns a reference to the style. The box presents an element or a
// ::before or ::after.
const CssComputedStyle* lineStyleOf(PrCssPresentation* r, PrLayoutNode box, const CssComputedStyle* parent,
        const CssComputedStyle* PR_NULLABLE own) {
    PrDomNode* node = presented(r, box);
    bool pseudo = isPseudo(node);
    startThreads(r);
    CssComputedStyle mixed{};
    if (own != nullptr && own->custom != parent->custom) {
        for (uint32_t g = 0; g < CssStyleGroup_Count; ++g)
            mixed.groups[g] = parent->groups[g];
        mixed.custom = own->custom;
        mixed.important = parent->important;
        mixed.locale = parent->locale;
        mixed.appearance = parent->appearance;
        mixed.inheritsReset = parent->inheritsReset;
        mixed.staticInline = parent->staticInline;
        mixed.childrenAreItems = parent->childrenAreItems;
        mixed.font = parent->font;
        parent = &mixed;
    }
    CssCascadeInput input{
        .element = static_cast<const PrDomElement*>(pseudo ? node->parent : node),
        .parent = parent,
        .rootFont = r->rootFont,
        .viewport = viewportOf(r),
        .fresh = false,
        .matched = nullptr,
        .pseudo = pseudo ? static_cast<CssPresentationPseudo*>(node)->pseudo : CssPseudo(CssPseudo_None),
    };
    return cssCascadeStyle(&r->rules, &r->threads.data[0].builder, &r->threads.data[0].scratch, input);
}

// Gives the box `box` and the boxes on its first formatted line their records for that line: `style` for the box
// itself, and for each inline box in it, its style under its parent's style there. If only white space and boxes out of
// flow come before a block-level box in flow, the first formatted line is that box's (css-pseudo-4/#first-text-line).
// That box takes the line's styles the same way, with its own ::first-line nested inside the line's. Appends each box
// and its style there to `lined`.
void lineRecords(PrCssPresentation* r, CssPresentationFirsts& firsts, PrLayoutNode box, const CssComputedStyle* style,
        Array<LinedBox>& lined, bool container) {
    bool reads = false;
    prLayoutNodeSetFirstLineStyle(r->tree, box, recordOf(r, presented(r, box), style, false, &reads));
    r->readsOutside = r->readsOutside || reads;
    firsts.lined.push(box);
    cssComputedStyleRetain(style);
    lined.push(LinedBox{box, style});
    bool before = false;
    for (PrLayoutNode child = prLayoutNodeFirstChild(r->tree, box); child != kPrLayoutNodeNone;
            child = prLayoutNodeNextSibling(r->tree, child)) {
        PrLayoutNodeKind kind = prLayoutNodeKind(r->tree, child);
        if (child == firsts.letterBox)
            continue;
        if (kind == PrLayoutNodeKind_Text) {
            before = before || !onlySpace(prLayoutNodeText(r->tree, child));
            continue;
        }
        if (kind == PrLayoutNodeKind_LineBreak) {
            before = true;
            continue;
        }
        FirstFlow flow = firstFlowOf(r, child);
        if (flow == FirstFlow_Skip)
            continue;
        if (flow == FirstFlow_Atomic) {
            before = true;
            continue;
        }
        if (flow == FirstFlow_Inline) {
            const CssComputedStyle* inner = lineStyleOf(r, child, style, presentedStyle(r, box));
            lineRecords(r, firsts, child, inner, lined, false);
            cssComputedStyleRelease(inner);
            before = true;
            continue;
        }
        // A block-level box: the line is its own if nothing in flow comes before it in a block container.
        if (!container || before)
            break;
        const CssComputedStyle* inner = lineStyleOf(r, child, style, presentedStyle(r, box));
        const CssComputedStyle* own = inner;
        PrDomNode* node = presented(r, child);
        const PrDomElement* element = domElementFromNode(node);
        if (element != nullptr && isBlockContainer(inner)) {
            if (const CssComputedStyle* nested = cssPresentationPseudoStyleUnder(r, element, CssPseudo_FirstLine, inner))
                own = nested;
        }
        if (isBlockContainer(inner))
            lineRecords(r, firsts, child, own, lined, true);
        if (own != inner)
            cssComputedStyleRelease(own);
        cssComputedStyleRelease(inner);
        break;
    }
}

// The first-letter text that a search found: the text box that holds it, its text, and the letter's bytes in it.
struct FirstLetterText {
    PrLayoutNode box;
    PrStringView text;
    size_t start;
    size_t end;
};

// How the search for the first-letter text in a box ended.
enum {
    LetterSearch_Continue,          // nothing in the box keeps the search from going on past it
    LetterSearch_Found,
    LetterSearch_None,              // other content of the line comes first, so there is no first letter
};
typedef uint8_t LetterSearch;

// Searches the box `box` for the first-letter text of the first formatted line
// (css-pseudo-4/#first-letter-application): the first text in flow, unless an atomic inline or a line break comes
// first. Marker boxes, boxes out of flow and the anonymous boxes of letters already taken are passed over.
LetterSearch findLetter(const PrCssPresentation* r, PrLayoutNode box, bool container, FirstLetterText* out) {
    for (PrLayoutNode child = prLayoutNodeFirstChild(r->tree, box); child != kPrLayoutNodeNone;
            child = prLayoutNodeNextSibling(r->tree, child)) {
        PrLayoutNodeKind kind = prLayoutNodeKind(r->tree, child);
        if (kind == PrLayoutNodeKind_Text) {
            // A text node of the document holds its data. A text box of generated content holds its own text, or the
            // text it held before a letter was taken from it.
            PrDomNode* node = presented(r, child);
            bool data = node != nullptr && domIsText(node);
            PrStringView text;
            if (data)
                text = prStringView(&static_cast<PrDomCharacterData*>(node)->data);
            else
                text = prLayoutNodeText(r->tree, child);
            for (uint32_t k = 0; k < r->firsts.count && !data; ++k) {
                const CssPresentationFirsts& taken = r->firsts.data[k];
                if (taken.letterSource == child)
                    text = PrStringView{taken.letterFull.data, taken.letterFull.count};
            }
            if (onlySpace(text))
                continue;
            size_t skip = 0;
            while (skip < text.size && prAsciiIsSpace(text.data[skip]))
                ++skip;
            size_t start = 0;
            size_t end = 0;
            if (!prTextFirstLetter(PrStringView{text.data + skip, text.size - skip}, &start, &end))
                return LetterSearch_None;
            *out = FirstLetterText{child, text, skip + start, skip + end};
            return LetterSearch_Found;
        }
        if (kind == PrLayoutNodeKind_LineBreak)
            return LetterSearch_None;
        FirstFlow flow = firstFlowOf(r, child);
        if (flow == FirstFlow_Skip)
            continue;
        if (flow == FirstFlow_Atomic)
            return LetterSearch_None;
        if (flow == FirstFlow_Inline) {
            LetterSearch found = findLetter(r, child, false, out);
            if (found != LetterSearch_Continue)
                return found;
            continue;
        }
        if (!container)
            return LetterSearch_None;
        const CssComputedStyle* style = presentedStyle(r, child);
        if (style == nullptr || !isBlockContainer(style))
            return LetterSearch_None;
        LetterSearch found = findLetter(r, child, true, out);
        return found == LetterSearch_Continue ? LetterSearch_None : found;
    }
    return LetterSearch_Continue;
}

// Returns true if the box of the first letter of `firsts` is still where applyFirsts put it: before the text box it was
// taken from, which still holds the text after the letter.
bool letterHolds(const PrCssPresentation* r, const CssPresentationFirsts& firsts) {
    if (firsts.letterBox == kPrLayoutNodeNone)
        return true;
    if (!prLayoutNodeIsValid(r->tree, firsts.letterBox) || !prLayoutNodeIsValid(r->tree, firsts.letterSource))
        return false;
    PrStringView rest{firsts.letterFull.data + firsts.letterEnd, firsts.letterFull.count - firsts.letterEnd};
    return prLayoutNodeParent(r->tree, firsts.letterBox) == prLayoutNodeParent(r->tree, firsts.letterSource) &&
        prLayoutNodeNextSibling(r->tree, firsts.letterBox) == firsts.letterSource &&
        prLayoutNodeText(r->tree, firsts.letterSource) == rest;
}

// Returns the number of ancestors of `node` in the flat tree.
uint32_t flatDepth(const PrDomNode* node) {
    uint32_t depth = 0;
    for (const PrDomNode* at = domFlatParent(node); at != nullptr; at = domFlatParent(at))
        ++depth;
    return depth;
}

// Gives the boxes of each element that originates a ::first-line or a ::first-letter what that pseudo-element asks
// (css-pseudo-4/#first-line-pseudo, css-pseudo-4/#first-letter-pseudo), once the boxes of an update are in place. The
// boxes on the first formatted line get records for that line (lineRecords). The first-letter text gets a box of its
// own, styled by the ::first-letter, which inherits from the box that holds the text as that box is styled on the first
// line. The text box keeps the text after the letter.
//
// The pseudo-elements of an element and of its ancestors nest (css-pseudo-4/#first-text-line,
// css-pseudo-4/#first-letter-tree), so the elements are applied outermost first. If an ancestor's first line reached an
// element's box, the element keeps the records into which the ancestor's lineRecords nested the element's own
// ::first-line. If an ancestor's letter already holds an element's first-letter text, the element's letter restyles
// that letter's box, with the ancestor's letter as its parent. All of them are applied again if any has a change, or if
// a reconciliation has removed a letter's box from its place.
void applyFirsts(PrCssPresentation* r) {
    bool any = false;
    for (uint32_t i = 0; i < r->firsts.count && !any; ++i)
        any = r->firsts.data[i].stale || !letterHolds(r, r->firsts.data[i]);
    if (!any)
        return;
    Array<uint32_t> order;
    Array<uint32_t> depths;
    for (uint32_t i = 0; i < r->firsts.count; ++i) {
        CssPresentationFirsts& firsts = r->firsts.data[i];
        firsts.stale = false;
        clearFirsts(r, firsts);
        order.push(i);
        depths.push(flatDepth(firsts.element));
    }
    sortItems(order.data, order.count, [&](uint32_t a, uint32_t b) { return depths.data[a] < depths.data[b]; });
    Array<LinedBox> lined;
    // The held style of the letter's box for each entry of CssPresentation::firsts, or null.
    Array<const CssComputedStyle*> letters;
    for (uint32_t i = 0; i < r->firsts.count; ++i)
        letters.push(nullptr);
    for (uint32_t o = 0; o < order.count; ++o) {
        uint32_t i = order.data[o];
        CssPresentationFirsts& firsts = r->firsts.data[i];
        uint32_t at = cssPresentationNodeEntry(r, firsts.element);
        // A text field's text is not its content, so neither pseudo-element reaches it (csswg-drafts/issues/13926).
        if (at == kHashMissing || !r->entries.data[at].boxed || r->entries.data[at].layoutNode == kPrLayoutNodeNone ||
                r->entries.data[at].style == nullptr || !isBlockContainer(r->entries.data[at].style) ||
                isTextField(firsts.element))
            continue;
        PrLayoutNode box = r->entries.data[at].layoutNode;
        bool reached = false;
        for (uint32_t k = 0; k < lined.count && !reached; ++k)
            reached = lined.data[k].box == box;
        if (firsts.line != nullptr && !reached)
            lineRecords(r, firsts, box, firsts.line, lined, true);
        FirstLetterText letter{};
        if (firsts.letter == nullptr || findLetter(r, box, true, &letter) != LetterSearch_Found)
            continue;
        // The letter inherits from the box around its text, as styled on the first line if it has a style there, or
        // from an ancestor's letter around the same text.
        PrLayoutNode holder = prLayoutNodeParent(r->tree, letter.box);
        const CssComputedStyle* parent = presentedStyle(r, holder);
        for (uint32_t k = 0; k < lined.count; ++k) {
            if (lined.data[k].box == holder)
                parent = lined.data[k].style;
        }
        uint32_t outer = kHashMissing;
        for (uint32_t k = 0; k < r->firsts.count && outer == kHashMissing; ++k) {
            if (k != i && letters.data[k] != nullptr && r->firsts.data[k].letterSource == letter.box)
                outer = k;
        }
        if (outer != kHashMissing)
            parent = letters.data[outer];
        const CssComputedStyle* style =
            parent != nullptr ? cascadeFirst(r, firsts.element, CssPseudo_FirstLetter, parent) : nullptr;
        if (style == nullptr)
            continue;
        bool reads = false;
        PrLayoutStyleIndex record = project(r, style, Presentation{}, false, &reads);
        r->readsOutside = r->readsOutside || reads;
        if (outer != kHashMissing) {
            // The innermost letter's style is the box's.
            prLayoutNodeSetStyle(r->tree, r->firsts.data[outer].letterBox, record);
            cssComputedStyleRelease(letters.data[outer]);
            letters.data[outer] = style;
            continue;
        }
        letters.data[i] = style;
        firsts.letterFull.append(letter.text.data, uint32_t(letter.text.size));
        firsts.letterEnd = uint32_t(letter.end);
        firsts.letterSource = letter.box;
        firsts.letterBox = prLayoutNodeCreateElement(r->tree, kPrLayoutNodeNone, record);
        prLayoutNodeSetRendered(r->tree, firsts.letterBox, PrLayoutRenderedFlag_Inline);
        prLayoutNodeCreateText(r->tree, firsts.letterBox,
            PrStringView{letter.text.data + letter.start, letter.end - letter.start});
        prLayoutTreeInsert(r->tree, holder, firsts.letterBox, letter.box);
        prLayoutNodeSetText(r->tree, letter.box,
            PrStringView{letter.text.data + letter.end, letter.text.size - letter.end});
    }
    for (uint32_t k = 0; k < lined.count; ++k)
        cssComputedStyleRelease(lined.data[k].style);
    for (uint32_t k = 0; k < letters.count; ++k)
        cssComputedStyleRelease(letters.data[k]);
    lined.reset();
    letters.reset();
    order.reset();
    depths.reset();
}

// Releases the styles computed ahead that the walk did not take.
void dropPrefetch(PrCssPresentation* r) {
    for (uint32_t i = r->prefetchNext; i < r->prefetched.count; ++i)
        cssComputedStyleRelease(r->prefetched.data[i].style);
    r->prefetched.clear();
    r->prefetchNext = 0;
}

// Returns the style that a prefetch computed for `element` under `parentStyle`, or null if there is none. The walk
// takes prefetched styles in tree order. A style computed under a different parent style is dropped, and the walk
// computes the element's style itself. This happens if the parent presents an animation, which the cascade does not.
// The walk takes an element's style only after its parent's, so the parent style that a prefetch read is still held.
const CssComputedStyle* PR_NULLABLE takePrefetched(PrCssPresentation* r, const PrDomElement* element,
        const CssComputedStyle* PR_NULLABLE parentStyle, uint32_t* generating) {
    if (r->prefetchNext == r->prefetched.count)
        return nullptr;
    // The walk visits the prefetched elements in tree order, among other elements that it computes as it goes.
    CssPresentationPrefetch& next = r->prefetched.data[r->prefetchNext];
    if (next.element != element)
        return nullptr;
    ++r->prefetchNext;
    const CssComputedStyle* style = next.style;
    *generating = next.generating;
    if (next.parentStyle != parentStyle) {
        cssComputedStyleRelease(style);
        style = nullptr;
    }
    if (r->prefetchNext == r->prefetched.count) {
        r->prefetched.clear();
        r->prefetchNext = 0;
    }
    return style;
}

// The state that the threads of a prefetch share.
struct PrefetchJob {
    PrCssPresentation* presentation;
    const CssComputedStyle* root;   // style of the subtree's root
    CssFontBases rootFont;          // font bases of the document root, for root-relative lengths
    uint32_t slices;                // number of slices that the tasks are dealt among
};

// Returns the style of a prefetched element's parent, which was computed before the element.
const CssComputedStyle* parentOf(const PrefetchJob& job, const CssPresentationPrefetch& entry) {
    return entry.parent == kHashMissing ? job.root : job.presentation->prefetched.data[entry.parent].style;
}

// Computes the styles of slice `slice` of the tasks. A slice holds every task whose index modulo the number of slices
// is the slice's index. Each task's subtree is computed with each style after its parent's, using the slice's own
// builder and scratch, whichever thread runs it. A slice finds the same styles cached in its scratch whichever thread
// runs it and whenever it runs, so that a restyle does the same work in every run.
void runPrefetch(void* PR_NULLABLE context, uint32_t slice, uint32_t) {
    PrefetchJob* job = static_cast<PrefetchJob*>(context);
    PrCssPresentation* r = job->presentation;
    CssPresentationThread& t = r->threads.data[slice];
    for (uint32_t task = slice; task < r->tasks.count; task += job->slices) {
        uint32_t start = r->tasks.data[task];
        uint32_t end = r->prefetched.data[start].end;
        for (uint32_t i = start; i < end; ++i) {
            CssPresentationPrefetch& entry = r->prefetched.data[i];
            if (entry.style != nullptr)
                continue;
            entry.parentStyle = parentOf(*job, entry);
            CssCascadeInput input{
                .element = entry.element,
                .parent = entry.parentStyle,
                .rootFont = job->rootFont,
                .viewport = viewportOf(r),
                .fresh = false,
                .matched = entry.matched,
                .pseudo = CssPseudo_None,
            };
            entry.style = cssCascadeStyle(&r->rules, &t.builder, &t.scratch, input);
            if (entry.matched == nullptr)
                entry.generating = cssCascadeGenerating(&r->rules, &t.scratch, entry.element);
            // Each slice keeps the matches of its own elements, in entries that the walk does not move while the
            // threads run.
            if (entry.entry != kHashMissing && entry.matched == nullptr)
                keepMatches(r, entry.entry, t.scratch);
        }
    }
}

// Computes the styles of the descendants of `element`, whose style is `style`, ahead of a walk that restyles each of
// them. Divides the subtrees among the presentation's threads. Stops at the first element beyond `fresh` elements new
// to the presentation, because the walk takes no more than that. Wakes a thread for each kPrefetchPerThread elements,
// and does nothing for fewer than two threads' worth. `rematch` says whether what the descendants match may have
// changed. If it has not, each descendant takes what it matched last, unless its own change, or the change of an
// element between it and `element`, can change that.
void prefetch(PrCssPresentation* r, PrDomElement* element, const CssComputedStyle* style, CssFontBases rootFont,
        uint32_t fresh, bool rematch) {
    if (r->prefetchNext != r->prefetched.count || domAgentRestyleWidth(r->agent) == 1)
        return;
    // Lists the descendant elements in tree order, each with its parent's index and the end of its subtree.
    Array<uint32_t> open;
    DomFlatWalk walk;
    for (PrDomNode* n = domFlatWalkNext(walk, element, element); n != nullptr; n = domFlatWalkNext(walk, element, n)) {
        PrDomElement* e = domElementFromNode(n);
        if (e == nullptr)
            continue;
        uint32_t entry = cssPresentationNodeEntry(r, e);
        if (entry == kHashMissing || r->entries.data[entry].style == nullptr) {
            if (fresh == 0)
                break;
            --fresh;
        }
        PrDomNode* flatParent = domFlatParent(e);
        while (open.count != 0 && r->prefetched.data[open.last()].element != flatParent) {
            r->prefetched.data[open.last()].end = r->prefetched.count;
            open.pop();
        }
        cssStylePrepare(e);
        uint32_t parent = open.count != 0 ? open.last() : kHashMissing;
        bool above = parent != kHashMissing ? r->prefetched.data[parent].rematch : rematch;
        bool own = above || (e->pending & DomStyleFlag_Self) != 0 ||
            (r->features.count != 0 && carries(e, r->features.data, r->features.count));
        const Array<uint32_t>* matched = !own && entry != kHashMissing ? cachedMatches(r, entry) : nullptr;
        bool below = above || (e->pending & (DomStyleFlag_Subtree | DomStyleFlag_Features)) != 0;
        r->prefetched.push(
            CssPresentationPrefetch{e, parent, 0, nullptr, nullptr, entry, matched, below, kGeneratingUnknown});
        open.push(r->prefetched.count - 1);
    }
    while (open.count != 0) {
        r->prefetched.data[open.last()].end = r->prefetched.count;
        open.pop();
    }
    open.reset();
    domFlatWalkReset(walk);
    uint32_t count = r->prefetched.count;
    startThreads(r);
    uint32_t width = count / kPrefetchPerThread;
    PrTaskPool* pool = width >= 2 ? domAgentRestylePool(r->agent) : nullptr;
    if (pool == nullptr) {
        r->prefetched.clear();
        return;
    }
    // The presentation's threads are as many as the agent's restyle width, which is no wider than the pool.
    if (width > r->threads.count)
        width = r->threads.count;
    // Tasks are subtrees. A subtree much larger than its share is split: its root is computed here, and the subtrees of
    // its children become tasks in its place.
    PrefetchJob job{r, style, rootFont, width};
    uint32_t share = count / (width * 4);
    r->tasks.clear();
    for (uint32_t i = 0; i < count; i = r->prefetched.data[i].end)
        r->tasks.push(i);
    for (uint32_t k = 0; k < r->tasks.count;) {
        uint32_t root = r->tasks.data[k];
        CssPresentationPrefetch& entry = r->prefetched.data[root];
        if (entry.end - root <= share || entry.end - root == 1) {
            ++k;
            continue;
        }
        entry.parentStyle = parentOf(job, entry);
        CssCascadeInput input{
            .element = entry.element,
            .parent = entry.parentStyle,
            .rootFont = rootFont,
            .viewport = viewportOf(r),
            .fresh = false,
            .matched = entry.matched,
            .pseudo = CssPseudo_None,
        };
        entry.style = cssCascadeStyle(&r->rules, &r->threads.data[0].builder, &r->threads.data[0].scratch, input);
        if (entry.matched == nullptr)
            entry.generating = cssCascadeGenerating(&r->rules, &r->threads.data[0].scratch, entry.element);
        if (entry.entry != kHashMissing && entry.matched == nullptr)
            keepMatches(r, entry.entry, r->threads.data[0].scratch);
        r->tasks.data[k] = r->tasks.data[r->tasks.count - 1];
        r->tasks.pop();
        for (uint32_t i = root + 1; i < entry.end; i = r->prefetched.data[i].end)
            r->tasks.push(i);
    }
    // The states that selectors read look up elements by ID. A lookup writes what it finds unless it was already found,
    // so the IDs are resolved here, before the threads run.
    if (r->rules.reads.states)
        domResolveIds(r->document);
    prTaskPoolRun(pool, width, width, runPrefetch, &job);
    r->accrued.elementsStyledAhead += count;
}

void releaseDroppedQueue(PrCssPresentation* r) {
    for (uint32_t i = 0; i < r->boxQueueDropped.count; ++i)
        domNodeRelease(const_cast<PrDomNode*>(r->boxQueueDropped.data[i]));
    r->boxQueueDropped.clear();
}

// Queues box operations for the entry's box. Applying them makes or erases the box as `boxed` says, and performs `ops`.
void queueBox(PrCssPresentation* r, uint32_t at, CssPresentationBoxOps ops) {
    CssPresentationEntry& entry = r->entries.data[at];
    if ((entry.ops & CssPresentationBoxOp_Queued) == 0)
        r->boxQueue.push(entry.node);
    entry.ops |= ops | CssPresentationBoxOp_Queued;
}

// The number of new elements that a restyle takes in between readings of the clock.
constexpr uint32_t kDeadlineStride = 32;

// The state of one update as it walks the document.
struct Walk {
    PrCssPresentation* presentation;
    bool all;                       // every element is restyled
    uint32_t fresh;                 // number of new elements that it may still take in, or UINT32_MAX for all
    uint32_t taken;                 // number of new elements that it took in
    double deadline;                // time on the monotonic clock after which it takes in no more, or 0 for none
    bool parallel;                  // a restyle of whole subtrees is divided among the presentation's threads
    CssFontBases rootFont;          // font bases of the root, for root-relative lengths
    PrDomElement* PR_NULLABLE body; // the body element (html/#the-body-element-2)
};

// Decides whether a text node has a box, which it has if its parent has a box and the node has data. Returns true if
// this made or erased a box.
bool visitText(Walk& w, PrDomNode* node, bool parentBox) {
    PrCssPresentation* r = w.presentation;
    DomStyleFlags flags = node->pending;
    node->pending = 0;
    PrStringView data = prStringView(&static_cast<PrDomCharacterData*>(node)->data);
    bool box = parentBox && data.size != 0;
    uint32_t at = cssPresentationNodeEntry(r, node);
    if (!box) {
        if (at == kHashMissing || !r->entries.data[at].boxed)
            return false;
        r->entries.data[at].boxed = false;
        queueBox(r, at, 0);
        return true;
    }
    at = entryFor(r, node);
    if (!r->entries.data[at].boxed) {
        r->entries.data[at].boxed = true;
        queueBox(r, at, CssPresentationBoxOp_Text);
        staleFirsts(r, node);
        return true;
    }
    if ((flags & DomStyleFlag_Self) != 0) {
        queueBox(r, at, CssPresentationBoxOp_Text);
        staleFirsts(r, node);
    }
    return false;
}

// Returns the record that the text of an element of display contents is set in. This is the element's own record,
// because its children's properties inherit from it. Returns kPrLayoutStyleNone for an element that has a box, whose
// text uses the record of the box.
PrLayoutStyleIndex textStyleOf(PrCssPresentation* r, const PrDomElement* element) {
    uint32_t at = cssPresentationNodeEntry(r, element);
    if (at == kHashMissing || !r->entries.data[at].contents)
        return kPrLayoutStyleNone;
    return projectEntry(r, r->entries.data[at]);
}

// Puts `node`, the box of a child of the box `box`, at `next`, the place of the next child.
void placeBox(PrCssPresentation* r, PrLayoutNode box, PrLayoutNode node, PrLayoutNode& next) {
    if (node == next) {
        next = prLayoutNodeNextSibling(r->tree, next);
        return;
    }
    if (prLayoutNodeParent(r->tree, node) != kPrLayoutNodeNone)
        prLayoutTreeRemove(r->tree, node);
    prLayoutTreeInsert(r->tree, box, node, next);
}

// Puts the box of the element's pseudo-element at `which` of CssPresentationEntry::pseudos under `box` at `next`, if it
// has one.
void placePseudo(PrCssPresentation* r, const PrDomElement* element, uint32_t which, PrLayoutNode box, PrLayoutNode& next) {
    uint32_t at = cssPresentationNodeEntry(r, element);
    CssPresentationPseudo* pseudo = at != kHashMissing ? r->entries.data[at].pseudos[which] : nullptr;
    uint32_t own = pseudo != nullptr ? cssPresentationNodeEntry(r, pseudo) : kHashMissing;
    if (own != kHashMissing && r->entries.data[own].layoutNode != kPrLayoutNodeNone)
        placeBox(r, box, r->entries.data[own].layoutNode, next);
}

// Puts the boxes of the element's children under `box`, from `next` on, in their order, between the boxes of its
// ::before and its ::after (css-pseudo-4/#generated-content). The box of its ::marker comes first, ahead of ::before
// (css-lists-3/#marker-pseudo). A text control's ::field-content follows ::before. A child of display contents is
// replaced by the boxes of its own children, and its text is set in its record.
void placeChildren(PrCssPresentation* r, const PrDomElement* element, PrLayoutNode box, PrLayoutNode& next) {
    PrLayoutStyleIndex textStyle = kPrLayoutStyleNone;
    bool styled = false;
    placePseudo(r, element, kMarkerPseudo, box, next);
    placePseudo(r, element, kBeforePseudo, box, next);
    placePseudo(r, element, kFieldContentPseudo, box, next);
    domFlatEach(element, [&](PrDomNode* child) PR_FORCE_INLINE_LAMBDA {
        uint32_t at = cssPresentationNodeEntry(r, child);
        if (at != kHashMissing && r->entries.data[at].contents) {
            placeChildren(r, static_cast<const PrDomElement*>(child), box, next);
            return;
        }
        PrLayoutNode node = at != kHashMissing ? r->entries.data[at].layoutNode : PrLayoutNode(kPrLayoutNodeNone);
        if (node == kPrLayoutNodeNone)
            return;
        if (domIsText(child)) {
            if (!styled)
                textStyle = textStyleOf(r, element);
            styled = true;
            prLayoutNodeSetStyle(r->tree, node, textStyle);
        }
        placeBox(r, box, node, next);
    });
    placePseudo(r, element, kAfterPseudo, box, next);
}

// Puts the boxes of the element's children under its box, in their order, and removes the boxes that belong to no
// child. A box already in its place stays, so that children added after the others move nothing. The children's boxes
// keep their contents.
void reconcile(PrCssPresentation* r, PrDomElement* element, PrLayoutNode box) {
    PrLayoutNode next = prLayoutNodeFirstChild(r->tree, box);
    placeChildren(r, element, box, next);
    while (next != kPrLayoutNodeNone) {
        PrLayoutNode after = prLayoutNodeNextSibling(r->tree, next);
        prLayoutTreeRemove(r->tree, next);
        next = after;
    }
}

// Returns true if the pseudo-element `pseudo` of `element`, styled by `style`, generates a box.
//
// A ::before or ::after generates one if its content is neither none nor normal, which computes to none
// (css-content-3/#valdef-content-none), and its display generates one. display: contents generates none for now. Nor
// does a table column or column group, which is not in a table, and renders nothing of its own or of its content
// (CSS2/tables.html#columns).
//
// A list item's ::marker generates one if its content is not none, and if a content of normal has a marker image or a
// marker string to show (css-lists-3/#content-property). Its display does not apply to it.
//
// A text field's ::field-content generates one if its content is not none and its display generates one. It is the
// field's text, which content normal leaves empty until the field holds a value.
bool generatesPseudo(const PrDomElement* element, CssPseudo pseudo, const CssComputedStyle* style) {
    PrCssKeyword content = cssComputedFirstKeyword(style, PrCssProperty_Content);
    if (pseudo == CssPseudo_FieldContent)
        return isTextField(element) && content != PrCssKeyword_None && generatesBox(element, style);
    if (cssPseudoIsMarker(pseudo)) {
        if (content != PrCssKeyword_Normal)
            return content != PrCssKeyword_None;
        return cssComputedFirstKeyword(style, PrCssProperty_ListStyleImage) != PrCssKeyword_None ||
            cssComputedFirstKeyword(style, PrCssProperty_ListStyleType) != PrCssKeyword_None;
    }
    PrCssKeyword display = cssComputedFirstKeyword(style, PrCssProperty_Display);
    return content != PrCssKeyword_None && content != PrCssKeyword_Normal && generatesBox(element, style) &&
        display != PrCssKeyword_TableColumn && display != PrCssKeyword_TableColumnGroup;
}

// What a masked control shows for each code point of its value.
constexpr PrStringView kPasswordMark = "\xe2\x80\xa2";

// Returns true if the text control shows marks in place of its value, as input-security decides.
bool isMasked(const PrCssPresentation* r, const PrDomElement* element) {
    bool input = domIsHtml(element, PrDomTag_Input);
    if (!input || domInputType(domAttributeValue(element, "type")) != PrDomInputType_Password)
        return false;
    uint32_t at = cssPresentationNodeEntry(r, element);
    const CssComputedStyle* style = at != kHashMissing ? r->entries.data[at].style : nullptr;
    return style == nullptr || cssComputedFirstKeyword(style, PrCssProperty_InputSecurity) != PrCssKeyword_None;
}

// Returns what a pseudo-element's rules are filed under: a text control's ::field-content takes those of ::placeholder
// while its placeholder shows.
CssPseudo cascadedPseudo(const PrDomElement* element, CssPseudo kind) {
    bool shown = domElementPlaceholderShown(element) && domAttributeValue(element, "placeholder").size != 0;
    return kind == CssPseudo_FieldContent && shown ? CssPseudo_Placeholder : kind;
}

// Calls `text` with each run of the text of a pseudo-element's content (css-content-3/#content-property), and `image`
// with each image between them, as its computed value. Strings are text, and so are counter(), counters(), open-quote
// and close-quote, whose texts `counters` holds in order. The content ends at the alternative text after a `/`, which
// is for accessibility, which the engine does not expose. An image in a content list is a url() or a filter(), and a
// gradient there draws nothing.
//
// A marker whose content is normal shows its list item's marker image followed by a space, or its marker string if it
// has no image or the image failed to load (css-lists-3/#content-property). The marker image may be a generated image.
// A light-dark() image is the image that the used color scheme picks, and a none that it picks is a transparent image
// (css-color-5/#light-dark), whose value is the keyword none. Sets `readsOutside` if the pick read the user's preferred
// scheme. Gives the text that the ::field-content of `field` shows: its value, with a space after a value that is empty
// or ends in a line feed, so that the line that a caret stands on has text to place it. An empty control shows its
// placeholder, on one line in a one-line control. A masked control shows a mark for each code point of its value.
template <typename Text>
void fieldText(const PrDomElement* field, bool masked, Text text) {
    bool line = domIsHtml(field, PrDomTag_Input);
    PrStringView placeholder = domAttributeValue(field, "placeholder");
    if (domElementPlaceholderShown(field) && placeholder.size != 0) {
        PrTextBuffer shown{};
        for (size_t i = 0; i < placeholder.size; ++i) {
            char c = placeholder.data[i];
            if (!line || (c != '\n' && c != '\r'))
                prTextBufferAppend(&shown, PrStringView{&c, 1});
        }
        text(prTextBufferView(&shown));
        prTextBufferReset(&shown);
        return;
    }
    PrTextBuffer value{};
    domControlValue(field, &value);
    if (value.size == 0 || value.data[value.size - 1] == '\n')
        prTextBufferAppend(&value, " ");
    if (masked && value.size != 0 && value.data[0] != ' ') {
        PrTextBuffer marks{};
        for (uint32_t i = 0; i < value.size; ++i) {
            if ((uint8_t(value.data[i]) & 0xc0) != 0x80)
                prTextBufferAppend(&marks, kPasswordMark);
        }
        text(prTextBufferView(&marks));
        prTextBufferReset(&marks);
    } else {
        text(prTextBufferView(&value));
    }
    prTextBufferReset(&value);
}

template <typename Text, typename Image>
void eachGenerated(PrCssPresentation* r, const CssComputedStyle* style, CssPseudo pseudo,
        const PrDomElement* PR_NULLABLE originating, const CssCountingTexts& counters, bool* readsOutside, Text text,
        Image image) {
    if (pseudo == CssPseudo_FieldContent && originating != nullptr) {
        fieldText(originating, isMasked(r, originating), text);
        return;
    }
    CssValueRun content = cssComputedValue(style, PrCssProperty_Content);
    auto counterText = [&](uint32_t at) {
        uint32_t start = at == 0 ? 0 : counters.ends.data[at - 1];
        uint32_t end = at < counters.ends.count ? counters.ends.data[at] : start;
        return PrStringView{counters.text.data + start, end - start};
    };
    if (cssPseudoIsMarker(pseudo) && cssValueRunFirstKeyword(content) == PrCssKeyword_Normal) {
        CssValueRun markerImage = cssComputedValue(style, PrCssProperty_ListStyleImage);
        bool scheme = markerImage.count != 0 && cssValueIsFunction(markerImage.values, PrCssKeyword_LightDark);
        if (scheme) {
            CssColorEnvironment environment = colorEnvironmentOf(r, r->headroom, readsOutside);
            markerImage = schemeImageOf(markerImage, ColorTarget{cssStyleColorContext(style, environment), 0});
        }
        bool shown =
            (scheme && cssValueRunFirstKeyword(markerImage) == PrCssKeyword_None) || imageShows(r, markerImage);
        if (shown) {
            image(markerImage);
            text(" ");
        } else if (counters.ends.count != 0 && counters.ends.data[0] != 0) {
            text(counterText(0));
        }
        return;
    }
    PrTextBuffer run{};
    uint32_t next = 0;
    for (uint32_t i = 0; i < content.count; i += content.values[i].next) {
        const CssValueNode* item = &content.values[i];
        if (cssValueIsLiteral(item, '/'))
            break;
        PrStringView reference{};
        if (item->kind == PrCssValueKind_String) {
            prTextBufferAppend(&run, cssValueText(content, item));
        } else if (cssValueIsFunction(item, PrCssKeyword_Counter) || cssValueIsFunction(item, PrCssKeyword_Counters) ||
                cssValueIsKeyword(item, PrCssKeyword_OpenQuote) || cssValueIsKeyword(item, PrCssKeyword_CloseQuote)) {
            prTextBufferAppend(&run, counterText(next++));
        } else if (urlOf(cssValueSubtree(content, i), &reference) || isFilterImage(cssValueSubtree(content, i))) {
            if (run.size != 0)
                text(prTextBufferView(&run));
            prTextBufferClear(&run);
            image(cssValueSubtree(content, i));
        }
    }
    if (run.size != 0)
        text(prTextBufferView(&run));
    prTextBufferReset(&run);
}

// Returns the record of an anonymous box in a box styled by `parent`, which inherits what inherits from it and takes
// the initial values of the rest (css-cascade-5/#inheriting), such as an image of generated content. The box shows the
// generated image `image` as its content if it is not empty. Sets `readsOutside` if a color resolved for the display's
// headroom or the user's preferred scheme.
PrLayoutStyleIndex anonymousRecord(PrCssPresentation* r, const CssComputedStyle* parent, CssValueRun image,
        bool* readsOutside) {
    CssComputedStyle anonymous{};
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g)
        anonymous.groups[g] = cssStyleGroupInherited(CssStyleGroup(g)) ? parent->groups[g] : r->styles.initial[g];
    anonymous.custom = parent->custom;
    anonymous.locale = parent->locale;
    anonymous.font = parent->font;
    return project(r, &anonymous, Presentation{.image = image}, false, readsOutside);
}

// The default object size of a marker image, in ems: 0.4em square, which csswg-drafts#4207 proposes in place of the 1em
// of css-lists-3/#image-markers. A 1em marker image dwarfs the text it numbers.
constexpr double kMarkerImageEms = 0.4;

// Returns the size of a marker image of natural dimensions `natural` in a marker of font size `em`, by the default
// sizing algorithm with no specified size (css-images-3/#default-sizing).
PrLayoutNaturalSize markerImageSize(PrLayoutNaturalSize natural, double em) {
    float side = float(em * kMarkerImageEms);
    bool hasWidth = natural.width.raw >= 0;
    bool hasHeight = natural.height.raw >= 0;
    if (hasWidth && hasHeight)
        return natural;
    float ratio = natural.ratio;
    PrVec2 size{side, side};
    if (hasWidth) {
        size.x = prLayoutUnitToPx(natural.width);
        size.y = ratio > 0 ? size.x / ratio : side;
    } else if (hasHeight) {
        size.y = prLayoutUnitToPx(natural.height);
        size.x = ratio > 0 ? size.y * ratio : side;
    } else if (ratio > 0) {
        // The largest box of the ratio that fits the default size, as `contain` fits it.
        size = ratio >= 1 ? PrVec2{side, side / ratio} : PrVec2{side * ratio, side};
    }
    return PrLayoutNaturalSize{
        .width = prLayoutUnitFromPx(size.x),
        .height = prLayoutUnitFromPx(size.y),
        .ratio = ratio,
    };
}

// Returns the box of the marker of the pseudo-element `entry`, or none if it has none.
PrLayoutNode markerBoxOf(const PrCssPresentation* r, const CssPresentationEntry& entry) {
    const CssPresentationPseudo* marker = entry.pseudos[kMarkerPseudo];
    uint32_t own = marker != nullptr ? cssPresentationNodeEntry(r, marker) : kHashMissing;
    return own != kHashMissing ? r->entries.data[own].layoutNode : PrLayoutNode(kPrLayoutNodeNone);
}

// Puts the box of the marker of the pseudo-element `entry`, if it has one, first among the boxes of its content.
void placeMarkerOf(PrCssPresentation* r, const CssPresentationEntry& entry) {
    PrLayoutNode marker = markerBoxOf(r, entry);
    if (marker == kPrLayoutNodeNone)
        return;
    PrLayoutNode next = prLayoutNodeFirstChild(r->tree, entry.layoutNode);
    placeBox(r, entry.layoutNode, marker, next);
}

// Puts the boxes of a pseudo-element's content (css-content-3/#content-property) under its box, in place of those
// there: a text box for each run of strings, and a replaced box for each image that has loaded, and for each generated
// image. An image that is loading has no box until it loads, and one that fails to load has none: a missing image
// leaves nothing where it would be (CSS2/generated-content's before-after-images-001). The boxes are anonymous: they
// present no node. The box of the pseudo-element's marker stays first.
void applyGenerated(PrCssPresentation* r, CssPresentationEntry& entry) {
    PrLayoutNode box = entry.layoutNode;
    PrDomNode* node = const_cast<PrDomNode*>(entry.node);
    const CssPresentationPseudo* pseudo = static_cast<const CssPresentationPseudo*>(entry.node);
    PrLayoutNode marker = markerBoxOf(r, entry);
    for (PrLayoutNode child = prLayoutNodeFirstChild(r->tree, box); child != kPrLayoutNodeNone;
            child = prLayoutNodeFirstChild(r->tree, box)) {
        prLayoutTreeRemove(r->tree, child);
        if (child != marker)
            r->erasing.push(child);
    }
    bool isMarker = cssPseudoIsMarker(pseudo->pseudo);
    PrLayoutStyleIndex record = kPrLayoutStyleNone;
    bool reads = false;
    const PrDomElement* originating = domElementFromNode(pseudo->parent);
    eachGenerated(r, entry.style, pseudo->pseudo, originating, pseudo->counters, &reads,
        [&](PrStringView text) { prLayoutNodeCreateText(r->tree, box, text); }, [&](CssValueRun value) {
            // A generated image, or a transparent image, which paints nothing.
            bool generated = generatedImage(value);
            if (generated || cssValueRunFirstKeyword(value) == PrCssKeyword_None) {
                PrLayoutNaturalSize natural;
                if (generated)
                    natural = generatedNaturalOf(r, value, imageDrawingOf(r, entry.style));
                else
                    natural = prLayoutNaturalSizeNone();
                if (isMarker)
                    natural = markerImageSize(natural, entry.style->font.em);
                PrLayoutStyleIndex painted = anonymousRecord(r, entry.style, generated ? value : CssValueRun{}, &reads);
                PrLayoutNode image = prLayoutNodeCreateReplaced(r->tree, box, painted);
                prLayoutNodeSetNaturalSize(r->tree, image, natural);
                return;
            }
            PrStringView reference{};
            urlOf(value, &reference);
            CssPresentationImage* loaded = imageOf(r, reference);
            if (loaded != nullptr && loaded->state == CssPresentationImageState_Loading)
                loaded->waiting.push(node);
            if (loaded == nullptr || loaded->state != CssPresentationImageState_Loaded)
                return;
            if (record == kPrLayoutStyleNone)
                record = anonymousRecord(r, entry.style, CssValueRun{}, &reads);
            PrLayoutNode image = prLayoutNodeCreateReplaced(r->tree, box, record);
            CssPresentationImageDrawing drawing = imageDrawingOf(r, entry.style);
            PrLayoutNaturalSize natural = orientedSize(loaded->natural, loaded->orientation, drawing.oriented);
            if (isMarker)
                natural = markerImageSize(natural, entry.style->font.em);
            prLayoutNodeSetNaturalSize(r->tree, image, natural);
            PrLayoutContent content{};
            if (loaded->image != nullptr) {
                content.kind = PrLayoutContentKind_Image;
                content.image = orientedImage(loaded->image, loaded->orientation, drawing.oriented, &loaded->stored);
                content.limit = drawing.limit;
                content.rendering = drawing.rendering;
            } else {
                content.kind = PrLayoutContentKind_Painter;
                content.painter = loaded->vector;
            }
            prLayoutNodeSetContent(r->tree, image, &content);
        });
    entry.contentReadsOutside = reads;
    r->readsOutside = r->readsOutside || reads;
    placeMarkerOf(r, entry);
}

// Returns a new pseudo-element of `parent`, which the presentation owns: an element, or the ::before or ::after whose
// marker it is.
CssPresentationPseudo* createPseudo(PrDomNode* parent, CssPseudo which) {
    CssPresentationPseudo* pseudo = allocZeroed<CssPresentationPseudo>();
    pseudo->type = DomTargetType_PseudoElement;
    pseudo->document = parent->document;
    pseudo->parent = parent;
    pseudo->pseudo = which;
    return pseudo;
}

// Returns true if a pseudo-element of the entry at `at`, or a marker of one, has a change marked on it.
bool pseudoMarked(const PrCssPresentation* r, uint32_t at) {
    for (const CssPresentationPseudo* pseudo : r->entries.data[at].pseudos) {
        if (pseudo == nullptr)
            continue;
        uint32_t own = cssPresentationNodeEntry(r, pseudo);
        if (pseudo->pending != 0 || (own != kHashMissing && pseudoMarked(r, own)))
            return true;
    }
    return false;
}

void visitMarkerOf(Walk& w, uint32_t at, CssPseudo kind, PrDomElement* element,
    const CssComputedStyle* PR_NULLABLE style, bool restyle, bool suppressed);

// Brings the pseudo-element `kind` that the entry at `at` holds at `which` up to date: its style, under `parent`, the
// presented style of what originates it, its animations, its box, and whether it takes part in the counter pass.
// `element` is the originating element, `restyle` says whether its style is computed again, and `generating` whether it
// may generate a box. `parent` may be null only if the pseudo-element generates no box. A ::before or an ::after that
// is a list item holds a marker of its own, which comes first among the boxes of its content. Returns true if the
// pseudo-element's box was made or erased, so that the boxes of its parent's children are placed again.
//
// The base style decides whether the pseudo-element exists, as an element's base style decides whether display: none
// cancels its animations. An animation that takes the box away, as one of display or content may, keeps the
// pseudo-element's styles and animations.
bool visitPseudo(Walk& w, uint32_t at, uint32_t which, CssPseudo kind, PrDomElement* element,
        const CssComputedStyle* PR_NULLABLE parent, bool restyle, bool generating, bool suppressed) {
    PrCssPresentation* r = w.presentation;
    CssPresentationPseudo* pseudo = r->entries.data[at].pseudos[which];
    if (pseudo == nullptr && !generating)
        return false;
    uint32_t own = pseudo != nullptr ? cssPresentationNodeEntry(r, pseudo) : kHashMissing;
    DomStyleFlags flags = pseudo != nullptr ? pseudo->pending : 0;
    if (pseudo != nullptr)
        pseudo->pending = 0;
    bool kept = own != kHashMissing && r->entries.data[own].style != nullptr;
    const CssComputedStyle* base = nullptr;
    if (restyle && generating) {
        PR_ASSERT(parent != nullptr);
        CssCascadeInput input{
            .element = element,
            .parent = parent,
            .rootFont = w.rootFont,
            .viewport = viewportOf(r),
            .fresh = false,
            .matched = nullptr,
            .pseudo = cascadedPseudo(element, kind),
        };
        base = cssCascadeStyle(&r->rules, &r->threads.data[0].builder, &r->threads.data[0].scratch, input);
    } else if (!restyle && !suppressed && kept) {
        const CssPresentationEntry& entry = r->entries.data[own];
        base = entry.base != nullptr ? entry.base : entry.style;
        cssComputedStyleRetain(base);
    }
    if (base == nullptr || !generatesPseudo(element, kind, base)) {
        cssComputedStyleRelease(base);
        if (own == kHashMissing)
            return false;
        bool changed = false;
        CssPresentationEntry& entry = r->entries.data[own];
        if (entry.boxed) {
            entry.boxed = false;
            queueBox(r, own, 0);
            changed = true;
        }
        if (entry.counts)
            cssCountersChanged(r, element);
        entry.counts = false;
        entry.listItem = false;
        if (entry.animated)
            cssAnimateForget(r, pseudo);
        entry.animated = false;
        entry.composited = false;
        cssComputedStyleRelease(entry.style);
        cssComputedStyleRelease(entry.base);
        entry.style = nullptr;
        entry.base = nullptr;
        visitMarkerOf(w, own, kind, element, nullptr, false, true);
        return changed;
    }
    if (pseudo == nullptr) {
        PrDomNode* holder = const_cast<PrDomNode*>(r->entries.data[at].node);
        pseudo = createPseudo(holder, kind);
        r->entries.data[at].pseudos[which] = pseudo;
    }
    own = entryFor(r, pseudo);
    CssPresentationEntry& entry = r->entries.data[own];
    const CssComputedStyle* old = entry.style;
    const CssComputedStyle* oldBase = entry.base != nullptr ? entry.base : old;
    // The style that the pseudo-element presents: its base style with its animations, sampled again if the time moved
    // them.
    const CssComputedStyle* pseudoStyle = old;
    if (restyle || ((flags & DomStyleFlag_Animate) != 0 && old != nullptr)) {
        bool animationReads = false;
        CssColorEnvironment environment = colorEnvironmentOf(r, r->headroom, &animationReads);
        const CssStyleValues* initial = r->styles.initial[CssStyleGroup_Animation];
        if (restyle && (entry.animated || base->groups[CssStyleGroup_Animation] != initial)) {
            CssAnimateContext context{parent, w.rootFont, viewportOf(r)};
            cssAnimateRestyled(r, pseudo, old != nullptr ? oldBase : nullptr, old, base, context, environment);
        }
        entry.animated = cssAnimateHas(r, pseudo);
        if (entry.animated) {
            pseudoStyle = cssAnimateApply(r, pseudo, base, false, environment);
        } else {
            pseudoStyle = base;
            cssComputedStyleRetain(pseudoStyle);
        }
        entry.animationReadsOutside = entry.animated && animationReads;
        r->readsOutside = r->readsOutside || entry.animationReadsOutside;
        cssComputedStyleRelease(entry.base);
        entry.base = pseudoStyle != base ? base : nullptr;
        if (pseudoStyle == base)
            cssComputedStyleRelease(base);
    } else {
        cssComputedStyleRetain(pseudoStyle);
        cssComputedStyleRelease(base);
    }
    entry.style = pseudoStyle;
    bool composited = entry.animated && cssAnimateComposited(r, pseudo);
    bool layerChanged = composited != entry.composited;
    entry.composited = composited;
    if (!generatesPseudo(element, kind, pseudoStyle)) {
        bool changed = false;
        if (entry.boxed) {
            entry.boxed = false;
            queueBox(r, own, 0);
            changed = true;
        }
        if (entry.counts)
            cssCountersChanged(r, element);
        entry.counts = false;
        entry.listItem = false;
        cssComputedStyleRelease(old);
        visitMarkerOf(w, own, kind, element, nullptr, false, true);
        return changed;
    }
    // The content of a marker whose content is normal comes from its list style, and an image marker is sized by its
    // font.
    const CssStyleValues* const* now = pseudoStyle->groups;
    bool marker = cssPseudoIsMarker(kind);
    bool groups = old == nullptr || old->groups[CssStyleGroup_Generated] != now[CssStyleGroup_Generated] ||
        (marker && old->groups[CssStyleGroup_List] != now[CssStyleGroup_List]);
    bool sized = marker && old != nullptr && old->groups[CssStyleGroup_Font] != now[CssStyleGroup_Font];
    bool content = groups || sized || (flags & DomStyleFlag_Self) != 0;
    // Whether a marker is outside depends on its list item's style, which its record reads.
    bool outside = marker && parent != nullptr && isOutsideMarker(parent);
    bool restyled = pseudoStyle != old || layerChanged || entry.reprojectPending || outside != pseudo->outside;
    pseudo->outside = outside;
    bool counts = cssCountsPseudo(r, pseudoStyle, kind);
    // The marks of quotes come from the quotes and the languages of the pseudo-element's ancestors, which a restyle of
    // the originating element reaches.
    bool quotes = counts && restyle && cssReadsQuotes(pseudoStyle);
    if (counts != entry.counts || (counts && groups) || quotes)
        cssCountersChanged(r, element);
    entry.counts = counts;
    if (counts)
        cssCountsBelow(r, pseudo);
    if (pseudoStyle != old)
        pseudo->markerKept = false;
    entry.reprojectPending = false;
    entry.listItem = !marker && cssIsListItem(pseudoStyle);
    cssComputedStyleRelease(old);
    bool changed = false;
    if (!entry.boxed) {
        entry.boxed = true;
        queueBox(r, own, CssPresentationBoxOp_Style | CssPresentationBoxOp_Content);
        changed = true;
        restyle = true;
    } else {
        if (restyled)
            queueBox(r, own, CssPresentationBoxOp_Style);
        if (content)
            queueBox(r, own, CssPresentationBoxOp_Content);
    }
    visitMarkerOf(w, own, kind, element, pseudoStyle, restyle, false);
    return changed;
}

// Brings the marker of the ::before or ::after at `at`, styled by `style`, up to date, as visitPseudo does: a list item
// has one (css-lists-3/#marker-pseudo). `kind` is the pseudo-element. A box made or erased places the boxes of the
// pseudo-element's content again.
void visitMarkerOf(Walk& w, uint32_t at, CssPseudo kind, PrDomElement* element,
        const CssComputedStyle* PR_NULLABLE style, bool restyle, bool suppressed) {
    if (kind != CssPseudo_Before && kind != CssPseudo_After)
        return;
    PrCssPresentation* r = w.presentation;
    bool listItem = style != nullptr && cssIsListItem(style);
    if (r->entries.data[at].pseudos[kMarkerPseudo] == nullptr && !listItem)
        return;
    CssPseudo marker = kind == CssPseudo_Before ? CssPseudo_BeforeMarker : CssPseudo_AfterMarker;
    if (visitPseudo(w, at, kMarkerPseudo, marker, element, style, restyle, listItem && !suppressed,
            suppressed || !listItem))
        queueBox(r, at, CssPresentationBoxOp_Children);
}

// Brings the ::before, ::after, ::marker and ::field-content of the element at `at` up to date (visitPseudo), under
// `style`, the element's presented style. `holds` says whether the boxes of the element's children have a place, which
// the boxes of its pseudo-elements share, unless the element's box is unusual (css-pseudo-4/#generated-content).
// `restyle` says whether the element's style was computed again, which computes theirs again too. `generating` gives
// what cssCascadeGenerating gives for the element, if a prefetch found it, or kGeneratingUnknown. Returns true if a
// pseudo-element's box was made or erased, so that its parent's children are placed again.
bool visitPseudos(Walk& w, uint32_t at, PrDomElement* element, const CssComputedStyle* style, bool holds,
        bool restyle, uint32_t generating) {
    PrCssPresentation* r = w.presentation;
    bool changed = false;
    bool suppressed = !holds || unboxesToNothing(element);
    bool any = false;
    for (const CssPresentationPseudo* pseudo : r->entries.data[at].pseudos)
        any = any || pseudo != nullptr;
    // Their styles and boxes change only with the element's, or with what marked them, such as an image they show. Most
    // elements have pseudo-elements that match no rule that sets content, and nothing to do.
    if (!restyle && !(any && pseudoMarked(r, at)))
        return false;
    startThreads(r);
    if (suppressed || !restyle)
        generating = 0;
    else if (generating == kGeneratingUnknown)
        generating = cssCascadeGenerating(&r->rules, &r->threads.data[0].scratch, element);
    if (restyle && !suppressed && cachedMatches(r, at) != nullptr)
        r->entries.data[at].generating = generating;
    // Every list item has a marker, whose style shows whether it generates a box (css-lists-3/#marker-pseudo), and
    // every text field has the part that holds its text.
    if (restyle && !suppressed && r->entries.data[at].listItem)
        generating |= 1u << CssPseudo_Marker;
    // A text field's part is its own, which no rule for unusual boxes suppresses.
    if (restyle && holds && isTextField(element))
        generating |= 1u << CssPseudo_FieldContent;
    if (generating == 0 && !any)
        return false;
    for (uint32_t which = 0; which < kCssPresentationPseudos; ++which) {
        CssPseudo kind = CssPseudo(CssPseudo_Before + which);
        bool hidden = kind == CssPseudo_FieldContent ? !holds : suppressed;
        changed = visitPseudo(w, at, which, kind, element, style, restyle, (generating & (1u << kind)) != 0, hidden) || changed;
    }
    return changed;
}

// Brings an element up to date. Restyles it if it changed, if an ancestor's change reaches its subtree (`forced`), or
// if its parent's style changed under it. Then makes, erases or restyles its box, and visits the descendants that need
// it. `rematch` says that an ancestor's change can change what the element and its descendants match. If neither that
// nor the element's own change can, a restyle takes what the element matched last. `parentBox` says whether the parent
// has a box for the boxes of its children. Returns true if the element's box was made or erased.
bool visitElement(Walk& w, PrDomElement* element, const CssComputedStyle* PR_NULLABLE parentStyle, ParentChange parent,
        bool forced, bool rematch, bool parentBox) {
    PrCssPresentation* r = w.presentation;
    ++r->accrued.elementsVisited;
    DomStyleFlags flags = element->pending;
    element->pending = 0;
    uint32_t at = entryFor(r, element);
    const CssComputedStyle* old = r->entries.data[at].style;
    const CssComputedStyle* oldBase = r->entries.data[at].base != nullptr ? r->entries.data[at].base : old;
    bool carried = r->features.count != 0 && carries(element, r->features.data, r->features.count);
    bool matches = w.all || rematch || old == nullptr || (flags & DomStyleFlag_Self) != 0 || carried;
    // The style that the element presents takes the parent's value of a property that does not inherit if its base
    // style or one of its animations does.
    bool restyle = matches || forced || parent == ParentChange_Inherited ||
        (parent == ParentChange_Reset && (oldBase->inheritsReset || old->inheritsReset));
    bool animate = (flags & DomStyleFlag_Animate) != 0 && old != nullptr;
    if (old == nullptr) {
        ++w.taken;
        if (w.fresh != UINT32_MAX)
            --w.fresh;
        // The clock is read periodically, as new elements are taken in.
        if (w.deadline != 0 && w.taken % kDeadlineStride == 0 && prTimeMonotonicMilliseconds() > w.deadline)
            w.fresh = 0;
    }
    ParentChange change = ParentChange_None;
    bool customChanged = false;
    // The pseudo-elements that may generate a box, if a prefetch found them with the style.
    uint32_t generating = kGeneratingUnknown;
    // An element that only its animations change keeps its custom properties.
    bool sameCustom = true;
    // Whether the restyle changed the element's counter properties or whether it is a list item, and whether it changed
    // the element's style containment, which scopes the counters of its subtree.
    bool countersChanged = false;
    bool scopeChanged = false;
    // Whether the restyle changed a computed value, which is all that SVG content reads.
    bool valuesChanged = false;
    const CssComputedStyle* style = old;
    if (restyle || animate) {
        const CssComputedStyle* base = oldBase;
        if (restyle) {
            base = takePrefetched(r, element, parentStyle, &generating);
            if (base == nullptr) {
                const Array<uint32_t>* cached = matches ? nullptr : cachedMatches(r, at);
                base = cascade(r, element, parentStyle, w.rootFont, false, cached, at);
                if (cached == nullptr)
                    keepMatches(r, at, r->threads.data[0].scratch);
            }
            ++r->accrued.elementsStyled;
            // What the element matched last decides its pseudo-elements, as it decides its style.
            if (!matches && cachedMatches(r, at) != nullptr && generating == kGeneratingUnknown)
                generating = r->entries.data[at].generating;
            // The style that an element presents has the custom properties of its base style, whatever its animations
            // do. Comparing the custom properties of the base styles therefore says whether those that the element
            // presents changed.
            sameCustom = old != nullptr && cssCustomPropertiesEqual(oldBase->custom, base->custom);
            if (old != nullptr && sameStyle(oldBase, base, sameCustom))
                ++r->accrued.elementsStyledUnchanged;
        } else {
            cssComputedStyleRetain(base);
        }
        // An element with no animations and an initial animation group starts no animations.
        CssPresentationEntry& entry = r->entries.data[at];
        const CssStyleValues* initial = r->styles.initial[CssStyleGroup_Animation];
        // Animated colors resolve for the display, as the box's colors do. A new headroom then restyles the element.
        bool animationReads = false;
        CssColorEnvironment environment = colorEnvironmentOf(r, r->headroom, &animationReads);
        if (restyle && (entry.animated || base->groups[CssStyleGroup_Animation] != initial)) {
            CssAnimateContext context{parentStyle, w.rootFont, viewportOf(r)};
            cssAnimateRestyled(r, element, old != nullptr ? oldBase : nullptr, old, base, context, environment);
        }
        entry.animated = cssAnimateHas(r, element);
        if (entry.animated) {
            // A compositor runs the composited animations. The element's box keeps its base effects, as a layer.
            style = cssAnimateApply(r, element, base, false, environment);
            if (animate)
                ++r->accrued.elementsAnimated;
        } else {
            style = base;
            cssComputedStyleRetain(style);
        }
        entry.animationReadsOutside = entry.animated && animationReads;
        r->readsOutside = r->readsOutside || entry.animationReadsOutside;
        if (restyle) {
            bool unknown = !r->rules.reads.direction || !automaticDirection(element);
            if (unknown)
                entry.direction = CssPresentationDirection_Unknown;
            else if (domElementRtl(element))
                entry.direction = CssPresentationDirection_Rtl;
            else
                entry.direction = CssPresentationDirection_Ltr;
        }
        change = changeOf(old, style, sameCustom);
        valuesChanged = style != old && (old == nullptr || !sameStyle(old, style, sameCustom));
        // A change to the custom properties restyles every descendant, because descendants inherit them.
        customChanged = old != nullptr && !sameCustom;
        // The box group holds display, which makes a list item, and the properties that give style containment.
        if (old == nullptr || style->groups[CssStyleGroup_Box] != old->groups[CssStyleGroup_Box]) {
            bool listItem = cssIsListItem(style);
            countersChanged = listItem != entry.listItem;
            entry.listItem = listItem;
            scopeChanged = old != nullptr && cssContainsStyle(style) != cssContainsStyle(old);
        }
        if (old != nullptr && style->groups[CssStyleGroup_Generated] != old->groups[CssStyleGroup_Generated])
            countersChanged = true;
        cssComputedStyleRelease(entry.base);
        cssComputedStyleRelease(old);
        entry.style = style;
        entry.base = style != base ? base : nullptr;
        if (style == base)
            cssComputedStyleRelease(base);
    }
    bool composited = r->entries.data[at].animated && cssAnimateComposited(r, element);
    bool layerChanged = composited != r->entries.data[at].composited;
    r->entries.data[at].composited = composited;
    bool restyled = style != old || layerChanged || r->entries.data[at].reprojectPending;
    r->entries.data[at].reprojectPending = false;
    // The content of svg elements reads the computed values, and any element may be in it or be referenced from it.
    if (valuesChanged)
        svgNote(r, element, CssPresentationSvgNoteKind_Style);
    // Only an element that originates a ::first-line or ::first-letter, or a rule set that may style one, restyles
    // them, so a page without such rules pays nothing for them.
    if ((restyle || style != old) && (r->entries.data[at].originates || stylesFirsts(r)))
        restyleFirsts(r, at, element, style);
    if (restyle || style != old || (flags & DomStyleFlag_Children) != 0)
        staleFirsts(r, element);
    if (parentStyle == nullptr) {
        // In every element, rem and rlh units are relative to the font size and line height of the root
        // (css-values-4/#font-relative-lengths).
        CssFontBases rootFont = cssStyleFontBases(style);
        // rex, rcap, rch and ric also read the root's font. A page that has measured no metric has never read it.
        bool metrics = r->readsMetrics &&
            (old == nullptr || old->groups[CssStyleGroup_Font] != style->groups[CssStyleGroup_Font]);
        if ((rootFont.em != w.rootFont.em || rootFont.lh != w.rootFont.lh || metrics) && !w.all)
            w.all = true;
        w.rootFont = rootFont;
    }
    bool subtree = forced || (flags & DomStyleFlag_Subtree) != 0 || customChanged;
    // What the descendants match changes with a change that reaches the subtree, or with the features that this element
    // pushes.
    bool childRematch = rematch || (flags & DomStyleFlag_Subtree) != 0;
    // For a change that restyles every descendant, their styles are computed ahead in parallel, if a caller waits for
    // the result.
    if (w.parallel && domFlatHasChildren(element) && (w.all || subtree))
        prefetch(r, element, style, w.rootFont, w.fresh, w.all || childRematch || (flags & DomStyleFlag_Features) != 0);

    // A foreignObject of SVG content has a box of its own, a root of the layout tree, whatever its parent's.
    bool box = (parentBox || isForeignRoot(element)) && generatesBox(element, style);
    bool contents = parentBox && passesBoxes(element, style);
    CssBoxTraits traits = cssBoxTraits(element);
    bool replaced = traits.kind == CssBoxKind_Replaced;
    bool semiReplaced = traits.kind == CssBoxKind_Button || traits.kind == CssBoxKind_TextField;
    bool childBox = (box && !replaced && traits.kind != CssBoxKind_LineBreak) || contents;
    // A text control's children are its default value, which its ::field-content shows, and they have no boxes.
    bool nodeBoxes = childBox && traits.kind != CssBoxKind_TextField;
    bool boxChanged = false;
    bool made = false;
    // The parent reconciles whether its box holds this element's children's boxes in the element's place.
    if (contents != r->entries.data[at].contents) {
        r->entries.data[at].contents = contents;
        boxChanged = true;
        made = contents;
    }
    if (!box && r->entries.data[at].boxed) {
        r->entries.data[at].boxed = false;
        queueBox(r, at, 0);
        boxChanged = true;
        dropPendingBoxes(r, element);
    } else if (box && !r->entries.data[at].boxed) {
        r->entries.data[at].boxed = true;
        queueBox(r, at, CssPresentationBoxOp_Style);
        boxChanged = true;
        made = true;
    } else if (box && (replaced != r->entries.data[at].replaced || semiReplaced != r->entries.data[at].semiReplaced)) {
        queueBox(r, at, CssPresentationBoxOp_Remake);
        boxChanged = true;
        made = true;
        dropPendingBoxes(r, element);
    } else if (box && restyled) {
        queueBox(r, at, CssPresentationBoxOp_Style);
        // A replaced element's image takes its orientation, its cap and its resampling with its content.
        if (replaced && imageDrawingOf(r, style) != r->entries.data[at].imageDrawing)
            queueBox(r, at, CssPresentationBoxOp_Content);
    }
    if (box) {
        r->entries.data[at].replaced = replaced;
        r->entries.data[at].semiReplaced = semiReplaced;
    }
    // The counters change with a participant's counter properties, and with the boxes of the subtrees that hold
    // participants. An element whose counter properties, display and box are unchanged changes no counter.
    if (old == nullptr || countersChanged || scopeChanged || boxChanged) {
        bool counts = box && cssCountsElement(r, style, r->entries.data[at].listItem);
        CssPresentationEntry& counted = r->entries.data[at];
        if (counts != counted.counts || (counts && countersChanged) ||
                ((boxChanged || scopeChanged) && counted.countsBelow))
            cssCountersChanged(r, element);
        counted.counts = counts;
        if (counts)
            cssCountsBelow(r, element);
    }
    // Only a replaced element has content, although another element, such as an image button, may also load an image.
    if (box && replaced && !made && (flags & DomStyleFlag_Content) != 0)
        queueBox(r, at, CssPresentationBoxOp_Content);

    // The pseudo-elements' boxes are among the children's.
    bool pseudos = visitPseudos(w, at, element, style, childBox, restyle || style != old || boxChanged, generating);

    // The features by which an ancestor's change restyles descendants also apply under this element, together with the
    // element's own features.
    uint32_t features = r->features.count;
    if ((flags & DomStyleFlag_Features) != 0)
        pushFeatures(r, element);
    bool checking = r->features.count != 0;
    bool visit = made || w.all || subtree || change != ParentChange_None || checking ||
        (flags & (DomStyleFlag_Descendant | DomStyleFlag_Children)) != 0;
    if (!visit) {
        if (pseudos && contents)
            return true;
        if (pseudos && childBox)
            queueBox(r, at, CssPresentationBoxOp_Children);
        return boxChanged;
    }
    bool children = made || pseudos || (flags & DomStyleFlag_Children) != 0;
    // A change that reaches every child, or added children, makes the walk look at each child. Otherwise only the
    // marked children have anything to do.
    bool every = children || checking || change != ParentChange_None || w.all || subtree;
    // Once the walk may take in no more new elements, the remaining new children wait for a later update, which visits
    // the element for them. Children that are already in are visited as usual.
    bool held = false;
    auto showsBox = [&](const PrDomNode* child) {
        return nodeBoxes && ((traits.flags & CssBoxTraitFlag_ShowsOneChild) == 0 || child == traits.shownChild);
    };
    domFlatEach(element, [&](PrDomNode* child) PR_FORCE_INLINE_LAMBDA {
        if (!every && child->pending == 0)
            return;
        if (PrDomElement* e = domElementFromNode(child)) {
            bool marked = e->pending != 0;
            uint32_t entry = cssPresentationNodeEntry(r, e);
            bool fresh = entry == kHashMissing || r->entries.data[entry].style == nullptr;
            if (fresh && (held || w.fresh == 0)) {
                held = true;
                return;
            }
            bool unseen = entry == kHashMissing || (nodeBoxes && !r->entries.data[entry].boxed && made);
            if (marked || unseen || checking || change != ParentChange_None || w.all || made || subtree)
                children |= visitElement(w, e, style, change, subtree, childRematch, showsBox(child));
        } else if (domIsText(child)) {
            // New text waits if a new element before it waits. Text costs the walk nothing of its own.
            bool fresh = cssPresentationNodeEntry(r, child) == kHashMissing;
            if (fresh && held)
                return;
            if (made || child->pending != 0 || w.all || fresh)
                children |= visitText(w, child, showsBox(child));
        }
    });
    if (held)
        mark(element, DomStyleFlag_Children);
    r->features.count = features;
    // The boxes of the children of an element of display contents are children of its parent's box, and its text is set
    // in its record.
    if (contents)
        return boxChanged || children || restyled;
    if (childBox && children)
        queueBox(r, at, CssPresentationBoxOp_Children);
    return boxChanged;
}

// Appends the sheets of the document's trees to `out`: the document's own, then those of each connected shadow root, in
// shadow-including tree order. A document that has never had a shadow root has no other tree to walk for.
void treeSheets(PrDomDocument* document, Array<CssTreeSheets>& out) {
    PrCssStyleSheetList* sheets = cssDocumentStyleSheets(document);
    out.push(CssTreeSheets{document, 0, sheets->sheets.data, sheets->sheets.count});
    if (document->slots == nullptr)
        return;
    for (PrDomNode* node = document; node != nullptr; node = domShadowIncludingNext(document, node)) {
        PrDomShadowRoot* shadow = domNodeShadowRoot(node);
        if (shadow == nullptr || shadow->style == nullptr || shadow->style->sheets.sheets.count == 0)
            continue;
        uint32_t depth = 0;
        for (const PrDomShadowRoot* at = shadow; at != nullptr; at = domNodeShadowRoot(domNodeRoot(at->host)))
            ++depth;
        out.push(CssTreeSheets{shadow, depth, shadow->style->sheets.sheets.data, shadow->style->sheets.sheets.count});
    }
}

// Returns true if `element` is an SVG element that is never rendered, nor is its content (SVG2/render.html#Rendering).
bool neverRendered(const PrDomElement* element) {
    if (element->space != PrDomNamespace_Svg)
        return false;
    switch (element->tag) {
        case PrDomTag_ClipPath:
        case PrDomTag_Defs:
        case PrDomTag_Desc:
        case PrDomTag_LinearGradient:
        case PrDomTag_Marker:
        case PrDomTag_Mask:
        case PrDomTag_Metadata:
        case PrDomTag_Pattern:
        case PrDomTag_RadialGradient:
        case PrDomTag_Script:
        case PrDomTag_Style:
        case PrDomTag_Symbol:
        case PrDomTag_Title:
            return true;
        default:
            return false;
    }
}

// Returns the nearest ancestor of `element` whose children are fallback content, as a canvas's are, or null.
const PrDomElement* PR_NULLABLE fallbackAround(const PrDomElement* element) {
    for (const PrDomElement* at = domElementFromNode(element->parent); at != nullptr;
            at = domElementFromNode(at->parent)) {
        if ((cssBoxTraits(at).flags & CssBoxTraitFlag_HoldsFallback) != 0)
            return at;
    }
    return nullptr;
}

// Implements cssPresentationIsInteractive, as the last restyle left the styles. An element of SVG content has no box,
// since the content of its svg element draws it: it is rendered if it and its ancestors in that content are displayed
// and none is never rendered. An element there that the presentation did not style, such as an HTML element outside a
// foreignObject, is rendered if its parent is. A canvas's fallback content has no box either, and counts if it is
// displayed and the canvas is rendered (html/#being-used-as-relevant-canvas-fallback-content).
bool interactive(const PrCssPresentation* r, const PrDomElement* element) {
    const PrDomElement* parent = domElementFromNode(element->parent);
    bool svgContent = parent != nullptr && parent->space == PrDomNamespace_Svg && parent->tag != PrDomTag_ForeignObject;
    if (svgContent && neverRendered(element))
        return false;
    uint32_t at = cssPresentationNodeEntry(r, element);
    const CssPresentationEntry* entry = at != kHashMissing ? &r->entries.data[at] : nullptr;
    if (entry == nullptr || entry->style == nullptr)
        return svgContent && interactive(r, parent);
    bool displayed = cssComputedFirstKeyword(entry->style, PrCssProperty_Display) != PrCssKeyword_None;
    const PrDomElement* canvas = !svgContent && !entry->boxed && !entry->contents ? fallbackAround(element) : nullptr;
    const PrDomElement* around = svgContent ? parent : canvas;
    bool shown = around != nullptr ? displayed : entry->boxed || entry->contents;
    return shown && cssComputedFirstKeyword(entry->style, PrCssProperty_Visibility) == PrCssKeyword_Visible &&
        cssComputedFirstKeyword(entry->style, PrCssProperty_Interactivity) != PrCssKeyword_Inert &&
        (around == nullptr || interactive(r, around));
}

// Returns the layout node that presents `node`, or kPrLayoutNodeNone.
PrLayoutNode layoutNodeOf(const PrCssPresentation* r, const PrDomNode* node) {
    uint32_t at = cssPresentationNodeEntry(r, node);
    return at != kHashMissing ? r->entries.data[at].layoutNode : PrLayoutNode(kPrLayoutNodeNone);
}

// Returns the side of `property` if it is a margin or padding property, which `*padding` then says, or
// PrRectSide_Count.
uint32_t edgeSideOf(PrCssProperty property, bool* padding) {
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        *padding = property == kPaddings[side];
        if (property == kMargins[side] || *padding)
            return side;
    }
    return PrRectSide_Count;
}

// Returns true if the resolved value of `property` can be a used value, which a layout decides.
bool resolvesFromLayout(PrCssProperty property) {
    bool padding = false;
    return edgeSideOf(property, &padding) != PrRectSide_Count || property == PrCssProperty_Width ||
        property == PrCssProperty_Height ||
        property == PrCssProperty_TransformOrigin || property == PrCssProperty_PerspectiveOrigin ||
        property == PrCssProperty_GridTemplateColumns || property == PrCssProperty_GridTemplateRows;
}

// Writes the used track list of the grid container `node` along `axis` (css-grid-1/#resolved-track-list): each track of
// its implicit grid as its size in pixels, with the names of each line before it in one bracketed set. A grid with no
// tracks writes none.
void serializeTrackList(PrTextBuffer* out, const PrCssPresentation* r, PrLayoutNode node, PrLayoutGridAxis axis) {
    uint32_t count = prLayoutNodeGridTrackSizes(r->tree, node, axis, nullptr, 0);
    if (count == 0) {
        prTextBufferAppend(out, "none");
        return;
    }
    Array<PrLayoutUnit> sizes;
    sizes.resize(count);
    prLayoutNodeGridTrackSizes(r->tree, node, axis, sizes.data, count);
    Array<PrLayoutGridLineName> names;
    names.resize(prLayoutNodeGridLineNames(r->tree, node, axis, nullptr, 0));
    prLayoutNodeGridLineNames(r->tree, node, axis, names.data, names.count);
    uint32_t next = 0;
    for (uint32_t line = 0; line <= count; ++line) {
        bool opened = false;
        for (; next < names.count && names.data[next].line == line; ++next) {
            prTextBufferAppend(out, opened ? " " : line != 0 ? " [" : "[");
            prTextBufferAppend(out, prLayoutGridNameText(r->tree, names.data[next].name));
            opened = true;
        }
        if (opened)
            prTextBufferAppendChar(out, ']');
        if (line == count)
            break;
        if (line != 0 || opened)
            prTextBufferAppendChar(out, ' ');
        cssSerializeNumber(out, double(prLayoutUnitToPx(sizes.data[line])));
        prTextBufferAppend(out, "px");
    }
    sizes.reset();
    names.reset();
}

// Returns the extent in px of the reference box that the box's transform-box names (css-transforms-1/#transform-box).
PrVec2 referenceBoxOf(const PrCssPresentation* r, PrLayoutNode node) {
    PrLayoutStyle record = prLayoutStyleRecord(r->tree, prLayoutNodeStyle(r->tree, node));
    PrLayoutEffectStyle effects = prLayoutEffectStyleRecord(r->tree, record.groups[PrLayoutGroup_Effect]);
    if (effects.transformBox == PrLayoutTransformBox_ContentBox || effects.transformBox == PrLayoutTransformBox_FillBox)
        return prLayoutVecToPx(prLayoutNodeContentExtent(r->tree, node));
    PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(r->tree, node, PrLayoutCoordinates_Layout));
    return PrVec2{bounds.right - bounds.left, bounds.bottom - bounds.top};
}

// Writes `m` as matrix() if it is a 2D matrix, and as matrix3d() otherwise
// (css-transforms-2/#serialization-of-the-computed-value).
void serializeMatrix(PrTextBuffer* out, const PrMatrix4& m) {
    bool flat = m.m[2][0] == 0 && m.m[2][1] == 0 && m.m[0][2] == 0 && m.m[1][2] == 0 && m.m[2][2] == 1 &&
        m.m[3][0] == 0 && m.m[3][1] == 0 && m.m[3][2] == 0 && m.m[3][3] == 1 && m.m[2][3] == 0;
    // A matrix's arguments go column by column. matrix() leaves out the third row and column, and the fourth row.
    prTextBufferAppend(out, flat ? "matrix(" : "matrix3d(");
    bool first = true;
    for (uint32_t column = 0; column < 4; ++column) {
        if (flat && column == 2)
            continue;
        for (uint32_t row = 0; row < (flat ? 2u : 4u); ++row) {
            if (!first)
                prTextBufferAppend(out, ", ");
            first = false;
            // A negative zero is written as zero.
            cssSerializeNumber(out, double(m.m[row][column]) + 0.0);
        }
    }
    prTextBufferAppendChar(out, ')');
}

// The error that converting a length's unit to px can leave. Snapping must not round such a value down past a whole
// target pixel: 72pt is 96px, not 95.
constexpr double kSnapTolerance = 1e-6;

// Returns `px` snapped as a border width at `ratio` target pixels per px (css-values-4/#snap-as-a-line-width): a
// magnitude above zero and under one target pixel is one, and a greater one is rounded down to whole target pixels, the
// sign kept. Layout snaps the widths and offsets that it is given in the same way, so this is the length it uses.
double snapBorderWidth(double px, float ratio) {
    double pixels = prF64Abs(px) * double(ratio);
    if (pixels == 0)
        return 0;
    double snapped = (pixels < 1 ? 1 : prF64Floor(pixels + kSnapTolerance)) / double(ratio);
    return px < 0 ? -snapped : snapped;
}

// Returns true if the computed value of `property` is snapped as a border width: a <line-width>
// (css-backgrounds-3/#typedef-line-width), or outline-offset (w3c/csswg-drafts#12906).
bool snapsAsBorderWidth(PrCssProperty property) {
    return property == PrCssProperty_BorderTopWidth || property == PrCssProperty_BorderRightWidth ||
        property == PrCssProperty_BorderBottomWidth || property == PrCssProperty_BorderLeftWidth ||
        property == PrCssProperty_OutlineWidth || property == PrCssProperty_ColumnRuleWidth ||
        property == PrCssProperty_OutlineOffset;
}

// Serializes the resolved value of `property` (cssom-1/#resolved-values). For width and height, this is the used value
// if the element generates a box that those properties apply to, which is every box except an inline box
// (CSS2/#the-width-property). For transform-origin and perspective-origin, it is the used offset in pixels from the
// reference box's top left, if the element generates a box that is not an inline box. For a border width, it is the
// used width, which the border's style decides (css-backgrounds-3/#border-width). A border width, an outline's width or
// offset, or a column rule's width is snapped as a border width at the media environment's resolution, the tree's
// target pixel ratio, as its computed value is specified to be. Layout snaps the unsnapped value that styles compute in
// the same way. For a margin or padding, it is the used length if the computed value is a percentage or a calculation.
// For grid-template-columns and grid-template-rows, it is the used track list of a grid container. For line-height, it
// is the used length, a number times the font size, or normal. For transform, it is the matrix that the list composes
// to over the reference box, which is empty if the element generates no box that is not an inline box. For every other
// property, it is the computed value.
void serializeResolved(PrTextBuffer* out, PrCssPresentation* r, const CssComputedStyle* style, PrCssProperty property,
        PrLayoutNode node) {
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        if (property == kBorderWidths[side] && !borderStyled(style, side)) {
            prTextBufferAppend(out, "0px");
            return;
        }
    }
    if (snapsAsBorderWidth(property)) {
        CssValueRun length = cssComputedValue(style, property);
        if (length.count == 1 && length.values[0].kind == PrCssValueKind_Dimension &&
                length.values[0].unit == PrCssUnit_Px) {
            float ratio = cssMediaEnvironmentPixelRatio(r->document->media);
            cssSerializeNumber(out, snapBorderWidth(length.values[0].number, ratio));
            prTextBufferAppend(out, "px");
            return;
        }
    }
    // A computed letter-spacing of zero resolves to normal, for legacy reasons (css-text-4/#letter-spacing-property).
    if (property == PrCssProperty_LetterSpacing) {
        CssValueRun spacing = cssComputedValue(style, PrCssProperty_LetterSpacing);
        if (spacing.count == 1 && spacing.values[0].kind == PrCssValueKind_Dimension && spacing.values[0].number == 0) {
            prTextBufferAppend(out, "normal");
            return;
        }
    }
    if (property == PrCssProperty_LineHeight) {
        CssValueRun lineHeight = cssComputedValue(style, PrCssProperty_LineHeight);
        if (lineHeight.count == 1 && lineHeight.values[0].kind == PrCssValueKind_Number) {
            cssSerializeNumber(out, cssStyleLineHeight(style));
            prTextBufferAppend(out, "px");
            return;
        }
    }
    bool hasBox = node != kPrLayoutNodeNone && (prLayoutNodeKind(r->tree, node) == PrLayoutNodeKind_Replaced ||
        !prLayoutDisplayIsInlineBox(displayOf(cssComputedValue(style, PrCssProperty_Display))));
    if (property == PrCssProperty_Transform) {
        CssValueRun transform = cssComputedValue(style, PrCssProperty_Transform);
        if (transform.count == 0 || cssValueIsKeyword(transform.values, PrCssKeyword_None)) {
            prTextBufferAppend(out, "none");
            return;
        }
        CssValueRun empty{transform.values, 0, transform.text};
        PrLayoutTransformList list = cssTransformFunctions(empty, empty, empty, transform);
        serializeMatrix(out, prLayoutTransformListMatrix(&list, hasBox ? referenceBoxOf(r, node) : PrVec2{0, 0}));
        return;
    }
    if ((property == PrCssProperty_TransformOrigin || property == PrCssProperty_PerspectiveOrigin) && hasBox) {
        PrLayoutStyle record = prLayoutStyleRecord(r->tree, prLayoutNodeStyle(r->tree, node));
        PrLayoutEffectStyle effects = prLayoutEffectStyleRecord(r->tree, record.groups[PrLayoutGroup_Effect]);
        PrVec2 extent = referenceBoxOf(r, node);
        bool transform = property == PrCssProperty_TransformOrigin;
        double origin[2] = {};
        if (transform) {
            origin[0] = double(prLayoutUnitToPx(
                prLayoutLengthResolve(r->tree, effects.originX, prLayoutUnitFromPx(extent.x))));
            origin[1] = double(prLayoutUnitToPx(
                prLayoutLengthResolve(r->tree, effects.originY, prLayoutUnitFromPx(extent.y))));
        } else {
            positionPx(cssComputedValue(style, PrCssProperty_PerspectiveOrigin), extent, origin);
        }
        cssSerializeNumber(out, origin[0]);
        prTextBufferAppend(out, "px ");
        cssSerializeNumber(out, origin[1]);
        prTextBufferAppend(out, "px");
        if (transform && effects.originZ != 0) {
            prTextBufferAppend(out, " ");
            cssSerializeNumber(out, double(effects.originZ));
            prTextBufferAppend(out, "px");
        }
        return;
    }
    // A margin or padding that is a percentage or a calculation resolves against the containing block's inline size
    // that the box's layout read (cssom-1/#resolved-values). An auto margin stays auto.
    bool padding = false;
    uint32_t side = edgeSideOf(property, &padding);
    if (side != PrRectSide_Count && hasBox) {
        PrLayoutUnit base = prLayoutNodeContainingInlineSize(r->tree, node);
        PrLayoutStyle record = prLayoutStyleRecord(r->tree, prLayoutNodeStyle(r->tree, node));
        PrLayoutBoxStyle box = prLayoutBoxStyleRecord(r->tree, record.groups[PrLayoutGroup_Box]);
        PrLayoutLength length = padding ? box.padding[side] : box.margin[side];
        PrLayoutWordKind kind = prLayoutWordKind(length.bits);
        if (base >= PrLayoutUnit{0} && (kind == PrLayoutWordKind_Percent || kind == PrLayoutWordKind_Compound)) {
            cssSerializeNumber(out, double(prLayoutUnitToPx(prLayoutLengthResolve(r->tree, length, base))));
            prTextBufferAppend(out, "px");
            return;
        }
    }
    bool gridTracks = property == PrCssProperty_GridTemplateColumns || property == PrCssProperty_GridTemplateRows;
    if (gridTracks && hasBox && prLayoutNodeKind(r->tree, node) != PrLayoutNodeKind_Replaced) {
        PrLayoutStyle record = prLayoutStyleRecord(r->tree, prLayoutNodeStyle(r->tree, node));
        PrLayoutDisplay display = prLayoutBoxStyleRecord(r->tree, record.groups[PrLayoutGroup_Box]).display;
        if (display == PrLayoutDisplay_Grid || display == PrLayoutDisplay_InlineGrid) {
            bool columns = property == PrCssProperty_GridTemplateColumns;
            serializeTrackList(out, r, node, columns ? PrLayoutGridAxis_Columns : PrLayoutGridAxis_Rows);
            return;
        }
    }
    if (resolvesFromLayout(property) && hasBox && !gridTracks && side == PrRectSide_Count) {
        PrLayoutStyle record = prLayoutStyleRecord(r->tree, prLayoutNodeStyle(r->tree, node));
        PrLayoutBoxStyle box = prLayoutBoxStyleRecord(r->tree, record.groups[PrLayoutGroup_Box]);
        bool width = property == PrCssProperty_Width;
        // The size of the box that box-sizing names: the border box, or the content box, whose padding layout resolved
        // against the containing block.
        float used = 0;
        if (box.boxSizing == PrLayoutBoxSizing_BorderBox) {
            PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(r->tree, node, PrLayoutCoordinates_Layout));
            used = width ? bounds.right - bounds.left : bounds.bottom - bounds.top;
        } else {
            PrVec2 content = prLayoutVecToPx(prLayoutNodeContentExtent(r->tree, node));
            used = width ? content.x : content.y;
        }
        cssSerializeNumber(out, double(used > 0 ? used : 0));
        prTextBufferAppend(out, "px");
        return;
    }
    // A color's resolved value is its used value: a keyword resolves to sRGB, written with rgb(), and a function that
    // reads the element resolves in its own space (cssom-1/#resolved-values).
    CssValueRun computed = cssComputedValue(style, property);
    // A caret-color of auto is the element's color (css-ui-4/#caret-color).
    bool automatic = computed.count == 1 && cssValueIsKeyword(computed.values, PrCssKeyword_Auto);
    if (property == PrCssProperty_CaretColor && automatic)
        computed = cssComputedValue(style, PrCssProperty_Color);
    // light-dark() resolves to the color that the used color scheme chooses (css-color-5/#light-dark), which resolves
    // as if it were written alone.
    while (cssIsColorProperty(property) && computed.count != 0 &&
            cssValueIsFunction(computed.values, PrCssKeyword_LightDark)) {
        uint32_t at = 1;
        if (cssUsesDarkScheme(style, colorEnvironmentOf(r, 0, nullptr)))
            at += computed.values[at].next + 1;
        computed = cssValueSubtree(computed, at);
    }
    CssColor color{};
    bool resolvable = computed.count != 0 && (computed.values[0].kind == PrCssValueKind_Keyword ||
        computed.values[0].kind == PrCssValueKind_Function);
    if (cssIsColorProperty(property) && resolvable &&
            cssResolveColorComponents(computed, 0, cssStyleColorContext(style, colorEnvironmentOf(r, 0, nullptr)), &color)) {
        CssValuePool pool{};
        if (computed.values[0].kind == PrCssValueKind_Keyword) {
            PrColor srgb = cssColorToSrgb(color);
            auto channel = [](float c) { return uint8_t(prF32Clamp(c, 0, 1) * 255 + 0.5f); };
            cssValuePoolAddColor(&pool, PrColor8{channel(srgb.r), channel(srgb.g), channel(srgb.b), channel(srgb.a)});
        } else {
            cssValuePoolAddColorComponents(&pool, color);
        }
        cssSerializeValues(out, cssValuePoolRun(&pool, CssValueSpan{0, pool.values.count}));
        cssValuePoolReset(&pool);
        return;
    }
    // drop-shadow()'s currentcolor resolves to the element's color, as a color property's does.
    bool filters = property == PrCssProperty_Filter || property == PrCssProperty_BackdropFilter;
    bool current = false;
    for (uint32_t i = 0; filters && i < computed.count && !current; ++i)
        current = cssValueIsKeyword(&computed.values[i], PrCssKeyword_Currentcolor);
    if (current) {
        PrColor srgb = cssStyleColor(style, colorEnvironmentOf(r, 0, nullptr));
        auto channel = [](float c) { return uint8_t(prF32Clamp(c, 0, 1) * 255 + 0.5f); };
        PrColor8 resolved{channel(srgb.r), channel(srgb.g), channel(srgb.b), channel(srgb.a)};
        CssValuePool pool{};
        for (uint32_t i = 0; i < computed.count; i += computed.values[i].next) {
            if (computed.values[i].kind != PrCssValueKind_Function) {
                cssValuePoolAddRun(&pool, cssValueSubtree(computed, i));
                continue;
            }
            uint32_t at = cssValuePoolAdd(&pool, computed.values[i]);
            for (uint32_t k = i + 1; k < i + computed.values[i].next; k += computed.values[k].next) {
                if (cssValueIsKeyword(&computed.values[k], PrCssKeyword_Currentcolor))
                    cssValuePoolAddColor(&pool, resolved);
                else
                    cssValuePoolAddRun(&pool, cssValueSubtree(computed, k));
            }
            pool.values.data[at].next = pool.values.count - at;
        }
        cssSerializeValues(out, CssValueRun{pool.values.data, pool.values.count, computed.text});
        cssValuePoolReset(&pool);
        return;
    }
    cssSerializeValues(out, computed);
}

// Returns true if a computed overflow is visible along both axes.
bool overflowsVisibly(const CssComputedStyle* style) {
    return overflowOf(cssComputedValue(style, PrCssProperty_OverflowX)) == PrLayoutOverflow_Visible &&
        overflowOf(cssComputedValue(style, PrCssProperty_OverflowY)) == PrLayoutOverflow_Visible;
}

// Returns true if any containment applies to an element styled by `style` (css-contain-2/#containment-types). The
// containment can come from contain, from a content-visibility other than visible, or from a container-type that sizes
// the element.
bool isContained(const CssComputedStyle* style) {
    return cssComputedFirstKeyword(style, PrCssProperty_Contain) != PrCssKeyword_None ||
        cssComputedFirstKeyword(style, PrCssProperty_ContentVisibility) != PrCssKeyword_Visible ||
        cssComputedFirstKeyword(style, PrCssProperty_ContainerType) != PrCssKeyword_Normal;
}

// Sets the tree's canvas color (prLayoutTreeSetCanvasColor): the Canvas system color in the root's used color scheme
// for a top-level document, and for a frame whose root uses another scheme than the frame's element, which is the
// preferred scheme. Any other frame's canvas is transparent, so that its embedder shows through. Sets `reads` if the
// canvas read the preferred scheme.
void setCanvasColor(PrCssPresentation* r, const CssComputedStyle* root, bool* reads) {
    CssColorEnvironment environment = colorEnvironmentOf(r, r->headroom, reads);
    CssValueNode canvas = cssKeywordValue(PrCssKeyword_Canvas);
    PrColor color{1, 1, 1, 1};
    cssResolveColor(CssValueRun{&canvas, 1, nullptr}, 0, cssStyleColorContext(root, environment), &color);
    if (r->embedding == PrCssPresentationEmbedding_Frame) {
        *reads = true;
        if (cssUsesDarkScheme(root, environment) == r->prefersDark)
            color = PrColor{0, 0, 0, 0};
    }
    prLayoutTreeSetCanvasColor(r->tree, prColor16FromColor(color));
}

// Sets the records of the root and the body. Each projects its own style, except for three things that an HTML root
// takes from its body. The root takes the body's background if it has none of its own, and the canvas paints it instead
// of the body (css-backgrounds-3/#body-background). The root takes the body's overflow if its own is visible, and the
// overflow becomes the viewport's and clips nothing of the body (css-overflow-3/#overflow-propagation). The root takes
// the body's writing-mode and direction, which are the principal writing mode, as its used values
// (css-writing-modes-4/#principal-flow). Containment on either element disables all three
// (css-contain-2/#containment-types).
void projectCanvas(PrCssPresentation* r, PrDomElement* root, PrDomElement* PR_NULLABLE body) {
    uint32_t rootAt = cssPresentationNodeEntry(r, root);
    uint32_t bodyAt = body != nullptr ? cssPresentationNodeEntry(r, body) : kHashMissing;
    if (rootAt == kHashMissing || r->entries.data[rootAt].layoutNode == kPrLayoutNodeNone)
        return;
    CssPresentationEntry& rootEntry = r->entries.data[rootAt];
    rootEntry.readsOutside = false;
    // If the body has no box, or containment applies, the root takes nothing from the body, whatever it took before.
    if (bodyAt == kHashMissing || r->entries.data[bodyAt].layoutNode == kPrLayoutNodeNone ||
            isContained(rootEntry.style) || isContained(r->entries.data[bodyAt].style)) {
        if (bodyAt != kHashMissing && r->entries.data[bodyAt].layoutNode != kPrLayoutNodeNone) {
            CssPresentationEntry& bodyEntry = r->entries.data[bodyAt];
            bodyEntry.readsOutside = false;
            r->projecting = body;
            prLayoutNodeSetStyle(r->tree, bodyEntry.layoutNode,
                project(r, bodyEntry.style, Presentation{}, bodyEntry.composited, &bodyEntry.readsOutside));
            r->readsOutside = r->readsOutside || bodyEntry.readsOutside;
        }
        r->projecting = root;
        setCanvasColor(r, rootEntry.style, &rootEntry.readsOutside);
        Presentation alone{.fillsViewport = domIsOuterSvg(root)};
        PrLayoutStyleIndex rootRecord =
            project(r, rootEntry.style, alone, rootEntry.composited, &rootEntry.readsOutside);
        prLayoutNodeSetStyle(r->tree, rootEntry.layoutNode, rootRecord);
        r->readsOutside = r->readsOutside || rootEntry.readsOutside;
        return;
    }
    CssPresentationEntry& bodyEntry = r->entries.data[bodyAt];
    // The canvas reads both boxes' backgrounds, so a headroom that either read projects both again.
    bool reads = false;
    PrColor background{};
    PrColor bodyBackground{};
    CssColorEnvironment environment = colorEnvironmentOf(r, r->headroom, &reads);
    cssResolveColor(cssComputedValue(rootEntry.style, PrCssProperty_BackgroundColor), 0,
        cssStyleColorContext(rootEntry.style, environment), &background);
    cssResolveColor(cssComputedValue(bodyEntry.style, PrCssProperty_BackgroundColor), 0,
        cssStyleColorContext(bodyEntry.style, environment), &bodyBackground);
    bool propagates = background.a == 0 && hasNoImages(cssComputedValue(rootEntry.style,
        PrCssProperty_BackgroundImage)) && (bodyBackground.a != 0 ||
            !hasNoImages(cssComputedValue(bodyEntry.style, PrCssProperty_BackgroundImage)));
    bool scrolls = overflowsVisibly(rootEntry.style) && !overflowsVisibly(bodyEntry.style);
    Presentation rootShare{
        .background = propagates ? bodyEntry.style : nullptr,
        .overflow = scrolls ? bodyEntry.style : nullptr,
        .flow = bodyEntry.style,
    };
    setCanvasColor(r, rootEntry.style, &reads);
    Presentation bodyShare{.dropBackground = propagates, .dropOverflow = scrolls};
    r->projecting = root;
    prLayoutNodeSetStyle(r->tree, rootEntry.layoutNode, project(r, rootEntry.style, rootShare, rootEntry.composited, &reads));
    r->projecting = body;
    prLayoutNodeSetStyle(r->tree, bodyEntry.layoutNode, project(r, bodyEntry.style, bodyShare, bodyEntry.composited, &reads));
    rootEntry.readsOutside = reads;
    bodyEntry.readsOutside = reads;
    r->readsOutside = r->readsOutside || reads;
}

// Returns the style that an entry's node presents now, with one reference that the caller holds. This is its cached
// style, with the composited animations that a compositor shows sampled at the presentation's time.
const CssComputedStyle* presentedStyle(PrCssPresentation* r, uint32_t at) {
    const CssPresentationEntry& entry = r->entries.data[at];
    if (!entry.composited) {
        cssComputedStyleRetain(entry.style);
        return entry.style;
    }
    const CssComputedStyle* base = entry.base != nullptr ? entry.base : entry.style;
    return cssAnimateApply(r, entry.node, base, true, colorEnvironmentOf(r, r->headroom, nullptr));
}

// Returns the entry of the pseudo-element `pseudo` of the element at `at`, if the presentation keeps a style for it, or
// kHashMissing.
uint32_t keptPseudoOf(const PrCssPresentation* r, uint32_t at, CssPseudo pseudo) {
    if (pseudo == CssPseudo_FirstLine || pseudo == CssPseudo_FirstLetter)
        return kHashMissing;
    CssPseudo holder = pseudo;
    if (pseudo == CssPseudo_BeforeMarker)
        holder = CssPseudo_Before;
    else if (pseudo == CssPseudo_AfterMarker)
        holder = CssPseudo_After;
    const CssPresentationPseudo* kept = r->entries.data[at].pseudos[holder - CssPseudo_Before];
    uint32_t own = kept != nullptr ? cssPresentationNodeEntry(r, kept) : kHashMissing;
    if (own != kHashMissing && holder != pseudo) {
        kept = r->entries.data[own].pseudos[kMarkerPseudo];
        own = kept != nullptr ? cssPresentationNodeEntry(r, kept) : kHashMissing;
    }
    return own != kHashMissing && r->entries.data[own].style != nullptr ? own : kHashMissing;
}

// Returns the style of an entry's element as presented, or of its pseudo-element `pseudo`, with one reference that the
// caller holds. A pseudo-element's style is the one the presentation keeps, with its animations, or else is computed on
// request under the presented style of what originates it.
const CssComputedStyle* targetStyle(PrCssPresentation* r, uint32_t at, CssPseudo pseudo) {
    if (pseudo != CssPseudo_None) {
        uint32_t kept = keptPseudoOf(r, at, pseudo);
        if (kept != kHashMissing)
            return presentedStyle(r, kept);
    }
    const CssComputedStyle* style = presentedStyle(r, at);
    if (pseudo == CssPseudo_None)
        return style;
    startThreads(r);
    auto cascadePseudo = [&](const CssComputedStyle* parent, CssPseudo target) {
        CssCascadeInput input{
            .element = static_cast<const PrDomElement*>(r->entries.data[at].node),
            .parent = parent,
            .rootFont = r->rootFont,
            .viewport = viewportOf(r),
            .fresh = false,
            .matched = nullptr,
            .pseudo = target,
        };
        const CssComputedStyle* made =
            cssCascadeStyle(&r->rules, &r->threads.data[0].builder, &r->threads.data[0].scratch, input);
        cssComputedStyleRelease(parent);
        return made;
    };
    if (pseudo == CssPseudo_BeforeMarker)
        style = cascadePseudo(style, CssPseudo_Before);
    else if (pseudo == CssPseudo_AfterMarker)
        style = cascadePseudo(style, CssPseudo_After);
    else if (pseudo == CssPseudo_FirstLetter)
        style = cascadePseudo(style, CssPseudo_FirstLine);
    return cascadePseudo(style, pseudo);
}

// The properties that a computed style lists, in the order of their names.
struct ComputedOrder {
    PrCssProperty properties[PrCssProperty_Count];
    uint32_t count;
};

ComputedOrder buildComputedOrder() {
    ComputedOrder order{};
    for (uint32_t g = 0; g < CssStyleGroup_Count; ++g) {
        CssStyleGroupProperties group = cssStyleGroupProperties(CssStyleGroup(g));
        for (uint32_t i = 0; i < group.count && order.count < countOf(order.properties); ++i)
            order.properties[order.count++] = group.properties[i];
    }
    sortItems(order.properties, order.count, [](PrCssProperty a, PrCssProperty b) {
        return prStringCompare(cssPropertyName(a), cssPropertyName(b)) < 0;
    });
    return order;
}

const ComputedOrder& computedOrder() {
    static const ComputedOrder order = buildComputedOrder();
    return order;
}

// Returns the root's cached style, which rem units are relative to.
const CssComputedStyle* rootStyleOf(const PrCssPresentation* r, const PrDomElement* root) {
    uint32_t at = cssPresentationNodeEntry(r, root);
    PR_ASSERT(at != kHashMissing && r->entries.data[at].style != nullptr);
    return r->entries.data[at].style;
}

// Appends a short path to `node` for a report: each element's name, ID and first class, from the root down.
void describeNode(PrTextBuffer* out, const PrDomNode* node) {
    if (isPseudo(node)) {
        describeNode(out, node->parent);
        CssPseudo pseudo = static_cast<const CssPresentationPseudo*>(node)->pseudo;
        prTextBufferAppend(out, pseudo == CssPseudo_Before ? "::before" : pseudo == CssPseudo_After ? "::after" : "::marker");
        return;
    }
    const PrDomElement* element = domElementFromNode(const_cast<PrDomNode*>(node));
    if (element == nullptr) {
        if (node->parent != nullptr) {
            describeNode(out, node->parent);
            prTextBufferAppend(out, " > ");
        }
        prTextBufferAppend(out, "#text");
        return;
    }
    if (node->parent != nullptr && domElementFromNode(node->parent) != nullptr) {
        describeNode(out, node->parent);
        prTextBufferAppend(out, " > ");
    }
    prTextBufferAppend(out, domElementName(element));
    PrStringView id = domElementId(element);
    if (id.size != 0) {
        prTextBufferAppendChar(out, '#');
        prTextBufferAppend(out, id);
    }
    if (element->classes != kDomClassesNone) {
        DomClassList classes = domClassesList(element->document, element->classes);
        if (classes.count != 0) {
            prTextBufferAppendChar(out, '.');
            prTextBufferAppend(out, domClassesName(element->document, classes.atoms[0]));
        }
    }
}

// The first difference prCssPresentationVerify found.
struct Difference {
    const PrDomNode* PR_NULLABLE node;
    PrTextBuffer what;
};

// Records the first difference, which is `what` at `node`.
bool differs(Difference* out, const PrDomNode* node, const char* what) {
    out->node = node;
    prTextBufferAppend(&out->what, what);
    return false;
}

// The names of the style groups, for reports.
constexpr const char* kGroupNames[] = {
    "font",      "color",     "box",  "flex", "decoration",   "background",
    "effect",    "compositing", "animation", "generated", "list", "svg",
    "svg geometry", "table", "typography", "grid",
};
static_assert(countOf(kGroupNames) == CssStyleGroup_Count);

// Appends the first property of `group` whose value differs between the cached style and a restyle from scratch, with
// both values.
void describeGroupDifference(PrTextBuffer* out, CssStyleGroup group, const CssComputedStyle* cached,
        const CssComputedStyle* fresh) {
    CssStyleGroupProperties properties = cssStyleGroupProperties(group);
    for (uint32_t i = 0; i < properties.count; ++i) {
        PrCssProperty property = properties.properties[i];
        PrTextBuffer was{};
        PrTextBuffer now{};
        cssSerializeValues(&was, cssComputedValue(cached, property));
        cssSerializeValues(&now, cssComputedValue(fresh, property));
        bool differ = prTextBufferView(&was) != prTextBufferView(&now);
        if (differ) {
            prTextBufferAppend(out, ", ");
            prTextBufferAppend(out, cssPropertyName(property));
            prTextBufferAppend(out, ": cached ");
            prTextBufferAppend(out, prTextBufferView(&was));
            prTextBufferAppend(out, ", fresh ");
            prTextBufferAppend(out, prTextBufferView(&now));
        }
        prTextBufferReset(&was);
        prTextBufferReset(&now);
        if (differ)
            return;
    }
}

// Returns true if the pseudo-element `kind` that the entry at `at` holds at `which` of CssPresentationEntry::pseudos
// has the style, the box, the record and the content that a restyle from scratch under `style`, the presented style of
// what originates it, gives it, and its box is at `expected`, which then moves past it. `element` is the originating
// element, and `generating` says whether the pseudo-element may generate a box. `holds` says whether the boxes of the
// element's children have a place. The marker of a ::before or an ::after is checked as its first box.
bool verifyPseudo(PrCssPresentation* r, PrDomElement* element, uint32_t at, uint32_t which, CssPseudo kind,
        const CssComputedStyle* PR_NULLABLE style, bool generating, bool holds, CssFontBases rootFont,
        Difference* out, PrLayoutNode& expected) {
    const CssComputedStyle* fresh = nullptr;
    if (generating) {
        CssCascadeInput input{
            .element = element,
            .parent = style,
            .rootFont = rootFont,
            .viewport = viewportOf(r),
            .fresh = true,
            .matched = nullptr,
            .pseudo = cascadedPseudo(element, kind),
        };
        fresh = cssCascadeStyle(&r->rules, &r->threads.data[0].builder, &r->threads.data[0].scratch, input);
    }
    bool generates = fresh != nullptr && generatesPseudo(element, kind, fresh);
    CssPresentationPseudo* pseudo = r->entries.data[at].pseudos[which];
    uint32_t own = pseudo != nullptr ? cssPresentationNodeEntry(r, pseudo) : kHashMissing;
    PrLayoutNode node = own != kHashMissing ? r->entries.data[own].layoutNode : PrLayoutNode(kPrLayoutNodeNone);
    const char* name = "its ::marker";
    if (kind == CssPseudo_Before)
        name = "its ::before";
    else if (kind == CssPseudo_After)
        name = "its ::after";
    else if (kind == CssPseudo_BeforeMarker)
        name = "its ::before::marker";
    else if (kind == CssPseudo_AfterMarker)
        name = "its ::after::marker";
    bool same = true;
    bool kept = own != kHashMissing && r->entries.data[own].style != nullptr;
    // An element whose box holds no children keeps the styles of its pseudo-elements until its box holds them again,
    // which restyles them.
    if (holds && generates != kept) {
        same = differs(out, element, name);
        prTextBufferAppend(&out->what, generates ? " should have a style and has none" : " has a style it should not have");
    }
    if (same && generates && cssCountsPseudo(r, fresh, kind) && !r->entries.data[own].counts) {
        same = differs(out, element, name);
        prTextBufferAppend(&out->what, " takes part in the counter pass, and is not marked as a participant");
    }
    // The style without animations is what the cascade gives. The style with them is what is presented, and decides the
    // box.
    const CssComputedStyle* cached = kept ? r->entries.data[own].style : nullptr;
    const CssComputedStyle* base = kept && r->entries.data[own].base != nullptr ? r->entries.data[own].base : cached;
    for (uint32_t g = 0; same && generates && g < CssStyleGroup_Count; ++g) {
        if (fresh->groups[g] != base->groups[g]) {
            same = differs(out, element, name);
            prTextBufferAppend(&out->what, ": the computed values of the style group ");
            prTextBufferAppend(&out->what, kGroupNames[g]);
            describeGroupDifference(&out->what, CssStyleGroup(g), base, fresh);
        }
    }
    bool boxes = generates && same && kept && generatesPseudo(element, kind, cached);
    if (same && boxes != (node != kPrLayoutNodeNone)) {
        same = differs(out, element, name);
        prTextBufferAppend(&out->what, boxes ? " should generate a box and has none" : " has a box it should not generate");
    }
    if (same && boxes) {
        bool reads = false;
        PrLayoutStyleIndex record = recordOf(r, pseudo, cached, r->entries.data[own].composited, &reads);
        if (same && record != prLayoutNodeStyle(r->tree, node)) {
            same = differs(out, element, name);
            prTextBufferAppend(&out->what, ": the box's layout record");
        }
        // The content's boxes: its marker's, its runs of text, and a replaced box for each image.
        PrLayoutNode run = prLayoutNodeFirstChild(r->tree, node);
        if (same && (kind == CssPseudo_Before || kind == CssPseudo_After)) {
            CssPseudo marker = kind == CssPseudo_Before ? CssPseudo_BeforeMarker : CssPseudo_AfterMarker;
            same = verifyPseudo(r, element, own, kMarkerPseudo, marker, cached, cssIsListItem(cached), true, rootFont, out, run);
        }
        bool content = true;
        bool contentReads = false;
        eachGenerated(r, cached, kind, element, pseudo->counters, &contentReads, [&](PrStringView text) {
            content = content && run != kPrLayoutNodeNone && prLayoutNodeText(r->tree, run) == text;
            run = run != kPrLayoutNodeNone ? prLayoutNodeNextSibling(r->tree, run) : run;
        }, [&](CssValueRun value) {
            PrStringView reference{};
            CssPresentationImage* loaded = urlOf(value, &reference) ? imageOf(r, reference) : nullptr;
            bool painted = generatedImage(value) || cssValueRunFirstKeyword(value) == PrCssKeyword_None;
            if (!painted && (loaded == nullptr || loaded->state != CssPresentationImageState_Loaded))
                return;
            content = content && run != kPrLayoutNodeNone &&
                prLayoutNodeKind(r->tree, run) == PrLayoutNodeKind_Replaced;
            run = run != kPrLayoutNodeNone ? prLayoutNodeNextSibling(r->tree, run) : run;
        });
        content = content && run == kPrLayoutNodeNone;
        if (same && !content) {
            same = differs(out, element, name);
            prTextBufferAppend(&out->what, ": the boxes of its content");
        }
        if (same && node != expected) {
            same = differs(out, element, name);
            prTextBufferAppend(&out->what, ": the box is not at its place among its parent's boxes");
        }
        if (same)
            expected = prLayoutNodeNextSibling(r->tree, expected);
    }
    cssComputedStyleRelease(fresh);
    return same;
}

// Returns true if the cached styles, boxes and records of the element and its descendants match a restyle from scratch
// under `parentStyle`, and the children of each box are the boxes of the element's children in order. Writes the first
// difference to `out`.
bool verifyElement(PrCssPresentation* r, PrDomElement* element, const CssComputedStyle* PR_NULLABLE parentStyle,
        bool parentBox, CssFontBases rootFont, Difference* out, PrLayoutNode& cursor) {
    const PrDomNode* domNode = element;
    uint32_t at = cssPresentationNodeEntry(r, element);
    if (at == kHashMissing || r->entries.data[at].style == nullptr)
        return differs(out, domNode, "the element has no cached style");
    const CssComputedStyle* fresh = cascade(r, element, parentStyle, rootFont, true);
    // The style without animations is what the cascade gives. The style with them is what is presented and inherited.
    const CssComputedStyle* cached = r->entries.data[at].style;
    const CssComputedStyle* base = r->entries.data[at].base != nullptr ? r->entries.data[at].base : cached;
    bool same = cssCustomPropertiesEqual(fresh->custom, base->custom);
    if (!same)
        differs(out, domNode, "custom properties");
    if (same && fresh->locale != base->locale)
        same = differs(out, domNode, "the locale");
    if (same && fresh->appearance != base->appearance)
        same = differs(out, domNode, "the used appearance");
    if (same && fresh->staticInline != base->staticInline)
        same = differs(out, domNode, "whether its display was inline-level before it was blockified");
    if (same && fresh->childrenAreItems != base->childrenAreItems)
        same = differs(out, domNode, "whether its children are flex or grid items");
    for (uint32_t g = 0; same && g < CssStyleGroup_Count; ++g) {
        if (fresh->groups[g] != base->groups[g]) {
            same = differs(out, domNode, "the computed values of the style group ");
            prTextBufferAppend(&out->what, kGroupNames[g]);
            describeGroupDifference(&out->what, CssStyleGroup(g), base, fresh);
        }
    }
    cssComputedStyleRelease(fresh);
    if (!same)
        return false;
    bool box = (parentBox || isForeignRoot(element)) && generatesBox(element, cached);
    bool contents = parentBox && passesBoxes(element, cached);
    if (contents != r->entries.data[at].contents)
        return differs(out, domNode, "whether its parent's box holds its children's boxes");
    PrLayoutNode node = r->entries.data[at].layoutNode;
    if (box != (node != kPrLayoutNodeNone))
        return differs(out, domNode, box ? "the element should generate a box and has none" : "the element has a box it "
            "should not generate");
    // The records of the root and the body hold the canvas's background, which projectCanvas decides.
    bool canvas = parentStyle == nullptr || element == domDocumentBody(r->document);
    if (box && !canvas) {
        bool reads = false;
        PrLayoutStyleIndex record = project(r, cached, presentationOf(element), r->entries.data[at].composited, &reads);
        if (record != prLayoutNodeStyle(r->tree, node))
            return differs(out, domNode, "the box's layout record");
    }
    bool replaced = isReplaced(element);
    if (box) {
        PrLayoutNodeKind kind = prLayoutNodeKind(r->tree, node);
        if ((kind == PrLayoutNodeKind_Replaced) != replaced ||
                (kind == PrLayoutNodeKind_SemiReplaced) != isSemiReplaced(element))
            return differs(out, domNode, "the box's kind");
        if (replaced) {
            CssPresentationImageDrawing drawing = imageDrawingOf(r, cached);
            PrLayoutNaturalSize natural = prLayoutNodeNaturalSize(r->tree, node);
            PrLayoutNaturalSize expected = naturalOf(r, element, drawing.oriented);
            if (natural.width != expected.width || natural.height != expected.height)
                return differs(out, domNode, "the replaced box's natural size");
            PrLayoutContent content = prLayoutNodeContent(r->tree, node);
            bool sized = expected.width.raw > 0 && expected.height.raw > 0 && embedOf(element) == 0;
            const PrDrawImage* picture = pictureOf(element, drawing.oriented);
            // The painter of an outermost svg element's content, or of an SVG image.
            PrLayoutPainter vector = element->state != nullptr ? element->state->vector : PrLayoutPainter{};
            bool painted = (domIsOuterSvg(element) && presentsSvg(r)) ||
                (embedOf(element) == 0 && !(sized && picture != nullptr) && vector.paint != nullptr);
            if ((content.kind == PrLayoutContentKind_Embed) != (embedOf(element) != 0) ||
                    (content.kind == PrLayoutContentKind_Embed && content.embed != embedOf(element)) ||
                    (content.kind == PrLayoutContentKind_Image) != (sized && picture != nullptr) ||
                    (content.kind == PrLayoutContentKind_Image && content.image != picture) ||
                    content.kind == PrLayoutContentKind_DrawList || (content.kind == PrLayoutContentKind_Painter) != painted ||
                    (content.kind == PrLayoutContentKind_Painter && !domIsOuterSvg(element) &&
                        (content.painter.context != vector.context ||
                            content.revision != element->state->vectorRevision)))
                return differs(out, domNode, "the replaced box's content");
            if (content.kind == PrLayoutContentKind_Image &&
                    (content.limit != drawing.limit || content.rendering != drawing.rendering))
                return differs(out, domNode, "how the replaced box's image is drawn");
        }
    }
    if (box && (prLayoutNodeKind(r->tree, node) == PrLayoutNodeKind_LineBreak) != isLineBreak(element))
        return differs(out, domNode, "the box's kind");
    box = box && holdsChildren(element);
    // The boxes of the children of an element of display contents are children of its parent's box, starting at the
    // place that `cursor` names.
    PrLayoutNode own = box && node != kPrLayoutNodeNone ? prLayoutNodeFirstChild(r->tree, node) : kPrLayoutNodeNone;
    PrLayoutNode& expected = contents ? cursor : own;
    box = box || contents;
    // Every list item has a marker, whose style shows whether it generates a box (css-lists-3/#marker-pseudo), and
    // every text field has the part that holds its text.
    bool shows = box && !unboxesToNothing(element);
    uint32_t generating = shows ? cssCascadeGenerating(&r->rules, &r->threads.data[0].scratch, element) : 0;
    if (shows && cssIsListItem(cached))
        generating |= 1u << CssPseudo_Marker;
    if (box && isTextField(element))
        generating |= 1u << CssPseudo_FieldContent;
    auto pseudoSame = [&](uint32_t which) {
        CssPseudo kind = CssPseudo(CssPseudo_Before + which);
        return verifyPseudo(r, element, at, which, kind, cached, (generating & (1u << kind)) != 0, box, rootFont, out, expected);
    };
    if (!pseudoSame(kMarkerPseudo) || !pseudoSame(kBeforePseudo) || !pseudoSame(kFieldContentPseudo))
        return false;
    DomFlatChildren children = domFlatChildren(element);
    while (PrDomNode* child = domFlatNext(children)) {
        PrDomElement* e = domElementFromNode(child);
        if (e != nullptr) {
            if (!verifyElement(r, e, cached, box && !isTextField(element), rootFont, out, expected))
                return false;
        } else if (!domIsText(child)) {
            continue;
        } else if (box && prStringView(&static_cast<PrDomCharacterData*>(child)->data).size != 0 &&
                layoutNodeOf(r, child) == kPrLayoutNodeNone) {
            return differs(out, child, "the text node should generate a box and has none");
        }
        // A foreignObject's box is a root, among no box's children.
        PrLayoutNode childBox = e != nullptr && isForeignRoot(e) ? PrLayoutNode(kPrLayoutNodeNone) : layoutNodeOf(r, child);
        if (childBox != kPrLayoutNodeNone && domIsText(child)) {
            bool reads = false;
            PrLayoutStyleIndex textStyle = kPrLayoutStyleNone;
            if (contents)
                textStyle = project(r, cached, presentationOf(element), r->entries.data[at].composited, &reads);
            if (prLayoutNodeStyle(r->tree, childBox) != textStyle)
                return differs(out, child, "the record the text is set in");
        }
        if (childBox != kPrLayoutNodeNone) {
            if (childBox != expected)
                return differs(out, child, "the box is not at its place among its parent's boxes");
            expected = prLayoutNodeNextSibling(r->tree, expected);
        }
    }
    if (!pseudoSame(kAfterPseudo))
        return false;
    if (!contents && expected != kPrLayoutNodeNone)
        return differs(out, domNode, "the box has children that no child generates");
    return true;
}

// Returns true if two sets of counter texts are equal.
bool sameCounterTexts(const CssCountingTexts& a, const CssCountingTexts& b) {
    if (a.text.count != b.text.count || a.ends.count != b.ends.count)
        return false;
    return (a.text.count == 0 || prMemEquals(a.text.data, b.text.data, a.text.count)) &&
        (a.ends.count == 0 || prMemEquals(a.ends.data, b.ends.data, a.ends.count * sizeof(uint32_t)));
}

// Returns true if the texts that the counters give each pseudo-element are those that a pass over every box gives. The
// pass walks the boxes as the last restyle left them, which the styles and boxes have been checked against. Writes the
// first difference to `out`.
bool verifyCounters(PrCssPresentation* r, Difference* out) {
    struct Check {
        Difference* out;
        bool same;
    };
    Check check{out, true};
    cssCountersRun(r, CssCountingWalk_Every, [](void* PR_NULLABLE context, CssPresentationPseudo* pseudo,
        const CssCountingTexts* texts) {
            Check* c = static_cast<Check*>(context);
            if (!c->same || sameCounterTexts(pseudo->counters, *texts))
                return;
            c->same = differs(c->out, pseudo, "the texts that counters give its content: cached ");
            prTextBufferAppend(&c->out->what, PrStringView{pseudo->counters.text.data, pseudo->counters.text.count});
            prTextBufferAppend(&c->out->what, ", fresh ");
            prTextBufferAppend(&c->out->what, PrStringView{texts->text.data, texts->text.count});
        }, &check);
    return check.same;
}

// Returns true if the content of each outermost svg element that has a box matches content built from scratch.
bool verifySvgs(PrCssPresentation* r, Difference* out) {
    for (uint32_t i = 0; i < r->svgs.count; ++i) {
        const CssPresentationSvg* svg = r->svgs.data[i];
        uint32_t at = cssPresentationNodeEntry(r, svg->element);
        if (at == kHashMissing || !r->entries.data[at].boxed || svg->functions == nullptr)
            continue;
        PrString report{};
        bool same = svg->functions->verify(svg->content, &report);
        if (!same) {
            differs(out, svg->element, "the content of its SVG tree: ");
            prTextBufferAppend(&out->what, prStringView(&report));
        }
        prStringReset(&report);
        if (!same)
            return false;
    }
    return true;
}

// The minimum number of new elements that a job takes in, so that a load makes progress however long its jobs take.
constexpr uint32_t kMinJobElements = 64;

// The number of new elements that a job takes in per millisecond, which a presentation assumes until its jobs measure
// it.
constexpr double kInitialJobRate = 200;

// Returns the number of new elements that the next job takes in. This is the number that the measured rates allow in
// the budget, using the lower of the rate at which the presentation's thread lays out elements and the rate at which
// the caller styles them.
uint32_t jobShare(const PrCssPresentation* r) {
    if (r->jobBudget <= 0)
        return UINT32_MAX;
    double rate = r->jobRate > 0 ? r->jobRate : kInitialJobRate;
    if (r->restyleRate > 0 && r->restyleRate < rate)
        rate = r->restyleRate;
    double share = rate * r->jobBudget;
    if (share < kMinJobElements)
        return kMinJobElements;
    return share < double(UINT32_MAX - 1) ? uint32_t(share) : UINT32_MAX - 1;
}

// Updates the job rate from the last job, if it took in enough new elements to measure it.
void measureJob(PrCssPresentation* r) {
    if (r->agent->worker == nullptr || r->jobTaken < kMinJobElements)
        return;
    double time = prLayoutWorkerJobTime(r->agent->worker, r->tree);
    if (time <= 0)
        return;
    double rate = double(r->jobTaken) / time;
    r->jobRate = r->jobRate > 0 ? (r->jobRate + rate) / 2 : rate;
}

// How much of the content that is new to the presentation a restyle takes in.
enum {
    RestyleReach_All,               // every new element, which a synchronous query needs
    RestyleReach_Job,               // the rest of the next job's share (newLeft), within the job's budget
};
typedef uint8_t RestyleReach;

// Starts loading the faces that text requested, and brings the presentation's font set up to date with the document's
// fonts (css-fonts-4/#font-face-rule, css-font-loading-3/#font-face-load). The document's fonts are the CSS-connected
// faces of its sheets and the faces added to its FontFaceSet. The set holds each face as pending until it loads. Then
// it holds the face's font, derived by its descriptors, or marks it as failed. A change to the set marks the
// presentation, so that the next layout sees it.
void syncFonts(PrCssPresentation* r) {
    auto keyAt = [&](uint32_t at) { return r->fontFaces.data[at].key; };
    auto addressAt = [&](uint32_t at) { return uint64_t(uintptr_t(r->fontFaces.data[at].face)); };
    uint64_t keys[16];
    uint32_t count = 0;
    while ((count = prFontSetTakeLoads(r->fonts, keys, 16)) != 0) {
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t at = r->fontFaceKeys.find(keys[k], keyAt);
            if (at == kHashMissing)
                continue;
            PrCssFontFace* face = r->fontFaces.data[at].face;
            if (face->status == PrCssFontFaceStatus_Unloaded)
                cssFontFaceLoad(face, r->document);
        }
    }
    CssRoot* style = cssDocumentRoot(r->document);
    if (style->fontsVersion == r->fontsVersion && !r->rulesStale && !r->restyleAll)
        return;
    r->fontsVersion = style->fontsVersion;
    Array<PrCssFontFace* PR_NONNULL> faces;
    cssDocumentFontFaces(r->document, &faces);
    // The faces that left the document leave the set. The indexes are rebuilt over the faces that stay.
    KeyIndex current;
    for (uint32_t i = 0; i < faces.count; ++i)
        current.insert(uint64_t(uintptr_t(faces.data[i])), i);
    bool changed = false;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < r->fontFaces.count; ++i) {
        CssPresentationFontFace& known = r->fontFaces.data[i];
        uint64_t address = uint64_t(uintptr_t(known.face));
        if (current.find(address, [&](uint32_t at) { return uint64_t(uintptr_t(faces.data[at])); }) != kHashMissing) {
            r->fontFaces.data[kept++] = known;
            continue;
        }
        if (known.described)
            prFontSetRemove(r->fonts, known.key);
        cssFontFaceRelease(known.face);
        changed = true;
    }
    current.reset();
    r->fontFaces.count = kept;
    r->fontFaceKeys.rebuild(r->fontFaces.count, keyAt);
    r->fontFaceAddresses.rebuild(r->fontFaces.count, addressAt);
    // The set ranks the faces in the order of the font source (css-font-loading-3/#fontfaceset-set-entries), so that a
    // face added for a rule that was put back ranks at the position of its rule.
    Array<uint64_t> order;
    bool added = false;
    for (uint32_t i = 0; i < faces.count; ++i) {
        PrCssFontFace* face = faces.data[i];
        uint32_t index = r->fontFaceAddresses.find(uint64_t(uintptr_t(face)), addressAt);
        PrString family = prStringNull();
        PrFontFaceDescriptor descriptor{};
        Array<PrFontCodePointRange> ranges;
        bool described = false;
        if (index == kHashMissing) {
            cssFontFaceRetain(face);
            described = cssFontFaceDescribe(face, &family, &descriptor, &ranges);
            index = r->fontFaces.count;
            uint64_t key = ++r->nextFontFaceKey;
            r->fontFaces.push(CssPresentationFontFace{
                .face = face,
                .key = key,
                .status = PrCssFontFaceStatus_Unloaded,
                .described = described,
            });
            r->fontFaceKeys.insert(key, index);
            r->fontFaceAddresses.insert(uint64_t(uintptr_t(face)), index);
            if (described) {
                prFontSetAddPending(r->fonts, &descriptor, key);
                // A face whose source the document has already decoded is set immediately, with no frame of fallback.
                // Its load, which takes the same font, updates its status in a task.
                if (const PrFont* cached = cssFontFaceCachedFont(face, r->document)) {
                    prFontSetLoaded(r->fonts, key, cached);
                    cssFontFaceLoad(face, r->document);
                }
            }
            changed = changed || described;
            added = added || described;
        }
        CssPresentationFontFace& known = r->fontFaces.data[index];
        if (known.described)
            order.push(known.key);
        if (known.described && face->status != known.status) {
            known.status = face->status;
            if (face->status == PrCssFontFaceStatus_Loaded) {
                // The set derives the face's font from its descriptors.
                prFontSetLoaded(r->fonts, known.key, face->font);
                changed = true;
            } else if (face->status == PrCssFontFaceStatus_Error) {
                prFontSetFailed(r->fonts, known.key);
                changed = true;
            }
        }
        prStringReset(&family);
        ranges.reset();
    }
    if (added)
        prFontSetOrder(r->fonts, order.data, order.count);
    order.reset();
    r->unloadedFaces = 0;
    r->unloadedFacesVersion = style->fontSetVersion;
    for (uint32_t i = 0; i < faces.count; ++i)
        r->unloadedFaces += faces.data[i]->status == PrCssFontFaceStatus_Unloaded ? 1 : 0;
    cssFontsSourceSynced(r->document, faces);
    faces.reset();
    if (!changed)
        return;
    r->changed = true;
    // The content of an svg element sets its text when it is built, in the fonts that the set held then.
    svgInvalidate(r);
    // A style that measured a font measured what the set held then.
    if (r->readsMetrics) {
        for (uint32_t i = 0; i < r->entries.count; ++i) {
            const CssPresentationEntry& entry = r->entries.data[i];
            if (entry.node != nullptr && entry.style != nullptr &&
                    (entry.style->readsFontMetrics || entry.style->readsRootMetrics))
                mark(seen(const_cast<PrDomNode*>(entry.node)), DomStyleFlag_Self);
        }
    }
}

// Sets the texts that counters give the content of a pseudo-element, if they changed, and queues the content of its box
// to be made again with them.
void setCounterTexts(void* PR_NULLABLE context, CssPresentationPseudo* pseudo, const CssCountingTexts* texts) {
    PrCssPresentation* r = static_cast<PrCssPresentation*>(context);
    CssCountingTexts& kept = pseudo->counters;
    if (sameCounterTexts(kept, *texts))
        return;
    kept.text.clear();
    kept.text.append(texts->text.data, texts->text.count);
    kept.ends.clear();
    kept.ends.append(texts->ends.data, texts->ends.count);
    uint32_t at = cssPresentationNodeEntry(r, pseudo);
    if (r->entries.data[at].boxed)
        queueBox(r, at, CssPresentationBoxOp_Content);
}

// Notes that the document's counter styles changed, which changes the text of every counter and marker. The next pass
// starts at the root, and makes every marker string again.
void counterStylesChanged(PrCssPresentation* r) {
    cssCountersChanged(r, nullptr);
    for (uint32_t i = 0; i < r->entries.count; ++i) {
        const PrDomNode* node = r->entries.data[i].node;
        if (node != nullptr && isPseudo(node))
            static_cast<CssPresentationPseudo*>(const_cast<PrDomNode*>(node))->markerKept = false;
    }
}

// Runs the counter pass over the participants, after a restyle that added, removed or changed one.
void updateCounters(PrCssPresentation* r) {
    r->countersStale = false;
    cssCountersRun(r, CssCountingWalk_Participants, setCounterTexts, r);
    r->accrued.counterSteps += r->counterScratch.steps.count;
}

// Brings styles up to date with the document, for a viewport of `extent`, decides which nodes have boxes, and queues
// the resulting changes to the tree (applyBoxes). It neither reads nor writes the tree, so it can run while the
// presentation's thread holds the tree. New elements beyond `reach` wait for a later restyle, which the presentation
// keeps as pending work. Returns true if anything changed, which leaves the tree out of date.
bool restyle(PrCssPresentation* r, PrLayoutVec extent, RestyleReach reach) {

    syncFonts(r);
    bool resized = extent != r->extent;
    if (!r->changed && !resized)
        return false;
    // The shadow trees of the uses that the document's changes reached are brought up to date, which marks what they
    // change for the walk. A change that makes a use stale is a change of the document, which the presentation has
    // heard.
    domUsesSync(r->document);
    if (r->mediaStale && !r->restyleAll && cssRuleSetMediaChanged(&r->rules, r->document->media))
        r->restyleAll = true;
    r->mediaStale = false;
    // A new headroom or preferred scheme projects again the boxes whose records read state outside their styles, and
    // only those. New schemes for the page project every box again.
    float headroom = cssMediaEnvironmentHeadroom(r->document->media);
    bool prefersDark = cssMediaEnvironmentPrefersDark(r->document->media);
    CssColorSchemeFlags pageSchemes = r->pageSchemes;
    if (r->schemesStale) {
        r->schemesStale = false;
        pageSchemes = cssPageSchemes(r->document);
    }
    bool everyBox = pageSchemes != r->pageSchemes;
    bool outsideChanged = headroom != r->headroom || prefersDark != r->prefersDark;
    bool reproject = everyBox || (outsideChanged && r->readsOutside);
    r->headroom = headroom;
    r->prefersDark = prefersDark;
    r->pageSchemes = pageSchemes;
    // The svg elements draw their colors for the display's headroom, the user's preferred scheme and the page's
    // schemes.
    if (outsideChanged || everyBox)
        svgInvalidate(r);
    for (uint32_t i = 0; reproject && i < r->entries.count; ++i) {
        const CssPresentationEntry& entry = r->entries.data[i];
        if (entry.node != nullptr &&
                (everyBox || entry.readsOutside || entry.animationReadsOutside || entry.contentReadsOutside)) {
            r->entries.data[i].reprojectPending = true;
            mark(const_cast<PrDomNode*>(r->entries.data[i].node), DomStyleFlag_Self);
        }
    }
    // Viewport units are relative to the extent. If a style reads a viewport unit, a new extent restyles every element,
    // in the same way as new sheets. A change to the document's conditions also restyles every element, because it
    // changes which user agent sheets apply. The parser can set quirks mode after the presentation's first update.
    PrCssSheetConditions conditions = 0;
    if (r->document->quirks == DomQuirks_Yes)
        conditions |= PrCssSheetCondition_Quirks;
    if (r->document->kind == DomDocumentKind_Html)
        conditions |= PrCssSheetCondition_HtmlDocument;
    if (prStringView(&r->document->contentType) == "text/markdown")
        conditions |= PrCssSheetCondition_Markdown;
    if (conditions != r->conditions) {
        r->conditions = conditions;
        r->restyleAll = true;
    }
    bool all = r->restyleAll || (resized && r->readsViewport);
    if (all)
        r->readsViewport = false;
    // The user agent's rules change only with the document's conditions and media, either of which restyles every
    // element. A change to the document's sheets files them again over the same user agent's rules.
    if (r->restyleAll || r->userAgentRules.generation == 0) {
        cssRuleSetBuildUserAgent(&r->userAgentRules, r->document, r->userAgent.data, r->userAgent.count, conditions,
            r->document->media);
    }
    if (r->restyleAll || r->rulesStale) {
        Array<CssTreeSheets> trees;
        treeSheets(r->document, trees);
        Array<const CssRuleData* PR_NONNULL> counterStyles;
        counterStyles.append(r->rules.counterStyleRules.data, r->rules.counterStyleRules.count);
        cssRuleSetBuild(&r->rules, &r->userAgentRules, trees.data, trees.count, r->document->media);
        trees.reset();
        const Array<const CssRuleData* PR_NONNULL>& now = r->rules.counterStyleRules;
        if (now.count != counterStyles.count ||
                !prMemEquals(now.data, counterStyles.data, now.count * sizeof(const CssRuleData*)))
            counterStylesChanged(r);
        counterStyles.reset();
    }
    r->extent = extent;
    r->changed = false;
    r->restyleAll = false;
    r->rulesStale = false;
    startThreads(r);
    for (uint32_t i = 0; i < r->threads.count; ++i)
        cssCascadeScratchForget(&r->threads.data[i].scratch);
    queuedDirections(r, all);
    PrDomElement* root = domDocumentElement(r->document);
    if (root != nullptr) {
        PrDomElement* body = domIsHtml(root, PrDomTag_Html) ? domDocumentBody(r->document) : nullptr;
        // A restyle for a job takes no longer than the job's budget.
        bool job = reach == RestyleReach_Job;
        double deadline = job && r->jobBudget > 0 ? prTimeMonotonicMilliseconds() + r->jobBudget : 0;
        // The restyles of a load are background work, which uses only this thread. A caller that waits for the restyle,
        // or a change after all the content is in, counts as someone waiting.
        bool loading = r->document->readiness == DomReadiness_Loading || r->holding;
        Walk w{
            .presentation = r,
            .all = all,
            .fresh = job ? r->newLeft : UINT32_MAX,
            .taken = 0,
            .deadline = deadline,
            .parallel = !job || !loading,
            .rootFont = r->rootFont,
            .body = body,
        };
        visitElement(w, root, nullptr, ParentChange_None, false, false, true);
        r->rootFont = w.rootFont;
        if (r->countersStale)
            updateCounters(r);
        if (reach == RestyleReach_Job)
            r->newLeft = w.fresh;
        r->newTaken += w.taken;
    }
    dropPrefetch(r);
    r->invalidations.clear();
    r->invalidationIndex.clear();
    r->document->pending = 0;
    for (uint32_t i = 0; i < r->threads.count; ++i) {
        const Array<CssStructureRead>& structure = r->threads.data[i].scratch.structure;
        for (uint32_t k = 0; k < structure.count; ++k) {
            uint32_t at = cssPresentationNodeEntry(r, structure.data[k].node);
            if (at != kHashMissing)
                r->entries.data[at].structure |= structure.data[k].flags;
        }
        r->readsViewport = r->readsViewport || r->threads.data[i].scratch.readsViewport;
        r->accrued.elementsStyledCached += r->threads.data[i].scratch.matchedHits;
        r->threads.data[i].scratch.matchedHits = 0;
        cssCascadeScratchForget(&r->threads.data[i].scratch);
    }
    // New content that was held back marked the elements that it waits under, and the next update visits them.
    r->holding = root != nullptr && root->pending != 0;
    if (r->holding)
        r->changed = true;
    r->layoutStale = true;
    ++r->version;
    return true;
}

// Restyles ahead of the next job, and updates the restyle rate if the restyle took in enough new elements to measure
// it.
void restyleAhead(PrCssPresentation* r, PrLayoutVec extent) {
    uint32_t taken = r->newTaken;
    double started = prTimeMonotonicMilliseconds();
    restyle(r, extent, RestyleReach_Job);
    double time = prTimeMonotonicMilliseconds() - started;
    taken = r->newTaken - taken;
    if (taken < kMinJobElements || time <= 0)
        return;
    double rate = double(taken) / time;
    r->restyleRate = r->restyleRate > 0 ? (r->restyleRate + rate) / 2 : rate;
}

// Returns whether a user's wheel anywhere over the document's viewport waits for the document. It does if the window,
// the document, its root or its body has a wheel listener that is not passive. A wheel event reaches those targets
// wherever the wheel is.
PrLayoutWheelEvents viewportWheelEvents(const PrCssPresentation* r) {
    PrDomDocument* document = r->document;
    const PrDomElement* root = domDocumentElement(document);
    const PrDomElement* body = root != nullptr ? domDocumentBody(document) : nullptr;
    bool waits = (document->global != nullptr && domHasBlockingWheelListener(document->global)) ||
        domHasBlockingWheelListener(document) || (root != nullptr && domHasBlockingWheelListener(root)) ||
        (body != nullptr && domHasBlockingWheelListener(body));
    return waits ? PrLayoutWheelEvents_Blocking : PrLayoutWheelEvents_Passive;
}

// Brings the tree up to date with the changes that the restyles queued since it was last brought up to date, on the
// thread that holds the tree. Erases the boxes of the nodes that no longer generate one, makes the boxes of the nodes
// that now do, sets their records and text, puts the children's boxes under their parents' boxes, and sets the root and
// what the canvas takes. The tree's target pixel ratio is the media environment's resolution, which font metrics,
// border widths and painting snap to.
void applyBoxes(PrCssPresentation* r) {
    prLayoutTreeSetTargetPixelRatio(r->tree, cssMediaEnvironmentPixelRatio(r->document->media));
    prLayoutTreeSetViewportWheelEvents(r->tree, viewportWheelEvents(r));
    PrDomElement* root = domDocumentElement(r->document);
    PrDomElement* body = root != nullptr && domIsHtml(root, PrDomTag_Html) ? domDocumentBody(r->document) : nullptr;
    // The canvas takes the styles of the root and the body, so it is projected again if either box is made or restyled,
    // or if either element has a different box.
    bool canvas = false;
    // An entry's state says what to apply, so a node queued twice, or listed after it left, changes nothing more.
    for (uint32_t i = 0; i < r->boxQueue.count; ++i) {
        uint32_t at = cssPresentationNodeEntry(r, r->boxQueue.data[i]);
        if (at == kHashMissing || r->entries.data[at].layoutNode == kPrLayoutNodeNone)
            continue;
        if (r->entries.data[at].boxed) {
            if ((r->entries.data[at].ops & CssPresentationBoxOp_Remake) != 0) {
                eraseBox(r, at);
                r->entries.data[at].boxed = true;
            }
            continue;
        }
        // The boxes of the children of an element that changed to display contents outlive its box. They become
        // children of its parent's box, where reconciling the parent places them.
        PrLayoutNode box = r->entries.data[at].layoutNode;
        while (r->entries.data[at].contents && prLayoutNodeFirstChild(r->tree, box) != kPrLayoutNodeNone)
            prLayoutTreeRemove(r->tree, prLayoutNodeFirstChild(r->tree, box));
        eraseBox(r, at);
    }
    for (uint32_t i = 0; i < r->boxQueue.count; ++i) {
        uint32_t at = cssPresentationNodeEntry(r, r->boxQueue.data[i]);
        if (at == kHashMissing || !r->entries.data[at].boxed)
            continue;
        CssPresentationEntry& entry = r->entries.data[at];
        PrDomNode* node = const_cast<PrDomNode*>(entry.node);
        bool shared = node == static_cast<PrDomNode*>(root) || node == static_cast<PrDomNode*>(body);
        bool restyled = entry.layoutNode == kPrLayoutNodeNone || (entry.ops & CssPresentationBoxOp_Style) != 0;
        canvas = canvas || (shared && restyled);
        if (entry.layoutNode == kPrLayoutNodeNone) {
            PrLayoutNode box = kPrLayoutNodeNone;
            if (isPseudo(node)) {
                box = prLayoutNodeCreateElement(r->tree, kPrLayoutNodeNone, projectEntry(r, entry));
                prLayoutNodeSetRendered(r->tree, box, PrLayoutRenderedFlag_Skipped);
            } else if (domIsText(node)) {
                box = prLayoutNodeCreateText(r->tree, kPrLayoutNodeNone,
                    prStringView(&static_cast<PrDomCharacterData*>(node)->data));
            } else if (isReplaced(static_cast<PrDomElement*>(node))) {
                box = prLayoutNodeCreateReplaced(r->tree, kPrLayoutNodeNone, projectEntry(r, entry));
                applyContent(r, static_cast<PrDomElement*>(node), box);
            } else if (isLineBreak(node)) {
                box = prLayoutNodeCreateLineBreak(r->tree, kPrLayoutNodeNone, projectEntry(r, entry));
            } else if (isSemiReplaced(node)) {
                box = prLayoutNodeCreateSemiReplaced(r->tree, kPrLayoutNodeNone, projectEntry(r, entry));
            } else {
                box = prLayoutNodeCreateElement(r->tree, kPrLayoutNodeNone, projectEntry(r, entry));
                prLayoutNodeSetRendered(r->tree, box, renderedFlagsOf(static_cast<PrDomElement*>(node)));
            }
            present(r, box, node);
            r->entries.data[at].layoutNode = box;
            ++r->accrued.boxesCreated;
            // The content of the svg element that draws a foreignObject draws the root that its box is.
            const PrDomElement* made = domElementFromNode(node);
            if (made != nullptr && isForeignRoot(made)) {
                ++r->foreignBoxes;
                svgNote(r, made, CssPresentationSvgNoteKind_Box);
            }
            if (isPseudo(node))
                applyGenerated(r, r->entries.data[at]);
            continue;
        }
        if ((entry.ops & CssPresentationBoxOp_Style) != 0)
            prLayoutNodeSetStyle(r->tree, entry.layoutNode, projectEntry(r, entry));
        if ((entry.ops & CssPresentationBoxOp_Text) != 0)
            prLayoutNodeSetText(r->tree, entry.layoutNode, prStringView(&static_cast<PrDomCharacterData*>(node)->data));
        if ((entry.ops & CssPresentationBoxOp_Content) != 0 && isPseudo(node))
            applyGenerated(r, entry);
        else if ((entry.ops & CssPresentationBoxOp_Content) != 0)
            applyContent(r, static_cast<PrDomElement*>(node), entry.layoutNode);
    }
    for (uint32_t i = 0; i < r->boxQueue.count; ++i) {
        uint32_t at = cssPresentationNodeEntry(r, r->boxQueue.data[i]);
        if (at == kHashMissing)
            continue;
        CssPresentationEntry& entry = r->entries.data[at];
        bool children = (entry.ops & CssPresentationBoxOp_Children) != 0;
        if (children && entry.layoutNode != kPrLayoutNodeNone && isPseudo(entry.node))
            placeMarkerOf(r, entry);
        else if ((entry.ops & CssPresentationBoxOp_Children) != 0 && entry.layoutNode != kPrLayoutNodeNone)
            reconcile(r, static_cast<PrDomElement*>(const_cast<PrDomNode*>(entry.node)), entry.layoutNode);
        r->entries.data[at].ops = 0;
    }
    r->boxQueue.clear();
    releaseDroppedQueue(r);
    applyFirsts(r);
    // No queue or box refers to the pseudo-elements dropped before now.
    for (uint32_t i = 0; i < r->retired.count; ++i)
        freePseudo(r->retired.data[i]);
    r->retired.clear();
    uint32_t at = root != nullptr ? cssPresentationNodeEntry(r, root) : kHashMissing;
    PrLayoutNode node = at != kHashMissing ? r->entries.data[at].layoutNode : PrLayoutNode(kPrLayoutNodeNone);
    r->root = node;
    uint32_t bodyAt = body != nullptr ? cssPresentationNodeEntry(r, body) : kHashMissing;
    PrLayoutNode bodyNode;
    if (bodyAt != kHashMissing)
        bodyNode = r->entries.data[bodyAt].layoutNode;
    else
        bodyNode = PrLayoutNode(kPrLayoutNodeNone);
    if (root != nullptr && (canvas || node != r->canvasRoot || bodyNode != r->canvasBody))
        projectCanvas(r, root, body);
    r->canvasRoot = node;
    r->canvasBody = bodyNode;
    eraseTaken(r);
    // No box paints the drawings retired before now.
    for (uint32_t i = 0; i < r->retiredSvgs.count; ++i)
        freeSvg(r->retiredSvgs.data[i]);
    r->retiredSvgs.clear();
}

// Appends to `out` the roots that the presentation lays out: the box of the document's root in the viewport of
// `extent`, then the content of each foreignObject that the content of an svg element draws, at the size of its
// positioning rectangle. A foreignObject comes after those around it, whose drawings draw it (prLayoutNodeDraw).
void layoutRootsOf(const PrCssPresentation* r, PrLayoutVec extent, Array<PrLayoutRoot>* out) {
    out->push(PrLayoutRoot{r->root, extent});
    if (r->foreignBoxes == 0 || r->svgs.count == 0)
        return;
    struct Ordered {
        uint32_t depth;
        PrLayoutRoot root;
    };
    Array<Ordered> roots;
    for (uint32_t i = 0; i < r->svgs.count; ++i) {
        uint32_t at = cssPresentationNodeEntry(r, r->svgs.data[i]->element);
        if (at == kHashMissing || !r->entries.data[at].boxed)
            continue;
        const Array<CssPresentationSvgForeignObject>& found = r->svgs.data[i]->foreignObjects;
        for (uint32_t k = 0; k < found.count; ++k) {
            uint32_t depth = 0;
            for (const PrDomNode* up = found.data[k].element; up != nullptr; up = up->parent)
                ++depth;
            roots.push(Ordered{depth, PrLayoutRoot{found.data[k].root, prLayoutVecFromPx(found.data[k].extent)}});
        }
    }
    sortItems(roots.data, roots.count, [](const Ordered& a, const Ordered& b) { return a.depth < b.depth; });
    for (uint32_t i = 0; i < roots.count; ++i)
        out->push(roots.data[i].root);
    roots.reset();
}

// Records the work done since the last update as the stats of this update.
void closeUpdate(PrCssPresentation* r) {
    r->stats = r->accrued;
    r->accrued = PrCssPresentationStats{};
}

// Returns the used appearance of the elements that a sheet for `appearance` styles, or None for every element.
CssAppearance appearanceOf(PrCssSheetAppearance appearance) {
    switch (appearance) {
        case PrCssSheetAppearance_Native: return CssAppearance_Native;
        case PrCssSheetAppearance_Base: return CssAppearance_Base;
        default: return CssAppearance_None;
    }
}

} // namespace

void cssPresentationRuleChanged(PrCssPresentation* presentation, const CssRuleData* rule) {
    if (presentation->restyleAll)
        return;
    Array<uint32_t> features;
    PrDomElement* root = domDocumentElement(presentation->document);
    if (!cssRuleSubjectFeatures(rule, &features) || presentation->rules.reads.global) {
        features.reset();
        cssPresentationChanged(presentation);
        return;
    }
    presentation->changed = true;
    presentation->rulesStale = true;
    if (root != nullptr && features.count != 0) {
        // The elements that carry a feature are restyled, in the same way as the descendant invalidations of a change
        // at the root.
        if (carries(root, features.data, features.count))
            mark(root, DomStyleFlag_Self);
        addFeatures(presentation, root, features);
    }
    features.reset();
}

void cssPresentationResourceLoaded(PrCssPresentation* presentation) {
    presentation->changed = true;
}

void cssPresentationInserted(PrCssPresentation* presentation, PrDomNode* node) {
    mark(node, DomStyleFlag_Self | DomStyleFlag_Subtree);
    markChildren(presentation, node);
    if (presentation->rules.reads.direction && invalidates(presentation))
        invalidateDirection(presentation, node);
    // The contents of the new subtree can make the :has() of an ancestor or an earlier sibling match.
    if (!presentation->restyleAll && presentation->rules.anchors.anchors.count != 0)
        invalidateAnchors(presentation, node, presentation->rules.anchors);
}

void cssPresentationRemoving(PrCssPresentation* presentation, PrDomNode* node) {
    // The ancestors of a focused, hovered or active element lose :focus-within, :hover or :active when it is removed.
    const PrDomDocument* document = node->document;
    const PrDomElement* pointed[] = {document->focused, document->hovered, document->active};
    const CssPseudoClass focus[] = {CssPseudoClass_Focus, CssPseudoClass_FocusVisible, CssPseudoClass_FocusWithin};
    const CssPseudoClass hover = CssPseudoClass_Hover;
    const CssPseudoClass active = CssPseudoClass_Active;
    const CssPseudoClass* states[] = {focus, &hover, &active};
    const uint32_t counts[] = {3, 1, 1};
    for (uint32_t p = 0; p < 3; ++p) {
        bool inside = false;
        for (const PrDomNode* at = pointed[p]; at != nullptr && !inside; at = at->parent)
            inside = at == node;
        for (PrDomNode* at = inside ? node->parent : nullptr; at != nullptr; at = at->parent) {
            if (PrDomElement* ancestor = domElementFromNode(at))
                cssPresentationStateChanged(presentation, ancestor, states[p], counts[p]);
        }
    }
    if (!presentation->restyleAll && presentation->rules.anchors.anchors.count != 0)
        invalidateAnchors(presentation, node, presentation->rules.anchors);
    if (presentation->rules.reads.direction && invalidates(presentation))
        invalidateDirection(presentation, node);
    forget(presentation, node);
    markChildren(presentation, node);
}

void cssPresentationAttributeChanged(PrCssPresentation* presentation, PrDomElement* element, DomNamespaceId space,
        PrStringView name, PrStringView oldValue, PrStringView value) {
    // HTML's attributes are in no namespace. An attribute in another namespace, such as xml:lang, reaches what reads
    // its local name.
    bool html = space == PrDomNamespace_None;
    // CSS hears the change first, and has dropped the hints that the attribute builds (css_hints.h).
    CssHintReach hinted = html ? cssHintsMapped(element, name) : CssHintReach_None;
    if (!invalidates(presentation))
        return;
    switch (hinted) {
        case CssHintReach_None: break;
        case CssHintReach_Self:
            presentation->changed = true;
            mark(element, DomStyleFlag_Self);
            break;
        case CssHintReach_Subtree:
            presentation->changed = true;
            mark(element, DomStyleFlag_Self | DomStyleFlag_Subtree);
            break;
        case CssHintReach_Document:
            presentation->changed = true;
            presentation->restyleAll = true;
            break;
    }
    invalidate(presentation, element, cssFeatureHash(CssFeature_Attribute, name));
    // The element's language is part of its style, and a change of locale restyles its descendants (changeOf).
    bool language = name == "lang" && (space == PrDomNamespace_Xml ||
        (html && (element->space == PrDomNamespace_Html || element->space == PrDomNamespace_Svg)));
    if (language) {
        presentation->changed = true;
        mark(element, DomStyleFlag_Self);
    }
    if (!html)
        return;
    // A style that reads attributes through attr() restyles its element: the element's inline style, or a rule whose
    // attr() takes its name from another function.
    const CssElementStyle* style = element->style;
    if (presentation->rules.reads.attributes ||
            (style != nullptr && style->inlineStyle != nullptr && cssBlockReadsAttributes(&style->inlineStyle->block))) {
        presentation->changed = true;
        mark(element, DomStyleFlag_Self);
    }
    // The style attribute styles only the element.
    if (name == "style") {
        presentation->changed = true;
        mark(element, DomStyleFlag_Self);
    }
    // What the element's markup gives its box, such as the spans of a table cell, comes from its attributes, not from
    // its style (CssBoxTraits).
    if (html && cssBoxReads(element, name)) {
        uint32_t at = cssPresentationNodeEntry(presentation, element);
        if (at != kHashMissing && presentation->entries.data[at].node != nullptr) {
            presentation->entries.data[at].reprojectPending = true;
            presentation->changed = true;
            // The traits may decide which children generate boxes.
            mark(element, DomStyleFlag_Self | DomStyleFlag_Subtree);
        }
    }
    if (name == "class") {
        // Invalidate the classes that are in one value and not the other.
        eachClass(oldValue, [&](PrStringView token) {
            if (!hasClass(value, token))
                invalidate(presentation, element, cssFeatureHash(CssFeature_Class, token));
        });
        eachClass(value, [&](PrStringView token) {
            if (!hasClass(oldValue, token))
                invalidate(presentation, element, cssFeatureHash(CssFeature_Class, token));
        });
    } else if (name == "id" && oldValue != value) {
        if (oldValue.size != 0)
            invalidate(presentation, element, cssFeatureHash(CssFeature_Id, oldValue));
        if (value.size != 0)
            invalidate(presentation, element, cssFeatureHash(CssFeature_Id, value));
    }
    // An attribute can decide a state of the element or of its descendants.
    if (presentation->rules.reads.states && decidesStates(name))
        invalidate(presentation, element, cssFeatureHash(CssFeature_State, ""));
}

void cssPresentationDataChanged(PrCssPresentation* presentation, PrDomNode* node) {
    presentation->changed = true;
    mark(node, DomStyleFlag_Self);
    if (presentation->rules.reads.direction && invalidates(presentation))
        invalidateDirection(presentation, node);
    // :empty in a :has() argument reads whether an element has text.
    if (!presentation->restyleAll && presentation->rules.reads.structure && presentation->rules.anchors.anchors.count != 0)
        invalidateAnchors(presentation, node, presentation->rules.anchors);
    // :empty reads whether an element has text.
    if (presentation->rules.reads.structure && node->parent != nullptr && readsEmpty(presentation, node->parent))
        mark(node->parent, DomStyleFlag_Self);
}

void cssPresentationContentChanged(PrCssPresentation* presentation, PrDomElement* element) {
    presentation->changed = true;
    mark(element, DomStyleFlag_Content);
}

void cssPresentationStateChanged(PrCssPresentation* presentation, PrDomElement* element, const CssPseudoClass* states,
        uint32_t count) {
    if (!invalidates(presentation))
        return;
    for (uint32_t i = 0; i < count; ++i)
        invalidate(presentation, element, cssFeatureHash(CssFeature_PseudoClass, kCssPseudoClassName[states[i]]));
}

void cssPresentationSlotChanged(PrCssPresentation* presentation, PrDomElement* slot,
        PrDomNode* const PR_NONNULL* PR_NULLABLE before, uint32_t beforeCount) {
    presentation->changed = true;
    uint32_t count = domSlotAssignedCount(slot);
    PrDomNode* const* now = domSlotAssigned(slot);
    auto holds = [](PrDomNode* const* PR_NULLABLE list, uint32_t size, const PrDomNode* node) {
        for (uint32_t i = 0; i < size; ++i) {
            if (list[i] == node)
                return true;
        }
        return false;
    };
    for (uint32_t i = 0; i < beforeCount; ++i) {
        PrDomNode* node = before[i];
        if (holds(now, count, node))
            continue;
        // A node that another slot took inherits from that slot now.
        if ((node->flags & DomNodeFlag_Slotted) != 0)
            mark(node, DomStyleFlag_Self | DomStyleFlag_Subtree);
        else
            forget(presentation, node);
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!holds(before, beforeCount, now[i]))
            mark(now[i], DomStyleFlag_Self | DomStyleFlag_Subtree);
    }
    // The slot's children are in the flat tree while it has nothing assigned.
    if ((beforeCount == 0) != (count == 0)) {
        for (PrDomNode* child = slot->first; child != nullptr; child = child->next) {
            if (count != 0)
                forget(presentation, child);
            else
                mark(child, DomStyleFlag_Self | DomStyleFlag_Subtree);
        }
    }
    mark(slot, DomStyleFlag_Children);
    // Whether a slot has slotted nodes, which :has-slotted reads, changes for the slot and for each slot that it is
    // assigned to in turn, whose flattened slottables include its own (css-shadow-1/#selectordef-has-slotted).
    for (PrDomNode* at = slot; at != nullptr && domIsSlot(at); at = domAssignedSlot(at))
        mark(at, DomStyleFlag_Self);
}

void cssPresentationShadowAttached(PrCssPresentation* presentation, PrDomElement* host) {
    presentation->changed = true;
    // The rules match by scope once the document has a shadow tree whose sheets style it alone, which a use element's
    // does not (dom_use.h). Filing them again changes no light element's matches.
    if (!presentation->rules.scoped && host->document->slots->scopes)
        presentation->rulesStale = true;
    for (PrDomNode* child = host->first; child != nullptr; child = child->next)
        forget(presentation, child);
    mark(host, DomStyleFlag_Children);
}

void cssPresentationWheelListenersChanged(PrCssPresentation* presentation, PrDomElement* element) {
    uint32_t at = cssPresentationNodeEntry(presentation, element);
    if (at == kHashMissing || presentation->entries.data[at].node == nullptr)
        return;
    presentation->entries.data[at].reprojectPending = true;
    presentation->changed = true;
    mark(element, DomStyleFlag_Self);
}

void cssPresentationStatesChanged(PrCssPresentation* presentation, PrDomElement* element) {
    if (invalidates(presentation) && presentation->rules.reads.states)
        invalidate(presentation, element, cssFeatureHash(CssFeature_State, ""));
}

void cssPresentationFieldChanged(PrCssPresentation* presentation, PrDomElement* element) {
    if (!isTextField(element))
        return;
    uint32_t at = cssPresentationNodeEntry(presentation, element);
    if (at == kHashMissing)
        return;
    CssPresentationPseudo* pseudo = presentation->entries.data[at].pseudos[kFieldContentPseudo];
    uint32_t own = pseudo != nullptr ? cssPresentationNodeEntry(presentation, pseudo) : kHashMissing;
    if (own != kHashMissing && presentation->entries.data[own].boxed)
        queueBox(presentation, own, CssPresentationBoxOp_Content);
    // The value of a control decides its automatic direction.
    if (automaticDirection(element)) {
        if (presentation->entries.data[at].direction == CssPresentationDirection_Unknown) {
            invalidate(presentation, element, cssFeatureHash(CssFeature_Attribute, "dir"));
        } else if (!presentation->entries.data[at].directionQueued) {
            presentation->entries.data[at].directionQueued = true;
            presentation->directionQueue.push(at);
            presentation->changed = true;
        }
    }
}

void cssPresentationHostStatesChanged(PrCssPresentation* presentation, PrDomHostState states) {
    // An element that the presentation has not styled takes the host's answers when it is first styled.
    for (uint32_t i = 0; i < presentation->entries.count; ++i) {
        PrDomElement* element = domElementFromNode(const_cast<PrDomNode*>(presentation->entries.data[i].node));
        if (element == nullptr)
            continue;
        if ((states & PrDomHostState_Visited) != 0 && domElementLink(element)) {
            PrStringView href = domElementLinkHref(element);
            cssPresentationAttributeChanged(presentation, element, PrDomNamespace_None, "href", href, href);
        }
        if ((states & PrDomHostState_Autofill) != 0 && domIsHtml(element, PrDomTag_Input)) {
            CssPseudoClass autofill = CssPseudoClass_Autofill;
            cssPresentationStateChanged(presentation, element, &autofill, 1);
        }
    }
}

void cssPresentationFlush(PrCssPresentation* presentation) {
    const DomDocumentHost* host = presentation->document->host;
    if (host != nullptr && host->updatePresentation != nullptr) {
        host->updatePresentation(host->context);
        return;
    }
    PrVec2 viewport{};
    PrLayoutVec extent = presentation->extent;
    if (host != nullptr && host->viewport != nullptr && host->viewport(host->context, &viewport))
        extent = prLayoutVecFromPx(viewport);
    prCssPresentationUpdate(presentation, extent);
}

void cssPresentationUpdateBoxes(PrCssPresentation* presentation, PrLayoutVec extent) {
    own(presentation);
    restyle(presentation, extent, RestyleReach_All);
    if (presentation->layoutStale)
        applyBoxes(presentation);
}

bool cssPresentationUpdateLayout(PrCssPresentation* presentation, PrLayoutVec extent) {
    closeUpdate(presentation);
    if (!presentation->layoutStale) {
        // A face that left with its sheet may have been the last face that the set waited for.
        cssFontsReadinessChanged(presentation->document);
        return false;
    }
    presentation->layoutStale = false;
    Array<PrLayoutRoot> roots;
    layoutRootsOf(presentation, extent, &roots);
    prLayoutTreeLayout(presentation->tree, roots.data, roots.count);
    roots.reset();
    svgsResized(presentation);
    anchorsMoved(presentation);
    presentation->sceneStale = presentation->sceneStale || !prLayoutTreeSceneIsCurrent(presentation->tree);
    // The faces that the layout asked for start loading. A set that waited on this layout and loads no face is loaded.
    syncFonts(presentation);
    cssFontsReadinessChanged(presentation->document);
    // What this update laid out belongs to no job.
    presentation->newTaken = 0;
    return true;
}

bool cssPresentationSvgsResized(PrCssPresentation* presentation) {
    return presentation->svgs.count != 0 && own(presentation);
}

PrLayoutNode cssPresentationBox(const PrCssPresentation* presentation, const PrDomNode* node) {
    return layoutNodeOf(presentation, node);
}

PrDomNode* PR_NULLABLE cssPresentationNodeFromLayout(const PrCssPresentation* presentation, PrLayoutNode node) {
    return seen(presented(presentation, node));
}

void cssPresentationHits(PrCssPresentation* presentation, PrLayoutVec point, uint32_t limit, Array<PrDomNode*>* out) {
    own(presentation);
    PrLayoutTree* tree = presentation->tree;
    // Most points hit few boxes, which one walk finds. A walk that finds more than the first guess walks again.
    Array<PrLayoutNode> boxes;
    boxes.resize(limit < 16 ? limit : 16);
    uint32_t total = prLayoutTreeHitTest(tree, point, boxes.data, boxes.count);
    if (total > boxes.count && limit > boxes.count) {
        boxes.resize(total < limit ? total : limit);
        prLayoutTreeHitTest(tree, point, boxes.data, boxes.count);
    }
    boxes.count = total < boxes.count ? total : boxes.count;
    auto add = [&](PrDomNode* PR_NULLABLE node) {
        if (node != nullptr && out->find(node) == kArrayMissing)
            out->push(node);
    };
    for (uint32_t i = 0; i < boxes.count; ++i) {
        PrDomNode* node = seen(presenter(presentation, boxes.data[i]));
        const PrDomElement* element = node != nullptr ? domElementFromNode(node) : nullptr;
        const CssPresentationSvg* svg = element != nullptr ? svgFind(presentation, element) : nullptr;
        PrLayoutVec local{};
        if (svg != nullptr && svg->functions != nullptr && prLayoutNodeLocalPoint(tree, boxes.data[i], point, &local)) {
            // The content of an outermost svg element is hit in its content box, which is its viewport.
            PrLayoutRect box = prLayoutNodeReplacedContent(tree, boxes.data[i]);
            PrVec2 at = prLayoutVecToPx(PrLayoutVec{local.x - box.left, local.y - box.top});
            Array<CssPresentationSvgHit> hits;
            hits.resize(svg->functions->hitTest(svg->content, at, nullptr, 0));
            svg->functions->hitTest(svg->content, at, hits.data, hits.count);
            for (uint32_t k = 0; k < hits.count; ++k) {
                const CssPresentationSvgHit& hit = hits.data[k];
                // A foreignObject's content is a layout root of its own, which is hit before the foreignObject.
                if (hit.root != kPrLayoutNodeNone) {
                    Array<PrLayoutNode> inner;
                    PrLayoutVec within = prLayoutVecFromPx(hit.rootPoint);
                    inner.resize(prLayoutNodeHitTest(tree, hit.root, within, nullptr, 0));
                    prLayoutNodeHitTest(tree, hit.root, within, inner.data, inner.count);
                    for (uint32_t n = 0; n < inner.count; ++n)
                        add(seen(presenter(presentation, inner.data[n])));
                    inner.reset();
                }
                add(const_cast<PrDomElement*>(hit.element));
            }
            hits.reset();
        }
        // A replaced element that takes no pointer is found only for its content.
        const CssComputedStyle* style = element != nullptr ? cssPresentationStyle(presentation, element) : nullptr;
        if (style == nullptr || cssComputedFirstKeyword(style, PrCssProperty_PointerEvents) != PrCssKeyword_None)
            add(node);
    }
    boxes.reset();
}

const CssComputedStyle* PR_NULLABLE cssPresentationStyle(const PrCssPresentation* presentation, const PrDomElement* element) {
    uint32_t at = cssPresentationNodeEntry(presentation, element);
    return at != kHashMissing ? presentation->entries.data[at].style : nullptr;
}

bool cssPresentationIsInteractive(PrCssPresentation* presentation, const PrDomElement* element) {
    restyle(presentation, presentation->extent, RestyleReach_All);
    return interactive(presentation, element);
}

const PrDomNode* PR_NULLABLE cssPresentationBoxNode(const PrCssPresentation* presentation, PrLayoutNode box) {
    const PrDomNode* node = presented(presentation, box);
    return node != nullptr && !isPseudo(node) ? node : nullptr;
}

const CssComputedStyle* cssPresentationStyleUnder(PrCssPresentation* presentation, const PrDomElement* element,
        const CssComputedStyle* PR_NULLABLE parent) {
    startThreads(presentation);
    cssStylePrepare(const_cast<PrDomElement*>(element));
    // The element is a copy, made again by each build of its tree, which may take the place of a copy that the matchers
    // learned of.
    cssCascadeScratchForget(&presentation->threads.data[0].scratch);
    CssCascadeInput input{
        .element = element,
        .parent = parent,
        .rootFont = presentation->rootFont,
        .viewport = viewportOf(presentation),
        .fresh = false,
        .matched = nullptr,
        .pseudo = CssPseudo_None,
    };
    CssPresentationThread& thread = presentation->threads.data[0];
    return cssCascadeStyle(&presentation->rules, &thread.builder, &thread.scratch, input);
}

const CssComputedStyle* PR_NULLABLE cssPresentationPseudoStyleUnder(PrCssPresentation* presentation,
        const PrDomElement* element, CssPseudo pseudo, const CssComputedStyle* PR_NULLABLE parent) {
    if (!stylesFirsts(presentation))
        return nullptr;
    startThreads(presentation);
    cssStylePrepare(const_cast<PrDomElement*>(element));
    cssCascadeScratchForget(&presentation->threads.data[0].scratch);
    uint32_t originating = cssCascadeOriginating(&presentation->rules, &presentation->threads.data[0].scratch, element);
    if ((originating & (1u << pseudo)) == 0)
        return nullptr;
    return cascadeFirst(presentation, element, pseudo, parent);
}

void cssPresentationReprojectReferencing(PrCssPresentation* presentation) {
    reprojectReferencing(presentation);
}

CssColorEnvironment cssPresentationColorEnvironment(const PrCssPresentation* presentation) {
    return colorEnvironmentOf(presentation, presentation->headroom, nullptr);
}

CssColorContext cssPresentationColorContext(PrCssPresentation* presentation, const PrDomElement* element) {
    restyle(presentation, presentation->extent, RestyleReach_All);
    CssColorEnvironment environment = cssPresentationColorEnvironment(presentation);
    const CssComputedStyle* style = cssPresentationStyle(presentation, element);
    if (style == nullptr)
        return CssColorContext{.current = PrColor{0, 0, 0, 1}, .environment = environment};
    return cssStyleColorContext(style, environment);
}

const CssComputedStyle* PR_NULLABLE cssPresentationStyleCurrent(PrCssPresentation* presentation, const PrDomElement* element) {
    restyle(presentation, presentation->extent, RestyleReach_All);
    return cssPresentationStyle(presentation, element);
}

PrLayoutTree* cssPresentationOwnTree(PrCssPresentation* presentation) {
    own(presentation);
    return presentation->tree;
}

bool cssPresentationMapFromContent(PrCssPresentation* presentation, const PrDomElement* element, PrDVec2* points,
        uint32_t count) {
    PrLayoutTree* tree = cssPresentationOwnTree(presentation);
    PrLayoutNode box = layoutNodeOf(presentation, element);
    if (box == kPrLayoutNodeNone)
        return false;
    PrLayoutRect content = prLayoutNodeReplacedContent(tree, box);
    for (uint32_t i = 0; i < count; ++i) {
        points[i].x += prLayoutUnitToPxDouble(content.left);
        points[i].y += prLayoutUnitToPxDouble(content.top);
    }
    return prLayoutNodeMapFromBox(tree, box, points, count);
}

void cssPresentationScrollTo(PrCssPresentation* presentation, PrLayoutNode node, PrLayoutVec offset) {
    own(presentation);
    scrollTree(presentation, node, offset, CssPresentationScrollBy_Document);
}

void cssPresentationRunScrollSteps(PrCssPresentation* presentation) {
    for (uint32_t i = 0; i < presentation->anchorMoves.count; ++i) {
        if (presentation->anchorMoves.data[i].delta != PrLayoutVec{})
            queueScrolled(presentation, presentation->anchorMoves.data[i].node);
    }
    presentation->anchorMoves.clear();
    if (presentation->scrolled.count == 0)
        return;
    // Event listeners can scroll again. Those scrolls wait for the next run.
    Array<PrDomNode*> targets = presentation->scrolled;
    presentation->scrolled = {};
    const PrStringView names[] = {"scroll", "scrollend"};
    for (PrStringView name : names) {
        for (uint32_t i = 0; i < targets.count; ++i) {
            PrDomNode* target = targets.data[i];
            PrDomEventInit init = prDomEventInitDefault();
            init.bubbles = target == target->document;
            domEventFire(target, name, init);
        }
    }
    for (uint32_t i = 0; i < targets.count; ++i)
        domNodeUnhold(targets.data[i]);
    targets.reset();
}

void cssPresentationSendAnimationEvents(PrCssPresentation* presentation) {
    CssAnimateState& a = presentation->animations;
    cssAnimateFindEvents(presentation);
    if (a.events.count == 0)
        return;
    // Listeners can start and cancel animations. The events of those animations wait for the next update.
    Array<CssAnimateEvent> events = a.events;
    a.events = {};
    sortItems(events.data, events.count, [](const CssAnimateEvent& x, const CssAnimateEvent& y) {
        return x.scheduled < y.scheduled;
    });
    PrDomEventInit init = prDomEventInitDefault();
    init.bubbles = true;
    for (uint32_t i = 0; i < events.count; ++i) {
        CssAnimateEvent& e = events.data[i];
        PrDomEvent* event;
        if (e.transition)
            event = domEventCreate<PrCssTransitionEvent>(e.type, init, PrDomEventKind_Transition);
        else
            event = domEventCreate<PrCssAnimationEvent>(e.type, init, PrDomEventKind_Animation);
        domEventSetAnimation(event, prStringView(&e.name), e.elapsedTime, cssPseudoName(e.pseudo));
        event->trusted = true;
        PrDomElement* target = const_cast<PrDomElement*>(e.element);
        domEventDispatch(target, event);
        domEventRelease(event);
        domNodeUnhold(target);
        prStringReset(&e.name);
    }
    events.reset();
}

bool cssPresentationHasWork(const PrCssPresentation* presentation) {
    return presentation->scrolled.count != 0 || presentation->anchorMoves.count != 0 ||
        presentation->animations.events.count != 0 || presentation->changed ||
        cssDocumentRoot(presentation->document)->fontsVersion != presentation->fontsVersion ||
        cssPresentationAwaitsFontRequests(presentation);
}

bool cssPresentationAwaitsFontRequests(const PrCssPresentation* presentation) {
    const CssRoot* style = cssDocumentRoot(presentation->document);
    bool stale =
        presentation->changed || presentation->layoutStale || style->fontsVersion != presentation->fontsVersion;
    if (!stale)
        return false;
    // The count describes the faces as the last sync listed them. A face added or deleted, or a sheet or rule changed
    // since then, leaves the count stale, and a stale count may hide a face that layout will request. A face's status
    // leaves Unloaded and never returns to it, so a status change leaves the count no lower than the true value.
    bool counted = style->fontSetVersion == presentation->unloadedFacesVersion && !presentation->rulesStale &&
        !presentation->restyleAll;
    return !counted || presentation->unloadedFaces != 0;
}

void cssPresentationUpdateStyle(PrCssPresentation* presentation) {
    restyle(presentation, presentation->extent, RestyleReach_Job);
    // The sync may have settled what the set waited for, and no layout follows to report it.
    cssFontsReadinessChanged(presentation->document);
}

void cssPresentationComputedBlock(PrCssPresentation* presentation, const PrDomElement* element, CssPseudo pseudo,
        CssDeclarationBlock* out, const void* PR_NULLABLE* PR_NONNULL filler, uint64_t* version) {
    cssPresentationFlush(presentation);
    if (*filler == presentation && *version == presentation->version)
        return;
    *filler = presentation;
    *version = presentation->version;
    uint32_t at = cssPresentationNodeEntry(presentation, element);
    PrTextBuffer text{};
    if (at != kHashMissing && presentation->entries.data[at].style != nullptr) {
        const CssComputedStyle* style = targetStyle(presentation, at, pseudo);
        PrLayoutNode node = pseudo == CssPseudo_None ? presentation->entries.data[at].layoutNode : kPrLayoutNodeNone;
        const ComputedOrder& order = computedOrder();
        for (uint32_t i = 0; i < order.count; ++i) {
            prTextBufferAppend(&text, cssPropertyName(order.properties[i]));
            prTextBufferAppend(&text, ": ");
            serializeResolved(&text, presentation, style, order.properties[i], node);
            prTextBufferAppend(&text, "; ");
        }
        cssCustomEach(style->custom, [&](const CssCustomRow& row) {
            prTextBufferAppend(&text, row.name);
            prTextBufferAppend(&text, ": ");
            prTextBufferAppend(&text, row.value);
            prTextBufferAppend(&text, "; ");
        });
        cssComputedStyleRelease(style);
    }
    cssDeclarationBlockParse(out, prTextBufferView(&text), nullptr);
    prTextBufferReset(&text);
}

bool cssPresentationComputedValue(PrCssPresentation* presentation, const PrDomElement* element, CssPseudo pseudo,
        PrCssProperty property, PrTextBuffer* out) {
    // A flow-relative property resolves as its physical counterpart, using the element's own writing mode and
    // direction.
    if (cssIsFlowRelative(property)) {
        restyle(presentation, presentation->extent, RestyleReach_All);
        uint32_t at = cssPresentationNodeEntry(presentation, element);
        if (at == kHashMissing || presentation->entries.data[at].style == nullptr)
            return true;
        const CssComputedStyle* style = targetStyle(presentation, at, pseudo);
        PrCssKeyword writingMode = cssValueKeyword(cssComputedValue(style, PrCssProperty_WritingMode).values);
        PrCssKeyword direction = cssValueKeyword(cssComputedValue(style, PrCssProperty_Direction).values);
        cssComputedStyleRelease(style);
        property = cssPhysicalProperty(property, writingMode, direction);
    }
    // Only a used size or origin reads the layout. Every other resolved value is the computed value, which a restyle
    // provides.
    if (resolvesFromLayout(property))
        cssPresentationFlush(presentation);
    else
        restyle(presentation, presentation->extent, RestyleReach_All);
    uint32_t at = cssPresentationNodeEntry(presentation, element);
    if (at == kHashMissing || presentation->entries.data[at].style == nullptr ||
            cssStyleSlot(property).group == CssStyleGroup_Count)
        return true;
    const CssComputedStyle* style = targetStyle(presentation, at, pseudo);
    serializeResolved(out, presentation, style, property,
        pseudo == CssPseudo_None ? presentation->entries.data[at].layoutNode : kPrLayoutNodeNone);
    cssComputedStyleRelease(style);
    return true;
}

bool cssPresentationComputedCustomValue(PrCssPresentation* presentation, const PrDomElement* element, CssPseudo pseudo,
        PrStringView name, PrTextBuffer* out) {
    restyle(presentation, presentation->extent, RestyleReach_All);
    uint32_t at = cssPresentationNodeEntry(presentation, element);
    if (at == kHashMissing || presentation->entries.data[at].style == nullptr)
        return true;
    const CssComputedStyle* style = targetStyle(presentation, at, pseudo);
    PrStringView value{};
    if (cssCustomValue(style->custom, name, &value))
        prTextBufferAppend(out, value);
    cssComputedStyleRelease(style);
    return true;
}

void cssPresentationPrepareJob(PrCssPresentation* presentation, PrLayoutVec extent) {
    own(presentation);
    measureJob(presentation);
    restyle(presentation, extent, RestyleReach_Job);
    applyBoxes(presentation);
}

void cssPresentationPostJob(PrCssPresentation* presentation, PrLayoutVec extent) {
    closeUpdate(presentation);
    presentation->layoutStale = false;
    presentation->sceneStale = false;
    presentation->jobTaken = presentation->newTaken;
    presentation->newTaken = 0;
    presentation->newLeft = jobShare(presentation);
    Array<PrLayoutRoot> roots;
    layoutRootsOf(presentation, extent, &roots);
    prLayoutWorkerPost(domAgentLayoutWorker(presentation->agent), presentation->tree, roots.data, roots.count);
    roots.reset();
    // The next job's styles are computed while this one lays out, and wait for it to take their boxes.
    restyleAhead(presentation, extent);
}

bool cssPresentationUsesDarkScheme(const PrCssPresentation* presentation, const PrDomElement* element) {
    uint32_t at = cssPresentationNodeEntry(presentation, element);
    const CssComputedStyle* style = at != kHashMissing ? presentation->entries.data[at].style : nullptr;
    return cssUsesDarkScheme(style, colorEnvironmentOf(presentation, presentation->headroom, nullptr));
}

} // namespace pr_impl

using namespace pr_impl;

PrCssPresentation* prCssPresentationCreate(const PrCssPresentationDesc* desc) {
    PrCssPresentation* r = allocZeroed<PrCssPresentation>();
    r->embedding = desc->embedding;
    r->document = desc->document;
    domDocumentRetain(r->document);
    r->fonts = prFontSetCreate(prTextFontSet(desc->font));
    PrLayoutTreeDesc treeDesc{.font = desc->font, .shaper = desc->shaper, .targetPixelRatio = 1};
    r->tree = prLayoutTreeCreate(&treeDesc);
    r->shaper = desc->shaper;
    for (uint32_t i = 0; i < desc->userAgentSheetCount; ++i) {
        const PrCssUserAgentSheet& sheet = desc->userAgentSheets[i];
        CssSheetData* data = cssSheetDataCreate();
        cssSheetDataParse(data, sheet.source, nullptr, CssOrigin_UserAgent);
        r->userAgent.push(CssParsedUserAgentSheet{
            .data = data,
            .conditions = sheet.conditions,
            .appearance = appearanceOf(sheet.appearance),
        });
    }
    r->extent = kDefaultExtent;
    r->schemesStale = true;
    r->rootFont = kCssInitialFontBases;
    r->fontMeasure = CssFontMeasure{measureFont, r};
    prMutexInit(&r->measuring);
    cssStyleTableInit(&r->styles);
    r->agent = desc->agent;
    r->newLeft = UINT32_MAX;
    r->changed = true;
    r->restyleAll = true;
    cssPresentationAttach(r, r->document);
    return r;
}

void prCssPresentationDestroy(PrCssPresentation* PR_NULLABLE presentation) {
    PrCssPresentation* r = presentation;
    if (r == nullptr)
        return;
    own(r);
    releaseDroppedQueue(r);
    r->boxQueueDropped.reset();
    if (r->agent->worker != nullptr)
        prLayoutWorkerForget(r->agent->worker, r->tree);
    r->erasing.reset();
    r->unmapping.reset();
    r->invalidations.reset();
    r->invalidationIndex.reset();
    r->features.reset();
    r->directionQueue.reset();
    for (uint32_t i = 0; i < r->scrolled.count; ++i)
        domNodeUnhold(r->scrolled.data[i]);
    r->scrolled.reset();
    r->anchorMoves.reset();
    r->pendingScrolls.reset();
    r->documentScrolls.reset();
    for (uint32_t i = 0; i < r->entries.count; ++i) {
        cssComputedStyleRelease(r->entries.data[i].style);
        cssComputedStyleRelease(r->entries.data[i].base);
        r->entries.data[i].matched.reset();
        cssCascadeMemoReset(&r->entries.data[i].memo);
        for (CssPresentationPseudo* pseudo : r->entries.data[i].pseudos)
            freePseudo(pseudo);
    }
    for (uint32_t i = 0; i < r->retired.count; ++i)
        freePseudo(r->retired.data[i]);
    r->retired.reset();
    r->counterScratch.steps.reset();
    r->counterScratch.counters.reset();
    r->counterScratch.boundaries.reset();
    r->counterScratch.texts.text.reset();
    r->counterScratch.texts.ends.reset();
    prTextBufferReset(&r->counterScratch.text);
    r->counterScratch.names.reset();
    r->counterScratch.points.reset();
    cssCounterResumeReset(r->counterResume);
    r->counterChanges.reset();
    cssPresentationClearSvgNotes(r);
    r->svgNotes.reset();
    cssAnimateReset(&r->animations);
    r->entries.reset();
    r->free.reset();
    cssPresentationDetach(r, r->document);
    // The text fonts read the document's set. The tree, whose font groups refer to the text fonts, is destroyed first.
    // Its compositing groups refer to the references' sources too.
    prLayoutTreeDestroy(r->tree);
    // The tree no longer draws the content of svg elements.
    for (uint32_t i = 0; i < r->svgs.count; ++i)
        freeSvg(r->svgs.data[i]);
    r->svgs.reset();
    for (uint32_t i = 0; i < r->retiredSvgs.count; ++i)
        freeSvg(r->retiredSvgs.data[i]);
    r->retiredSvgs.reset();
    r->referencing.reset();
    while (r->firsts.count != 0)
        dropFirsts(r, r->firsts.count - 1, false);
    r->firsts.reset();
    r->firstsIndex.reset();
    for (uint32_t i = 0; i < r->featureSets.count; ++i) {
        cssStyleValuesRelease(r->featureSets.data[i].group);
        prTextFeatureSetRelease(r->featureSets.data[i].set);
    }
    r->featureSets.reset();
    r->featureSetIndex.reset();
    textFontsReset(r->textFonts);
    textFontsReset(r->measured);
    prMutexReset(&r->measuring);
    prFontSetDestroy(r->fonts);
    for (uint32_t i = 0; i < r->fontFaces.count; ++i)
        cssFontFaceRelease(r->fontFaces.data[i].face);
    r->fontFaces.reset();
    r->fontFaceKeys.reset();
    r->fontFaceAddresses.reset();
    cssRuleSetReset(&r->rules);
    cssRuleSetReset(&r->userAgentRules);
    for (uint32_t i = 0; i < r->threads.count; ++i) {
        cssCascadeScratchReset(&r->threads.data[i].scratch);
        cssStyleBuilderReset(&r->threads.data[i].builder);
    }
    r->threads.reset();
    r->prefetched.reset();
    r->tasks.reset();
    cssStyleTableReset(&r->styles);
    for (uint32_t i = 0; i < r->userAgent.count; ++i)
        cssSheetDataRelease(r->userAgent.data[i].data);
    r->userAgent.reset();
    r->calc.reset();
    r->gridTracks.reset();
    r->gridNames.reset();
    r->gridRepeatNames.reset();
    r->gridAreas.reset();
    r->maskLayers.reset();
    r->maskSpans.reset();
    r->shapePoints.reset();
    r->filters.reset();
    for (uint32_t i = 0; i < r->imageFetches.count; ++i)
        r->imageFetches.data[i]->presentation = nullptr;
    r->imageFetches.reset();
    for (uint32_t i = 0; i < r->images.count; ++i) {
        prStringReset(&r->images.data[i].url);
        r->images.data[i].waiting.reset();
        prDrawImageRelease(r->images.data[i].image);
        prDrawImageRelease(r->images.data[i].stored);
        prLayoutPainterRelease(r->images.data[i].vector);
    }
    r->images.reset();
    r->imageIndex.reset();
    r->backgroundLayers.reset();
    r->backgroundStops.reset();
    r->backgroundSpans.reset();
    r->imageFilters.reset();
    r->imageSources.reset();
    domDocumentRelease(r->document);
    prMemoryFree(r);
}

bool prCssPresentationUpdate(PrCssPresentation* presentation, PrLayoutVec extent) {
    cssPresentationUpdateBoxes(presentation, extent);
    return cssPresentationUpdateLayout(presentation, extent);
}

void prCssPresentationStart(PrCssPresentation* presentation, PrLayoutVec extent) {
    cssPresentationPrepareJob(presentation, extent);
    cssPresentationPostJob(presentation, extent);
}

void prCssPresentationSetJobBudget(PrCssPresentation* presentation, double milliseconds) {
    presentation->jobBudget = prF64IsFinite(milliseconds) && milliseconds > 0 ? milliseconds : 0;
    presentation->newLeft = jobShare(presentation);
}

bool prCssPresentationIsCurrent(const PrCssPresentation* presentation) {
    // The listeners that make the wheel wait over the whole viewport are found as each job starts, since the window's
    // are not the document's to report.
    return !presentation->changed && !presentation->layoutStale && !presentation->sceneStale &&
        viewportWheelEvents(presentation) == prLayoutTreeViewportWheelEvents(presentation->tree);
}

void prCssPresentationWait(PrCssPresentation* presentation) {
    own(presentation);
}

bool prCssPresentationIsBusy(const PrCssPresentation* presentation) {
    return workerHolds(presentation);
}

const PrLayoutScene* PR_NULLABLE prCssPresentationTakeScene(PrCssPresentation* presentation) {
    own(presentation);
    PrLayoutWorker* worker = presentation->agent->worker;
    return worker != nullptr ? prLayoutWorkerTakeScene(worker, presentation->tree) : nullptr;
}

void prCssPresentationSetTime(PrCssPresentation* presentation, double milliseconds) {
    // The animations and the marks belong to the document's side, which the presentation's thread does not read.
    CssAnimateState& a = presentation->animations;
    if (milliseconds == a.time)
        return;
    // Marks the targets whose animations show something different at the new time: the targets of every transition, and
    // of each CSS animation that runs at either time. A compositor shows the composited animations. Their targets are
    // visited only when such an animation ends, because their boxes then stop being layers.
    double before = a.time;
    auto touch = [&](const PrDomNode* target) {
        mark(const_cast<PrDomNode*>(target), DomStyleFlag_Animate);
        presentation->changed = true;
    };
    for (uint32_t i = 0; i < a.animations.count; ++i) {
        CssRunningAnimation* animation = &a.animations.data[i];
        bool wasRunning = cssAnimationRunning(&a, animation);
        a.time = milliseconds;
        bool running = cssAnimationRunning(&a, animation);
        a.time = before;
        if (animation->composited ? wasRunning && !running : wasRunning || running)
            touch(animation->target);
    }
    for (uint32_t i = 0; i < a.transitions.count; ++i) {
        const CssRunningTransition& t = a.transitions.data[i];
        double end = t.start + t.delay + t.duration;
        if (!t.composited || (before < end && milliseconds >= end))
            touch(t.target);
    }
    a.time = milliseconds;
}

uint32_t prCssPresentationTracks(PrCssPresentation* presentation, PrLayoutTrack* PR_NULLABLE out, uint32_t capacity) {
    own(presentation);
    auto layoutNode = [](const PrCssPresentation* d, const PrDomNode* target) {
        uint32_t at = cssPresentationNodeEntry(d, target);
        if (at != kHashMissing && d->entries.data[at].composited)
            return d->entries.data[at].layoutNode;
        return PrLayoutNode(kPrLayoutNodeNone);
    };
    auto baseOf = [](const PrCssPresentation* d, const PrDomNode* target) -> const CssComputedStyle* {
        uint32_t at = cssPresentationNodeEntry(d, target);
        if (at == kHashMissing)
            return nullptr;
        return d->entries.data[at].base != nullptr ? d->entries.data[at].base : d->entries.data[at].style;
    };
    const Array<PrLayoutTrack>& tracks = cssAnimateTracks(presentation, layoutNode, baseOf);
    for (uint32_t i = 0; i < tracks.count && i < capacity; ++i)
        out[i] = tracks.data[i];
    return tracks.count;
}

bool prCssPresentationIsAnimating(const PrCssPresentation* presentation) {
    const CssAnimateState& a = presentation->animations;
    if (a.transitions.count != 0)
        return true;
    for (uint32_t i = 0; i < a.animations.count; ++i) {
        if (cssAnimationRunning(&a, &a.animations.data[i]))
            return true;
    }
    return false;
}

PrCssPresentationStats prCssPresentationStats(const PrCssPresentation* presentation) {
    return presentation->stats;
}

bool prCssPresentationVerify(PrCssPresentation* presentation, PrString* PR_NULLABLE report) {
    PrCssPresentation* r = presentation;
    own(r);
    Difference found{nullptr, {}};
    PrDomElement* root = domDocumentElement(r->document);
    PrLayoutNode cursor = kPrLayoutNodeNone;
    bool same =
        root == nullptr || verifyElement(r, root, nullptr, true, cssStyleFontBases(rootStyleOf(r, root)), &found, cursor);
    if (same)
        same = verifyCounters(r, &found);
    if (same)
        same = verifySvgs(r, &found);
    // What the matchers learned applies to the tree as it is now, not as the next update will find it.
    if (r->threads.count != 0)
        cssCascadeScratchForget(&r->threads.data[0].scratch);
    if (same) {
        PrLayoutTreeDifference difference{};
        same = prLayoutTreeVerify(r->tree, &difference);
        if (!same) {
            found.node = difference.node != kPrLayoutNodeNone ? prCssPresentationNode(r, difference.node) : nullptr;
            prTextBufferAppend(&found.what, prStringView(&difference.what));
            prStringReset(&difference.what);
        }
    }
    if (!same && report != nullptr) {
        PrTextBuffer text{};
        if (found.node != nullptr) {
            describeNode(&text, found.node);
            prTextBufferAppend(&text, ": ");
        }
        prTextBufferAppend(&text, prTextBufferView(&found.what));
        prStringReset(report);
        *report = prTextBufferTake(&text);
    }
    prTextBufferReset(&found.what);
    return same;
}

PrLayoutTree* prCssPresentationTree(PrCssPresentation* presentation) {
    own(presentation);
    return presentation->tree;
}

PrDomNode* PR_NULLABLE prCssPresentationNode(const PrCssPresentation* presentation, PrLayoutNode node) {
    // Waiting for the worker changes nothing that a caller observes.
    PrCssPresentation* r = const_cast<PrCssPresentation*>(presentation);
    own(r);
    return seen(presented(r, node));
}

PrLayoutNode prCssPresentationLayoutNode(const PrCssPresentation* presentation, const PrDomNode* node) {
    own(const_cast<PrCssPresentation*>(presentation));
    return layoutNodeOf(presentation, node);
}

PrLayoutNode prCssPresentationFieldText(const PrCssPresentation* presentation, const PrDomElement* element) {
    uint32_t at = cssPresentationNodeEntry(presentation, element);
    if (at == kHashMissing)
        return kPrLayoutNodeNone;
    const CssPresentationPseudo* pseudo = presentation->entries.data[at].pseudos[kFieldContentPseudo];
    uint32_t own = pseudo != nullptr ? cssPresentationNodeEntry(presentation, pseudo) : kHashMissing;
    if (own == kHashMissing || !presentation->entries.data[own].boxed)
        return kPrLayoutNodeNone;
    return prLayoutNodeFirstChild(presentation->tree, presentation->entries.data[own].layoutNode);
}

bool prCssPresentationFieldMasked(const PrCssPresentation* presentation, const PrDomElement* element) {
    return isMasked(presentation, element);
}

void prCssPresentationSelectionColors(PrCssPresentation* presentation, const PrDomElement* element, PrColor16* color,
        PrColor16* background) {
    *color = PrColor16{};
    *background = PrColor16{};
    uint32_t at = cssPresentationNodeEntry(presentation, element);
    const CssComputedStyle* parent = at != kHashMissing ? presentation->entries.data[at].style : nullptr;
    if (parent == nullptr)
        return;
    const CssComputedStyle* style = cascadeFirst(presentation, element, CssPseudo_Selection, parent);
    if (style == nullptr)
        return;
    CssColorEnvironment environment = colorEnvironmentOf(presentation, 0, nullptr);
    *color = prColor16FromColor(cssStyleColor(style, environment));
    PrColor fill{};
    CssColorContext colors = cssStyleColorContext(style, environment);
    if (cssResolveColor(cssComputedValue(style, PrCssProperty_BackgroundColor), 0, colors, &fill))
        *background = prColor16FromColor(fill);
    cssComputedStyleRelease(style);
}

PrColor16 prCssPresentationCaretColor(PrCssPresentation* presentation, const PrDomElement* element) {
    uint32_t at = cssPresentationNodeEntry(presentation, element);
    const CssComputedStyle* style = at != kHashMissing ? presentation->entries.data[at].style : nullptr;
    if (style == nullptr)
        return prColor16FromColor(PrColor{0, 0, 0, 1});
    CssColorEnvironment environment = colorEnvironmentOf(presentation, 0, nullptr);
    PrColor color = cssStyleColor(style, environment);
    if (cssComputedFirstKeyword(style, PrCssProperty_CaretColor) != PrCssKeyword_Auto)
        cssResolveColor(cssComputedValue(style, PrCssProperty_CaretColor), 0, cssStyleColorContext(style, environment), &color);
    return prColor16FromColor(color);
}

void prCssPresentationSetScroll(PrCssPresentation* presentation, PrLayoutNode node, PrLayoutVec offset) {
    if (prCssPresentationIsBusy(presentation)) {
        presentation->pendingScrolls.push(PrLayoutSceneScroll{node, offset});
        return;
    }
    own(presentation);
    scrollTree(presentation, node, offset, CssPresentationScrollBy_Host);
}

PrLayoutVec prCssPresentationScrollFrom(PrCssPresentation* presentation, PrLayoutNode node, PrLayoutVec delta) {
    own(presentation);
    PrLayoutTree* tree = presentation->tree;
    PrLayoutNode container = kPrLayoutNodeNone;
    PrLayoutScrollInfo info{};
    if (node != kPrLayoutNodeNone && prLayoutNodeIsValid(tree, node)) {
        bool scrolls = prLayoutNodeScrollInfo(tree, node, &info) && info.scrollContainer;
        container = scrolls ? node : prLayoutNodeScrollContainer(tree, node);
    }
    auto clamp = [](PrLayoutUnit unit, PrLayoutUnit least, PrLayoutUnit greatest) {
        return unit < least ? least : unit > greatest ? greatest : unit;
    };
    PrLayoutVec rest = delta;
    for (;;) {
        if (prLayoutNodeScrollInfo(tree, container, &info) && info.scrollContainer) {
            PrLayoutVec to = info.offset;
            if ((info.userAxes & PrLayoutScrollFlag_X) != 0)
                to.x = clamp(info.offset.x + rest.x, info.range.left, info.range.right);
            if ((info.userAxes & PrLayoutScrollFlag_Y) != 0)
                to.y = clamp(info.offset.y + rest.y, info.range.top, info.range.bottom);
            if (to != info.offset) {
                scrollTree(presentation, container, to, CssPresentationScrollBy_Document);
                rest -= to - info.offset;
            }
            if ((info.chainAxes & PrLayoutScrollFlag_X) == 0)
                rest.x = PrLayoutUnit{};
            if ((info.chainAxes & PrLayoutScrollFlag_Y) == 0)
                rest.y = PrLayoutUnit{};
        }
        if (container == kPrLayoutNodeNone || rest == PrLayoutVec{})
            return rest;
        container = prLayoutNodeScrollContainer(tree, container);
    }
}

void prCssPresentationReveal(PrCssPresentation* presentation, PrLayoutNode node, PrLayoutRect rect) {
    own(presentation);
    PrLayoutVec offset{};
    while ((node = prLayoutNodeRevealStep(presentation->tree, node, &rect, &offset)) != kPrLayoutNodeNone)
        scrollTree(presentation, node, offset, CssPresentationScrollBy_Document);
}

uint32_t prCssPresentationTakeScrolls(PrCssPresentation* presentation, PrLayoutSceneScroll* PR_NULLABLE out,
        uint32_t capacity) {
    uint32_t count = presentation->documentScrolls.count < capacity ? presentation->documentScrolls.count : capacity;
    for (uint32_t i = 0; i < count; ++i)
        out[i] = presentation->documentScrolls.data[i];
    presentation->documentScrolls.erase(0, count);
    return count;
}

PrDomNode* PR_NULLABLE prCssPresentationHitTest(PrCssPresentation* presentation, PrLayoutVec point) {
    Array<PrDomNode*> hits;
    cssPresentationHits(presentation, point, 1, &hits);
    PrDomNode* first = hits.count != 0 ? hits.data[0] : nullptr;
    hits.reset();
    return first;
}

PrCssStyleProperties* prCssElementComputedStyle(PrDomElement* element, PrStringView pseudoElement) {
    return static_cast<PrCssStyleProperties*>(cssElementComputedStyle(element, pseudoElement));
}
