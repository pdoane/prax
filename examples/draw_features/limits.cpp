// Limits: drawings that reach past what a renderer holds at full resolution, which it draws at a lower one.

#include "examples/draw_features/limits.h"

#include "pr/draw_filter.h"
#include "pr/draw_list.h"

namespace drawings {

using namespace pr;

namespace {

#if 0
// Off until its cost is fit for the gallery: it takes most of a second to draw on every renderer. A group whose blur
// reaches far past the drawing, so that its layer draws at a lower resolution, in bounded memory. The squares spread to
// almost nothing.
void farBlur(PrDrawList* list, float width, float height, double) {
    const float sigmas[] = {2000, 100000};
    for (uint32_t i = 0; i < 2; ++i) {
        PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
        PrDrawFilterNode blur{};
        blur.kind = PrDrawFilterKind_Blur;
        blur.inputs[0] = PrDrawFilterInput_Source;
        blur.blur.sigma = PrVec2{sigmas[i], sigmas[i]};
        prDrawFilterBuilderAdd(builder, &blur);
        const PrDrawFilter* filter = prDrawFilterCreate(builder);
        prDrawFilterBuilderDestroy(builder);
        PrDrawGroup group = prDrawGroupDefault();
        group.filter = filter;
        prDrawListPushGroup(list, &group);
        float left = width * (0.15f + 0.5f * float(i));
        prDrawListRect(list, PrRect{left, height * 0.3f, left + width * 0.2f, height * 0.7f}, {.color = PrColor{0, 0, 0, 1}});
        prDrawListPopGroup(list);
        prDrawFilterRelease(filter);
    }
}
#endif

// An image of a draw list whose tile is larger than the largest texture, which draws at the largest size.
void hugeListSource(PrDrawList* list, float width, float height, double) {
    PrDrawList* drawn = prDrawListCreate();
    prDrawListRect(drawn, PrRect{0, 0, 1, 1}, {.color = PrColor{0.85f, 0.25f, 0.2f, 1}});
    prDrawListRect(drawn, PrRect{0.25f, 0.25f, 0.75f, 0.75f}, {.color = PrColor{0.2f, 0.45f, 0.85f, 1}});
    PrDrawSource source = prDrawSourceList(drawn, PrRect{0, 0, 1, 1});
    PrDrawBrush brush = prDrawBrushImage(source, PrRect{0, 0, 20000, 20000}, PrDrawSampling_Linear);
    prDrawListRect(list, PrRect{0, 0, width, height}, {.brush = &brush});
    prDrawListRelease(drawn);
}

} // namespace

const Array<Drawing>& limitDrawings() {
    static const Array<Drawing> limits({
#if 0
        {"Far blur", "Blurs that reach thousands and a hundred thousand pixels, drawn at a lower resolution.", farBlur},
#endif
        {
            "Huge list image", "An image of a draw list 20,000 pixels on a side, drawn at the largest texture.",
            hugeListSource,
        },
    });
    return limits;
}

} // namespace drawings
