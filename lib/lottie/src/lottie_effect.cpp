// A layer's effects, as a filter graph on its content.
//
// lottie-spec - https://lottie.github.io/lottie-spec/

#include "lottie/src/lottie_effect.h"

#include "pr/draw_filter.h"
#include "pr/float.h"
#include "pr/vec.h"

#include "lottie/src/lottie_eval.h"
#include "lottie/src/lottie_render.h"

namespace pr_impl {

namespace {

// Writes the effect's value `index` at `frame` into `out`, up to four floats. Writes zeros if the effect has no such
// value.
void valueOf(const LottieAnimation& a, const LottieEffect& effect, uint32_t index, double frame, float* out) {
    out[0] = out[1] = out[2] = out[3] = 0;
    if (index >= effect.count)
        return;
    uint32_t property = a.effectValues.data[effect.values + index];
    uint32_t dims = lottieDims(a, property);
    if (dims == 0 || dims > 4)
        return;
    lottieEvaluate(a, property, frame, out);
}

float scalarOf(const LottieAnimation& a, const LottieEffect& effect, uint32_t index, double frame) {
    float v[4];
    valueOf(a, effect, index, frame, v);
    return v[0];
}

// Returns a color matrix node with the given rows for R, G, B and A. The rows apply to straight R, G, B, A and 1.
PrDrawFilterNode matrixNode(PrDrawFilterInput input, PrColorSpace space, const float (&rows)[20]) {
    PrDrawFilterNode node{};
    node.kind = PrDrawFilterKind_ColorMatrix;
    node.space = space;
    node.inputs[0] = input;
    node.inputs[1] = input;
    for (uint32_t i = 0; i < 20; ++i)
        node.matrix.m[i / 5][i % 5] = rows[i];
    return node;
}

// Returns the gray that tint and tritone map from: the average of the channels in linear light, with alpha multiplied
// by `alpha`.
PrDrawFilterNode grayNode(PrDrawFilterInput input, float alpha) {
    constexpr float kThird = 0.3333f;
    const float rows[20] = {
        kThird, kThird, kThird, 0, 0, kThird, kThird, kThird, 0, 0,
        kThird, kThird, kThird, 0, 0, 0,      0,      0,      alpha, 0,
    };
    return matrixNode(input, PrColorSpace_SrgbLinear, rows);
}

} // namespace

const PrDrawFilter* PR_NULLABLE lottieLayerFilter(const LottieRender& r, const LottieLayer& layer, double frame) {
    const LottieAnimation& a = *r.a;
    if (layer.effectCount == 0)
        return nullptr;
    PrDrawFilterBuilder* builder = prDrawFilterBuilderCreate();
    PrDrawFilterInput source = PrDrawFilterInput_Source;
    uint32_t added = 0;
    for (uint32_t i = 0; i < layer.effectCount; ++i) {
        const LottieEffect& effect = a.effects.data[layer.effects + i];
        if (!effect.enabled)
            continue;
        PrDrawFilterInput result = 0;
        bool supported = true;
        switch (effect.type) {
            case LottieEffectType_Tint: {
                // Tint maps the gray from black to white, and blends the result over the source by the amount.
                float black[4];
                float white[4];
                valueOf(a, effect, 0, frame, black);
                valueOf(a, effect, 1, frame, white);
                float amount = scalarOf(a, effect, 2, frame) / 100;
                PrDrawFilterNode gray = grayNode(source, amount);
                PrDrawFilterInput grayed = prDrawFilterBuilderAdd(builder, &gray);
                const float rows[20] = {
                    white[0] - black[0], 0, 0, 0, black[0], white[1] - black[1], 0, 0, 0, black[1],
                    white[2] - black[2], 0, 0, 0, black[2], 0, 0, 0, 1, 0,
                };
                PrDrawFilterNode mapped = matrixNode(grayed, PrColorSpace_Srgb, rows);
                PrDrawFilterInput tinted = prDrawFilterBuilderAdd(builder, &mapped);
                PrDrawFilterInput inputs[3] = {source, grayed, tinted};
                PrDrawFilterNode merge{};
                merge.kind = PrDrawFilterKind_Merge;
                merge.space = PrColorSpace_Srgb;
                merge.inputs[0] = source;
                merge.inputs[1] = source;
                merge.merge.inputs = inputs;
                merge.merge.inputCount = 3;
                result = prDrawFilterBuilderAdd(builder, &merge);
                break;
            }
            case LottieEffectType_Fill: {
                // Fill sets every pixel to the color, and multiplies the pixel's alpha by the opacity.
                float color[4];
                valueOf(a, effect, 2, frame, color);
                float opacity = scalarOf(a, effect, 6, frame);
                const float rows[20] = {
                    0, 0, 0, 0, color[0], 0, 0, 0, 0, color[1],
                    0, 0, 0, 0, color[2], 0, 0, 0, opacity, 0,
                };
                PrDrawFilterNode fill = matrixNode(source, PrColorSpace_Srgb, rows);
                result = prDrawFilterBuilderAdd(builder, &fill);
                break;
            }
            case LottieEffectType_Tritone: {
                // Tritone maps the gray through shadows, midtones and highlights.
                float bright[4];
                float mid[4];
                float dark[4];
                valueOf(a, effect, 0, frame, bright);
                valueOf(a, effect, 1, frame, mid);
                valueOf(a, effect, 2, frame, dark);
                PrDrawFilterNode gray = grayNode(source, 1);
                PrDrawFilterInput grayed = prDrawFilterBuilderAdd(builder, &gray);
                float tables[3][3] = {
                    {dark[0], mid[0], bright[0]}, {dark[1], mid[1], bright[1]}, {dark[2], mid[2], bright[2]},
                };
                PrDrawFilterNode transfer{};
                transfer.kind = PrDrawFilterKind_ComponentTransfer;
                transfer.space = PrColorSpace_Srgb;
                transfer.inputs[0] = grayed;
                transfer.inputs[1] = grayed;
                for (uint32_t c = 0; c < 3; ++c) {
                    transfer.transfer[c].kind = PrDrawTransferKind_Table;
                    transfer.transfer[c].values = tables[c];
                    transfer.transfer[c].valueCount = 3;
                }
                transfer.transfer[3].kind = PrDrawTransferKind_Identity;
                result = prDrawFilterBuilderAdd(builder, &transfer);
                break;
            }
            case LottieEffectType_DropShadow: {
                // Drop shadow blurs the alpha by a quarter of the softness and moves it by the distance in the
                // direction, which is measured clockwise from up. The shadow is drawn in the color, under the source.
                float color[4];
                valueOf(a, effect, 0, frame, color);
                float opacity = scalarOf(a, effect, 1, frame) / 255;
                float angle = (scalarOf(a, effect, 2, frame) - 90) * kPiF32 / 180;
                float distance = scalarOf(a, effect, 3, frame);
                float sigma = scalarOf(a, effect, 4, frame) / 4;
                PrDrawFilterNode shadow{};
                shadow.kind = PrDrawFilterKind_DropShadow;
                shadow.space = PrColorSpace_Srgb;
                shadow.inputs[0] = source;
                shadow.inputs[1] = source;
                shadow.shadow.offset = PrVec2{distance * cosF32(angle), distance * sinF32(angle)};
                shadow.shadow.sigma = PrVec2{sigma, sigma};
                shadow.shadow.color = PrColor{
                    roundF32(color[0] * 255) / 255, roundF32(color[1] * 255) / 255,
                    roundF32(color[2] * 255) / 255, clampF32(opacity, 0, 1),
                };
                result = prDrawFilterBuilderAdd(builder, &shadow);
                break;
            }
            case LottieEffectType_GaussianBlur: {
                // Gaussian blur uses a sigma of three tenths of the blurriness, in both dimensions or in one.
                float sigma = scalarOf(a, effect, 0, frame) * 0.3f;
                float dimensions = scalarOf(a, effect, 1, frame);
                bool wrap = scalarOf(a, effect, 2, frame) == 1;
                PrDrawFilterNode blur{};
                blur.kind = PrDrawFilterKind_Blur;
                blur.space = PrColorSpace_Srgb;
                blur.inputs[0] = source;
                blur.inputs[1] = source;
                blur.blur.sigma = PrVec2{dimensions == 3 ? 0 : sigma, dimensions == 2 ? 0 : sigma};
                blur.blur.edge = wrap ? PrImageEdgeMode_Wrap : PrImageEdgeMode_Duplicate;
                result = prDrawFilterBuilderAdd(builder, &blur);
                break;
            }
            default:
                supported = false;
                break;
        }
        if (supported) {
            source = result;
            ++added;
        }
    }
    const PrDrawFilter* filter = nullptr;
    if (added > 0)
        filter = prDrawFilterCreate(builder);
    prDrawFilterBuilderDestroy(builder);
    return filter;
}

} // namespace pr_impl
