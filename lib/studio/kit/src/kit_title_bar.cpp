// Studio Kit's title bar.

#include "pr/kit_title_bar.h"

#include "pr/kit_box.h"
#include "pr/ui.h"

#include "studio/kit/src/kit.h"
#include "studio/kit/src/kit_icon.h"

namespace pr_impl {

namespace {

// The width of a window button that the program draws, in px. Windows' own caption buttons are this wide at every
// height.
constexpr float kWindowButtonWidth = 46;

// The size of a window button's glyph, in px.
constexpr float kWindowButtonGlyph = 10;

// A window button's box: as tall as the title bar, flush with its neighbors, and without a border.
PR_NO_PADDING_BEGIN
struct WindowButtonBox {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Box;
    float height;

    void make(const PrKit&, PrLayoutBoxStyle& box) const {
        box.display = PrLayoutDisplay_Flex;
        box.boxSizing = PrLayoutBoxSizing_BorderBox;
        box.width = prLayoutLengthPx(kWindowButtonWidth);
        box.height = prLayoutLengthPx(height);
    }
};
PR_NO_PADDING_END

// A window button's flex: its glyph centered, and the button never shrinks.
struct WindowButtonFlex {
    static constexpr PrLayoutGroup kKind = PrLayoutGroup_Flex;
    void make(const PrKit&, PrLayoutFlexStyle& flex) const {
        flex.alignItems = PrLayoutAlignment_Center;
        flex.justifyContent = PrLayoutAlignment_Center;
        flex.shrink = 0;
    }
};

// Creates one of the window's buttons, which the system acts on (PrAppTitleBarRegionKind). The program sees only the
// pointer over it, which shows the button's hover.
void windowButton(PrKit& kit, PrLayoutNode parent, float height, PrKitIconKind icon, PrKitLookId look,
        PrAppTitleBarRegionKind kind) {
    KitControl c = kitControl(kit, parent, 0, PrUiFlag_Pressable, false);
    KitDress dress = kitDress(kit, c, look);
    prLayoutTreeFrameSetStyle(kit.tree, c.node,
        kitStyle(kit, WindowButtonBox{.height = height}, WindowButtonFlex{}, dress.fill, dress.ink));
    kitIcon(kit, c.node, 0, icon, kWindowButtonGlyph, kit.looks[look].icon);
    prKitPlace(&kit, c.node, {.align = PrKitAlign_Start});
    prUiTitleBarRegion(kit.ui, c.node, kind);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrLayoutNode prKitTitleBar(PrKit* kit, PrLayoutNode parent, PrKitTitleBar titleBar) {
    PrKit& k = *kit;
    const PrAppTitleBar& bar = titleBar.bar;
    PrKitBoxStyle bandStyle{.direction = PrKitDirection_Row, .align = PrKitAlign_Center};
    if (bar.height > 0) {
        bandStyle.padding = PrKitEdges{0, bar.trailing, 0, bar.leading};
        bandStyle.place.minHeight = prKitPx(bar.height);
        bandStyle.place.flex = PrKitFlex_Fixed;
    }
    PrLayoutNode band = prKitBox(kit, parent, PrKitBox{.key = titleBar.key}, bandStyle);
    PrLayoutNode content = prKitBox(kit, band, PrKitBox{}, PrKitBoxStyle{
        .direction = PrKitDirection_Row,
        .align = PrKitAlign_Center,
        .gap = kPrKitSpace3,
        .padding = PrKitEdges{0, kPrKitSpace4, 0, kPrKitSpace4},
        .place = {.flex = PrKitFlex_Fill},
    });
    if (bar.height <= 0)
        return content;
    prUiTitleBarRegion(k.ui, band, PrAppTitleBarRegionKind_Drag);
    if ((bar.flags & PrAppTitleBarFlag_ProgramButtons) != 0) {
        PrKitIconKind maximize = titleBar.maximized ? PrKitIconKind_WindowRestore : PrKitIconKind_WindowMaximize;
        windowButton(k, band, bar.height, PrKitIconKind_WindowMinimize, PrKitLookId_WindowButton,
            PrAppTitleBarRegionKind_Minimize);
        windowButton(k, band, bar.height, maximize, PrKitLookId_WindowButton, PrAppTitleBarRegionKind_Maximize);
        windowButton(k, band, bar.height, PrKitIconKind_Close, PrKitLookId_WindowClose, PrAppTitleBarRegionKind_Close);
    }
    return content;
}
