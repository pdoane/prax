#include "pr/canvas.h"

int prTestCanvasCApi(void);

// The public header compiled as C. A path is built and hit tested without a context.
int prTestCanvasCApi(void) {
    PrCanvasPath2D* path = prCanvasPath2DCreate();
    prCanvasPath2DRect(path, 0, 0, 10, 10);
    PrDVec2 radius = {2, 2};
    PrStatus status = prCanvasPath2DRoundRect(path, 20, 0, 10, 10, &radius, 1);
    PrCanvasSettings settings = prCanvasSettingsDefault();
    int result = status != PrStatus_Ok || !settings.alpha || settings.colorType != PrCanvasColorType_Unorm8;
    result |= prCanvasPixelFormatSize(PrCanvasPixelFormat_RgbaFloat16) != 8;
    prCanvasPath2DDestroy(path);
    return result;
}
