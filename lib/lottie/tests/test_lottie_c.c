#include "pr/lottie.h"

int prTestLottieCApi(void);

// The public header compiles as C. An animation is read, described and drawn.
int prTestLottieCApi(void) {
    PrLottieAnimation* animation = NULL;
    PrStatus status = prLottieAnimationCreate(
        PR_SV("{\"w\": 20, \"h\": 10, \"fr\": 25, \"ip\": 0, \"op\": 50, \"layers\": []}"), NULL, &animation);
    if (status != PrStatus_Ok || animation == NULL)
        return 1;
    PrLottieInfo info = prLottieAnimationInfo(animation);
    int result = info.extent.x != 20 || info.extent.y != 10 || info.frameRate != 25 || info.outPoint != 50;
    PrDrawList* list = prDrawListCreate();
    prLottieAnimationDraw(animation, 10, list);
    result |= prDrawListDepth(list) != 0;
    prDrawListRelease(list);
    prLottieAnimationDestroy(animation);
    return result;
}
