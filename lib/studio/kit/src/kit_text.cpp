// Studio Kit's text: labels and paragraphs in the theme's roles and tones.

#include "pr/kit_text.h"

#include "studio/kit/src/kit.h"

namespace pr_impl {

namespace {

// A label's box: a block, which clips its text and gives way to its parent if `clips` is set, and is as wide as its
// content needs otherwise.
PR_NO_PADDING_BEGIN
struct LabelBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    bool clips;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Block;
        if (clips) {
            box.overflowX = box.overflowY = PrLayoutOverflow_Clip;
            box.minWidth = prLayoutLengthPx(0);
        }
    }
};
PR_NO_PADDING_END

// Returns the record of a label or a paragraph in `style`.
PrLayoutStyleIndex labelStyle(PrKit& kit, const PrKitTextStyle& style) {
    PrKitWrap wrap = kitTextWrap(kit, style.role, style.wrap);
    return kitStyle(kit, LabelBox{.clips = wrap == PrKitWrap_Clip},
        KitFont{.role = style.role, .wrap = wrap, .align = style.align, .preserve = style.preserve},
        KitToneInk{.tone = kitTextTone(kit, style.role, style.tone)});
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prKitLabel(PrKit* kit, PrLayoutNode parent, PrKitLabel label, PrKitTextStyle style) {
    PrLayoutNode node = prLayoutTreeFrameElement(kit->tree, parent, label.key, labelStyle(*kit, style));
    prLayoutTreeFrameText(kit->tree, node, 0, label.text);
    return node;
}

PrLayoutNode prKitText(PrKit* kit, PrLayoutNode parent, PrKitText text, PrKitTextStyle style) {
    PrLayoutNode paragraph = prLayoutTreeFrameElement(kit->tree, parent, text.key, labelStyle(*kit, style));
    for (uint32_t i = 0; i < text.count; ++i) {
        const PrKitTextRun& run = text.runs[i];
        // A run is an inline box, layout's default, in its own role and tone.
        KitToneInk ink{.tone = kitTextTone(*kit, run.role, run.tone)};
        PrLayoutStyleIndex record = kitStyle(*kit, KitFont{.role = run.role}, ink);
        PrLayoutNode node = prLayoutTreeFrameElement(kit->tree, paragraph, 0, record);
        prLayoutTreeFrameText(kit->tree, node, 0, run.text);
    }
    return paragraph;
}

PrColor prKitToneColor(const PrKit* kit, PrKitTone tone) {
    return kitToneColor(*kit, tone);
}
