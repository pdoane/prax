// Page boxes: what a document's @page rules give each page.

#include "css/src/css_page.h"

#include "pr/ascii.h"
#include "pr/css.h"
#include "pr/float.h"
#include "pr/layout_style.h"
#include "pr/mem.h"

#include "css/src/css_at_rule.h"
#include "css/src/css_object.h"
#include "css/src/css_root.h"
#include "css/src/css_rule.h"
#include "css/src/css_style.h"
#include "css/src/css_unit.h"
#include "css/src/css_value.h"

namespace pr_impl {

namespace {

// A page selector (css-page-3/#page-selectors): a page name, which may be empty, and the pseudo-classes after it.
struct PageSelector {
    PrRawString name;
    bool first;
    bool blank;
    bool left;
    bool right;
    uint32_t pseudos;               // the number of :first and :blank pseudo-classes
    uint32_t sides;                 // the number of :left and :right pseudo-classes
};

// An @page rule that applies in the document's media: its selectors, empty for a rule that matches every page, and its
// declarations.
struct PageRule {
    RawArray<PageSelector> selectors;
    const CssDeclarationBlock* block;
    const CssRuleData* data;
};

// Parses a page selector list written as `text` into `out`. A selector that does not parse is skipped.
void parseSelectors(PrStringView text, RawArray<PageSelector>& out) {
    size_t at = 0;
    while (at <= text.size) {
        size_t end = at;
        while (end < text.size && text.data[end] != ',')
            ++end;
        PrStringView part{text.data + at, end - at};
        PageSelector selector{};
        size_t i = 0;
        auto skipSpace = [&]() {
            while (i < part.size && prAsciiIsSpace(part.data[i]))
                ++i;
        };
        skipSpace();
        size_t nameStart = i;
        while (i < part.size && part.data[i] != ':' && !prAsciiIsSpace(part.data[i]))
            ++i;
        bool valid = true;
        if (i > nameStart)
            selector.name = prRawStringCopy(PrStringView{part.data + nameStart, i - nameStart});
        while (i < part.size && valid) {
            skipSpace();
            if (i >= part.size)
                break;
            if (part.data[i] != ':') {
                valid = false;
                break;
            }
            size_t start = ++i;
            while (i < part.size && part.data[i] != ':' && !prAsciiIsSpace(part.data[i]))
                ++i;
            PrStringView pseudo{part.data + start, i - start};
            if (prAsciiEqualsFold(pseudo, "first")) {
                selector.first = true;
                ++selector.pseudos;
            } else if (prAsciiEqualsFold(pseudo, "blank")) {
                selector.blank = true;
                ++selector.pseudos;
            } else if (prAsciiEqualsFold(pseudo, "left")) {
                selector.left = true;
                ++selector.sides;
            } else if (prAsciiEqualsFold(pseudo, "right")) {
                selector.right = true;
                ++selector.sides;
            } else {
                valid = false;
            }
        }
        if (valid)
            out.push(selector);
        else
            prRawStringReset(&selector.name);
        at = end + 1;
    }
}

// Appends the @page rules of `list`, and of the rules nested in it whose conditions hold in `media`, in order.
void collectRules(PrCssRuleList* list, const CssMediaEnvironment* media, RawArray<PageRule>& out);

void collectSheet(PrCssStyleSheet* sheet, const CssMediaEnvironment* media, RawArray<PageRule>& out) {
    if (!sheet->disabled && cssMediaListMatches(&sheet->media, media))
        collectRules(&sheet->cssRules, media, out);
}

void collectRules(PrCssRuleList* list, const CssMediaEnvironment* media, RawArray<PageRule>& out) {
    const RawArray<CssRuleData* PR_NONNULL>& rules = cssRuleListData(list);
    for (uint32_t i = 0; i < rules.count; ++i) {
        const CssRuleData* data = rules.data[i];
        if (data->kind != CssRuleDataKind_At)
            continue;
        CssAtRule atRule = kCssAtRuleAlias[data->atRule];
        PrCssRule* rule = cssRuleListItem(list, i);
        if (atRule == CssAtRule_Page) {
            PageRule page{.block = &data->style, .data = data};
            PrRawString text = prCssPageRuleSelectorText(prCssPageRuleQuery(rule));
            parseSelectors(prRawStringView(&text), page.selectors);
            prRawStringReset(&text);
            out.push(page);
        } else if (atRule == CssAtRule_Media && cssMediaListMatches(&data->media, media)) {
            collectRules(&rule->cssRules, media, out);
        } else if (atRule == CssAtRule_Layer && data->block) {
            collectRules(&rule->cssRules, media, out);
        } else if (atRule == CssAtRule_Import && data->imported != nullptr && cssMediaListMatches(&data->media, media)) {
            collectSheet(cssImportRuleSheet(rule), media, out);
        }
    }
}

// Returns the specificity of `selector` as one number (css-page-3/#cascading-and-page-context): its page name first,
// then its :first and :blank pseudo-classes, then its :left and :right pseudo-classes. Returns false if it does not
// match the page `page` of the name `name`.
bool matches(const PageSelector& selector, PrStringView name, const PrLayoutPageContext& page, uint32_t* specificity) {
    PrStringView own = prRawStringView(&selector.name);
    if (own.size != 0 && own != name)
        return false;
    if ((selector.first && !page.first) || (selector.blank && !page.blank) ||
            (selector.left && page.side != PrLayoutPageSide_Left) ||
            (selector.right && page.side != PrLayoutPageSide_Right))
        return false;
    *specificity = (own.size != 0 ? 1u << 16 : 0u) | selector.pseudos << 8 | selector.sides;
    return true;
}

// A rule that applies to a page, with its specificity, in the order of the cascade.
struct Applied {
    uint32_t specificity;
    uint32_t order;
    const CssDeclarationBlock* block;
    const CssRuleData* data;
};

// Returns the value that wins the cascade of `name` among the rules that apply, in the order of the cascade, and sets
// `*found`: the last important declaration, or else the last declaration (css-cascade-5/#cascade-sort).
CssValueRun winner(const RawArray<Applied>& applied, const CssDeclaredName& name, bool* found) {
    CssValueRun value{};
    bool important = false;
    *found = false;
    for (uint32_t i = 0; i < applied.count; ++i) {
        const CssDeclarationBlock* block = applied.data[i].block;
        uint32_t at = cssDeclarationBlockFind(block, name);
        if (at >= block->count)
            continue;
        const CssDeclaration* declaration = &cssDeclarations(block)[at];
        if (important && !declaration->important)
            continue;
        important = declaration->important;
        value = cssDeclarationValue(block, declaration);
        *found = true;
    }
    return value;
}

// Returns the px of a length or a percentage of `base`, or false if `run` holds neither. Lengths are relative to
// `context`.
bool lengthOf(CssValueRun run, double base, const CssStyleContext& context, double* out) {
    if (run.count == 0)
        return false;
    const CssValueNode& value = run.values[0];
    switch (value.kind) {
        case PrCssValueKind_Percentage: *out = value.number / 100 * base; return true;
        case PrCssValueKind_Integer:
        case PrCssValueKind_Number:
            if (value.number != 0)
                return false;
            *out = 0;
            return true;
        case PrCssValueKind_Dimension:
            return cssUnitCategory(value.unit) == CssUnitCategory_Length &&
                cssStyleLengthPx(value.number, value.unit, context, out);
        default: return false;
    }
}

// Returns the extent of a page-size keyword in px, portrait, or false if `keyword` names none (css-page-3/#page-size).
bool pageSizeOf(PrCssKeyword keyword, double* width, double* height) {
    constexpr double kMm = 96 / 25.4;
    double w = 0;
    double h = 0;
    switch (keyword) {
        case PrCssKeyword_A5: w = 148 * kMm; h = 210 * kMm; break;
        case PrCssKeyword_A4: w = 210 * kMm; h = 297 * kMm; break;
        case PrCssKeyword_A3: w = 297 * kMm; h = 420 * kMm; break;
        case PrCssKeyword_B5: w = 176 * kMm; h = 250 * kMm; break;
        case PrCssKeyword_B4: w = 250 * kMm; h = 353 * kMm; break;
        case PrCssKeyword_JisB5: w = 182 * kMm; h = 257 * kMm; break;
        case PrCssKeyword_JisB4: w = 257 * kMm; h = 364 * kMm; break;
        case PrCssKeyword_Letter: w = 8.5 * 96; h = 11 * 96; break;
        case PrCssKeyword_Legal: w = 8.5 * 96; h = 14 * 96; break;
        case PrCssKeyword_Ledger: w = 11 * 96; h = 17 * 96; break;
        default: return false;
    }
    *width = w;
    *height = h;
    return true;
}

// Applies the size descriptor's value `run` to `box` (css-page-3/#page-size-prop). Auto keeps the box's extent.
void applySize(CssValueRun run, const CssStyleContext& context, PrLayoutPageBox& box) {
    double width = prLayoutUnitToPxDouble(box.extent.x);
    double height = prLayoutUnitToPxDouble(box.extent.y);
    double lengths[2] = {0, 0};
    uint32_t count = 0;
    PrCssKeyword orientation = PrCssKeyword_Auto;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        CssValueRun one{run.values + i, run.count - i, run.text};
        const CssValueNode& value = run.values[i];
        if (value.kind == PrCssValueKind_Keyword) {
            PrCssKeyword keyword = cssValueKeyword(&value);
            if (keyword == PrCssKeyword_Landscape || keyword == PrCssKeyword_Portrait)
                orientation = keyword;
            else
                pageSizeOf(keyword, &width, &height);
        } else if (count < 2 && lengthOf(one, 0, context, &lengths[count])) {
            ++count;
        }
        if (value.next == 0)
            break;
    }
    if (count == 0 && orientation == PrCssKeyword_Auto && run.count != 0 &&
            cssValueRunFirstKeyword(run) == PrCssKeyword_Auto)
        return;
    if (count != 0) {
        width = lengths[0];
        height = count == 2 ? lengths[1] : lengths[0];
    }
    bool wide = width > height;
    if ((orientation == PrCssKeyword_Landscape && !wide) || (orientation == PrCssKeyword_Portrait && wide)) {
        double swap = width;
        width = height;
        height = swap;
    }
    box.extent = PrLayoutVec{prLayoutUnitFromPxDouble(width), prLayoutUnitFromPxDouble(height)};
}

// Returns the marks that the marks descriptor's value `run` gives (css-page-3/#marks).
PrLayoutPageMarkFlags marksOf(CssValueRun run) {
    PrLayoutPageMarkFlags marks = PrLayoutPageMarkFlag_None;
    for (uint32_t i = 0; i < run.count; i += run.values[i].next) {
        const CssValueNode& value = run.values[i];
        if (value.kind == PrCssValueKind_Keyword) {
            PrCssKeyword keyword = cssValueKeyword(&value);
            if (keyword == PrCssKeyword_Crop)
                marks |= PrLayoutPageMarkFlag_Crop;
            else if (keyword == PrCssKeyword_Cross)
                marks |= PrLayoutPageMarkFlag_Cross;
        }
        if (value.next == 0)
            break;
    }
    return marks;
}

// Returns the box of the page `page` named `name`, from the rules that apply to it over `initial`, and appends to
// `applied` the rules that apply to it in the order of the cascade, each with a reference.
PrLayoutPageBox boxOf(const RawArray<PageRule>& rules, PrStringView name, const PrLayoutPageContext& page,
        const CssMediaEnvironment* media, const PrLayoutPageBox& initial, RawArray<CssRuleData*>& rulesOut) {
    RawArray<Applied> applied;
    for (uint32_t i = 0; i < rules.count; ++i) {
        const PageRule& rule = rules.data[i];
        bool any = rule.selectors.count == 0;
        uint32_t best = 0;
        for (uint32_t k = 0; k < rule.selectors.count; ++k) {
            uint32_t specificity = 0;
            if (matches(rule.selectors.data[k], name, page, &specificity)) {
                if (!any || specificity > best)
                    best = specificity;
                any = true;
            }
        }
        if (any)
            applied.push(Applied{best, i, rule.block, rule.data});
    }
    // The cascade sorts by specificity, then by order.
    for (uint32_t i = 1; i < applied.count; ++i) {
        Applied moved = applied.data[i];
        uint32_t at = i;
        while (at > 0 && applied.data[at - 1].specificity > moved.specificity) {
            applied.data[at] = applied.data[at - 1];
            --at;
        }
        applied.data[at] = moved;
    }
    PrLayoutPageBox box = initial;
    // The page context's lengths are relative to the initial font, and its viewport-percentage lengths to the paper's
    // page box, which media queries see (css-page-3/#page-size-prop). The page area that the rules decide cannot be the
    // base of their own lengths.
    double em = media->fontSize;
    CssStyleContext context{
        .font = cssFontBases(em, em * kCssNormalLineHeight),
        .root = cssFontBases(em, em * kCssNormalLineHeight),
        .viewport = CssViewport{
            float(media->features[PrCssDescriptor_MediaWidth - kCssDescriptorMediaFirst].number),
            float(media->features[PrCssDescriptor_MediaHeight - kCssDescriptorMediaFirst].number),
        },
    };
    bool found = false;
    CssValueRun size = winner(applied, cssDescriptorDeclaredName(PrCssDescriptor_PageSize), &found);
    if (found)
        applySize(size, context, box);
    // A margin's percentage is of the page box's width for the left and right margins, and of its height for the top
    // and bottom ones (css-page-3/#margin-dimension).
    const PrCssProperty sides[] = {
        PrCssProperty_MarginTop, PrCssProperty_MarginRight, PrCssProperty_MarginBottom, PrCssProperty_MarginLeft,
    };
    for (uint32_t side = 0; side < PrRectSide_Count; ++side) {
        CssValueRun margin = winner(applied, cssPropertyDeclaredName(sides[side]), &found);
        bool across = side == PrRectSide_Left || side == PrRectSide_Right;
        double base = prLayoutUnitToPxDouble(across ? box.extent.x : box.extent.y);
        double px = 0;
        if (found && lengthOf(margin, base, context, &px))
            box.margin[side] = prLayoutUnitFromPxDouble(px);
    }
    CssValueRun marks = winner(applied, cssDescriptorDeclaredName(PrCssDescriptor_PageMarks), &found);
    if (found)
        box.marks = marksOf(marks);
    // Auto bleeds 6 pt for crop marks, and not otherwise (css-page-3/#bleed).
    CssValueRun bleed = winner(applied, cssDescriptorDeclaredName(PrCssDescriptor_PageBleed), &found);
    double bleedPx = (box.marks & PrLayoutPageMarkFlag_Crop) != 0 ? 8 : 0;
    if (found)
        lengthOf(bleed, 0, context, &bleedPx);
    box.bleed = prLayoutUnitFromPxDouble(bleedPx);
    CssValueRun background = winner(applied, cssPropertyDeclaredName(PrCssProperty_BackgroundColor), &found);
    PrColor color{0, 0, 0, 0};
    CssColorContext colors{.current = PrColor{0, 0, 0, 1}, .environment = kCssDefaultColorEnvironment};
    if (found && cssResolveColor(background, 0, colors, &color))
        box.background = prColor16FromColor(color);
    for (uint32_t i = 0; i < applied.count; ++i) {
        CssRuleData* data = const_cast<CssRuleData*>(applied.data[i].data);
        cssRuleDataRetain(data);
        rulesOut.push(data);
    }
    applied.reset();
    return box;
}

} // namespace

