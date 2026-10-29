#include "pr/draw_cpu.h"

int prTestDrawCpuCApi(void);

// Tests that the public header compiles as C. A list is rendered into the caller's pixels.
int prTestDrawCpuCApi(void) {
    uint8_t bytes[4] = {0};
    PrPixels pixels = {bytes, 4, {1, 1}};
    PrDrawRenderer* renderer = prDrawCpuCreate(NULL);
    PrDrawList* list = prDrawListCreate();
    PrColor16 white = PR_COLOR16(1, 1, 1, 1);
    PrRect all = {0, 0, 1, 1};
    prDrawListClearRect(list, all, white);
    PrStatus status = prDrawRendererRenderPixels(renderer, list, pixels, PrPixelFormat_Rgba8);
    int result = status != PrStatus_Ok || bytes[3] != 255;
    prDrawListRelease(list);
    prDrawRendererDestroy(renderer);
    return result;
}
