// Checks that another flexbox can set Prax text: Yoga measures text through lib/text alone, with no layout tree. The
// program exits with a nonzero status if a result is wrong.

#include "pr/font_set.h"
#include "pr/text_harfbuzz.h"
#include "pr/text_shaping.h"

#include <stdio.h>
#include <string>
#include <yoga/Yoga.h>

namespace {

constexpr float kLineHeight = 20;

// The text that a Yoga node shows.
struct TextNode {
    std::string text;
    PrTextShaping* shaping;
    uint32_t measures;
};

// Breaks the text of `node` into lines at the width that Yoga asks about, without shaping it again.
YGSize measure(YGNodeConstRef node, float width, YGMeasureMode widthMode, float, YGMeasureMode) {
    TextNode& t = *static_cast<TextNode*>(YGNodeGetContext(node));
    ++t.measures;
    uint32_t size = uint32_t(t.text.size());
    PrTextIntrinsic sizes = prTextShapingIntrinsic(t.shaping, 0, size, nullptr, nullptr);
    if (widthMode == YGMeasureModeUndefined)
        return YGSize{sizes.maxContent, kLineHeight};
    uint32_t lines = 0;
    float widest = 0;
    for (uint32_t start = 0; start < size; ++lines) {
        PrTextShapingLine line{.start = start, .end = size};
        line.end = prTextShapingBreakLine(t.shaping, t.text, &line, width, nullptr);
        float lineWidth = prTextShapingLineWidth(t.shaping, &line);
        widest = lineWidth > widest ? lineWidth : widest;
        start = line.end;
    }
    return YGSize{widthMode == YGMeasureModeExactly ? width : widest, float(lines) * kLineHeight};
}

int failures = 0;

void check(bool passed, const char* what) {
    if (passed)
        return;
    fprintf(stderr, "text_yoga_check: %s\n", what);
    ++failures;
}

} // namespace

int main() {
    PrFontSet* set = prFontSetCreate(nullptr);
    const PrFont* PR_NULLABLE face = nullptr;
    std::string path = std::string(PR_FONTS_DIR) + "/Arimo-Regular.ttf";
    if (prFontCreateFromFile(path, 0, nullptr, &face) != PrStatus_Ok) {
        fprintf(stderr, "text_yoga_check: cannot open %s\n", path.c_str());
        return 1;
    }
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    prFontSetAdd(set, &descriptor, face);
    prFontRelease(face);
    prFontSetSetDefaultFamily(set, prFontFamilyFromName("Arimo"));
    PrTextFontDesc fontDesc{.set = set, .style = prFontStyleDefault()};
    PrTextFont* font = prTextFontCreate(&fontDesc);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextShapingScratch* scratch = prTextShapingScratchCreate();

    TextNode texts[2] = {
        {.text = "Yoga decides how wide this is, and Prax text decides where its lines break."},
        {.text = "Short."},
    };
    YGNodeRef root = YGNodeNew();
    YGNodeStyleSetFlexDirection(root, YGFlexDirectionRow);
    YGNodeStyleSetWidth(root, 300);
    for (uint32_t i = 0; i < 2; ++i) {
        TextNode& t = texts[i];
        PrTextPiece piece{.kind = PrTextPieceKind_Text, .end = uint32_t(t.text.size()), .font = font};
        PrTextShapingDesc desc{.text = t.text, .pieces = &piece, .pieceCount = 1, .shaper = shaper};
        t.shaping = prTextShapingCreate();
        prTextShapingSet(t.shaping, &desc, scratch);
        YGNodeRef node = YGNodeNew();
        YGNodeSetContext(node, &t);
        YGNodeSetMeasureFunc(node, measure);
        YGNodeStyleSetFlexShrink(node, i == 0 ? 1 : 0);
        YGNodeInsertChild(root, node, i);
    }
    YGNodeCalculateLayout(root, YGUndefined, YGUndefined, YGDirectionLTR);

    YGNodeConstRef wrapped = YGNodeGetChild(root, 0);
    YGNodeConstRef shortText = YGNodeGetChild(root, 1);
    uint32_t shortSize = uint32_t(texts[1].text.size());
    PrTextIntrinsic shortSizes = prTextShapingIntrinsic(texts[1].shaping, 0, shortSize, nullptr, nullptr);
    check(YGNodeLayoutGetWidth(root) == 300, "the row keeps its width");
    check(YGNodeLayoutGetWidth(shortText) >= shortSizes.maxContent, "the text that does not shrink has its whole width");
    // Yoga rounds the width of a measured node up to a whole pixel, so the two together may pass the row by less than
    // 2.
    check(YGNodeLayoutGetWidth(wrapped) + YGNodeLayoutGetWidth(shortText) < 302, "the text that shrinks makes room");
    check(YGNodeLayoutGetHeight(wrapped) > kLineHeight, "the text that shrinks wraps to more than one line");
    check(YGNodeLayoutGetHeight(root) == YGNodeLayoutGetHeight(wrapped), "the row is as tall as its tallest text");
    check(texts[0].measures >= 2, "Yoga measures the text at more than one width");

    YGNodeFreeRecursive(root);
    for (TextNode& t : texts)
        prTextShapingDestroy(t.shaping);
    prTextShapingScratchDestroy(scratch);
    prTextShaperDestroy(shaper);
    prTextFontRelease(font);
    prFontSetDestroy(set);
    return failures == 0 ? 0 : 1;
}
