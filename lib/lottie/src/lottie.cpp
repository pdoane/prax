// Lottie animations, drawn into draw lists.
//
// lottie-spec - https://lottie.github.io/lottie-spec/

#include "pr/lottie.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/float.h"

#include "lottie/src/lottie_eval.h"
#include "lottie/src/lottie_model.h"
#include "lottie/src/lottie_parse.h"
#include "lottie/src/lottie_render.h"

using namespace pr_impl;

PrLottieOptions prLottieOptionsDefault(void) {
    return PrLottieOptions{};
}

PrStatus prLottieAnimationCreate(PrStringView json, const PrLottieOptions* PR_NULLABLE options,
                                 PrLottieAnimation* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrLottieAnimation* a = allocZeroed<PrLottieAnimation>();
    PrStatus status = lottieParse(json, options != nullptr ? *options : prLottieOptionsDefault(), *a);
    if (status != PrStatus_Ok) {
        lottieReset(*a);
        prMemoryFree(a);
        return status;
    }
    *out = a;
    return PrStatus_Ok;
}

void prLottieAnimationDestroy(PrLottieAnimation* PR_NULLABLE animation) {
    if (animation == nullptr)
        return;
    lottieReset(*animation);
    prMemoryFree(animation);
}

PrLottieInfo prLottieAnimationInfo(const PrLottieAnimation* animation) {
    return animation->info;
}

double prLottieAnimationFrameAt(const PrLottieAnimation* animation, double seconds) {
    const PrLottieInfo& info = animation->info;
    double length = info.outPoint - info.inPoint;
    double frames = seconds * info.frameRate;
    if (!(length > 0))
        return info.inPoint;
    return info.inPoint + fmodF64(frames, length);
}

void prLottieAnimationDraw(const PrLottieAnimation* animation, double frame, PrDrawList* list) {
    LottieRender r{};
    lottieRenderInit(r, *animation, list);
    prDrawListPushClipRect(list, PrRect{0, 0, animation->info.extent.x, animation->info.extent.y},
                           PrDrawEdges_Antialiased);
    lottieDrawComp(r, 0, frame, animation->info.extent);
    prDrawListPopClip(list);
    lottieRenderReset(r);
}

