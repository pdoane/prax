#include "pr/layout_animation.h"
#include "pr/layout_compositor.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/layout_worker.h"
#include "pr/text_harfbuzz.h"

int prTestLayoutCApi(void);

// The public headers compile as C, and their values' constructors work there. A record interns once, and a tree lays
// out an empty root.
int prTestLayoutCApi(void) {
    PrFontSet* fonts = prFontSetCreate(NULL);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFontDesc defaultFontDesc = {.set = fonts, .style = prFontStyleDefault()};
    PrTextFont* defaultFont = prTextFontCreate(&defaultFontDesc);
    PrLayoutTreeDesc desc = {.font = defaultFont, .shaper = shaper, .targetPixelRatio = 1};
    PrLayoutTree* tree = prLayoutTreeCreate(&desc);
    prTextFontRelease(defaultFont);
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    box.width = prLayoutLengthPercent(100);
    PrLayoutNumber number = prLayoutNumber(1.5f);
    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
    decoration.background = PR_COLOR16(0, 0.5f, 1, 1);
    int result = prLayoutWordNumberValue(number.bits) != 1.5f || prLayoutLengthPx(2).bits != 2u * 64 << 1
                 || prColorFromColor16(decoration.background).g != 0.5f;
    PrLayoutStyle style = prLayoutStyleDefault();
    style.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
    uint32_t record = prLayoutStyleIntern(tree, &style);
    result |= record == 0 || prLayoutStyleIntern(tree, &style) != record;
    PrLayoutNode root = prLayoutNodeCreateElement(tree, kPrLayoutNodeNone, record);
    PrLayoutRoot viewport = {root, prLayoutVecFromPx((PrVec2){100, 50})};
    prLayoutTreeLayout(tree, &viewport, 1);
    PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(tree, root, PrLayoutCoordinates_Viewport));
    result |= bounds.right != 100 || bounds.bottom != 0;
    prLayoutTreeDestroy(tree);
    prTextShaperDestroy(shaper);
    prFontSetDestroy(fonts);
    return result;
}