void cssPageBoxesResolve(PrDomDocument* document, const CssMediaEnvironment* media, PrLayoutTree* tree,
        const PrLayoutPageBox& initial, CssPageBoxes& out) {
    cssPageBoxesReset(out);
    RawArray<PageRule> rules;
    PrCssStyleSheetList* sheets = cssDocumentStyleSheets(document);
    for (uint32_t i = 0; i < prCssStyleSheetListLength(sheets); ++i) {
        PrCssStyleSheet* sheet = prCssStyleSheetListStyleSheetAt(sheets, i);
        if (sheet != nullptr)
            collectSheet(sheet, media, rules);
    }
    // The names that the selectors use, and none.
    RawArray<PrStringView> names;
    names.push(PrStringView{});
    for (uint32_t i = 0; i < rules.count; ++i) {
        for (uint32_t k = 0; k < rules.data[i].selectors.count; ++k) {
            PrStringView name = prRawStringView(&rules.data[i].selectors.data[k].name);
            bool known = false;
            for (uint32_t n = 0; n < names.count && !known; ++n)
                known = names.data[n] == name;
            if (!known)
                names.push(name);
        }
    }
    for (uint32_t n = 0; n < names.count; ++n) {
        PrLayoutName name = names.data[n].size != 0 ? prLayoutNameIntern(tree, names.data[n]) : kPrLayoutNameNone;
        for (uint32_t kind = 0; kind < 8; ++kind) {
            PrLayoutPageSide side = PrLayoutPageSide_Right;
            if ((kind & 1) != 0)
                side = PrLayoutPageSide_Left;
            PrLayoutPageContext page{.name = name, .side = side, .first = (kind & 2) != 0, .blank = (kind & 4) != 0};
            CssPageBox kindBox{.name = name, .first = page.first, .blank = page.blank, .side = page.side};
            kindBox.box = boxOf(rules, names.data[n], page, media, initial, kindBox.rules);
            out.boxes.push(kindBox);
        }
    }
    names.reset();
    for (uint32_t i = 0; i < rules.count; ++i) {
        for (uint32_t k = 0; k < rules.data[i].selectors.count; ++k)
            prRawStringReset(&rules.data[i].selectors.data[k].name);
        rules.data[i].selectors.reset();
    }
    rules.reset();
}

uint32_t cssPageBoxesKind(const CssPageBoxes& boxes, const PrLayoutPageContext& page) {
    uint32_t unnamed = UINT32_MAX;
    for (uint32_t i = 0; i < boxes.boxes.count; ++i) {
        const CssPageBox& entry = boxes.boxes.data[i];
        if (entry.first != page.first || entry.blank != page.blank || entry.side != page.side)
            continue;
        if (entry.name == page.name)
            return i;
        if (entry.name == kPrLayoutNameNone)
            unnamed = i;
    }
    return unnamed;
}

PrLayoutPageBox cssPageBoxesFind(const CssPageBoxes& boxes, const PrLayoutPageContext& page, const PrLayoutPageBox& initial) {
    const CssPageBox* unnamed = nullptr;
    for (uint32_t i = 0; i < boxes.boxes.count; ++i) {
        const CssPageBox& entry = boxes.boxes.data[i];
        if (entry.first != page.first || entry.blank != page.blank || entry.side != page.side)
            continue;
        if (entry.name == page.name)
            return entry.box;
        if (entry.name == kPrLayoutNameNone)
            unnamed = &entry;
    }
    return unnamed != nullptr ? unnamed->box : initial;
}

bool cssPageBoxesEqual(const CssPageBoxes& a, const CssPageBoxes& b) {
    if (a.boxes.count != b.boxes.count)
        return false;
    for (uint32_t i = 0; i < a.boxes.count; ++i) {
        const CssPageBox& x = a.boxes.data[i];
        const CssPageBox& y = b.boxes.data[i];
        if (x.name != y.name || x.first != y.first || x.blank != y.blank || x.side != y.side ||
                !prMemEquals(&x.box, &y.box, sizeof(PrLayoutPageBox)))
            return false;
    }
    return true;
}

void cssPageBoxesReset(CssPageBoxes& boxes) {
    for (uint32_t i = 0; i < boxes.boxes.count; ++i) {
        RawArray<CssRuleData*>& rules = boxes.boxes.data[i].rules;
        for (uint32_t k = 0; k < rules.count; ++k)
            cssRuleDataRelease(rules.data[k]);
        rules.reset();
    }
    boxes.boxes.reset();
}

} // namespace pr_impl
