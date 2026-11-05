#include "pr/draw_commands.h"
#include "pr/draw_list.h"
#include "pr/draw_projection.h"
#include "pr/draw_renderer.h"
#include "pr/draw_structure.h"
#include "pr/draw_writer.h"

int prTestDrawCApi(void);

// The public headers compile as C. A list is recorded and read back.
int prTestDrawCApi(void) {
    PrDrawList* list = prDrawListCreate();
    PrRect rect = {0, 0, 1, 1};
    PrColor16 red = PR_COLOR16(1, 0, 0, 1);
    PrDrawStroke stroke = {
        .shape = {
            .width = 1,
            .miterLimit = 4,
            .join = PrPathJoin_Round,
            .startCap = PrPathCap_Round,
            .endCap = PrPathCap_Round,
            .dashCap = PrPathCap_Round,
            .zeroLengthDirection = {1, 0},
        },
        .align = PrDrawStrokeAlign_Center,
        .mapping = PrDrawStrokeMapping_Plain,
    };
    prDrawListPushTransformAffine2(list, prAffine2Scale((PrVec2){2, 2}));
    prDrawListRect(list, rect, (PrDrawPaint){.color = red});
    prDrawListRect(list, rect, (PrDrawPaint){.color = red, .stroke = &stroke});
    prDrawListPopTransform(list);
    int result = 0;
    const PrDrawCommand* commands = prDrawListCommands(list);
    result |= prDrawListCommandCount(list) != 4 || commands[1].rect.right != 1;
    result |= commands[1].stroke != NULL || commands[2].stroke->shape.join != PrPathJoin_Round;
    PrDrawTarget target = {0};
    PrDrawBeneath beneath = {0};
    target.beneath = &beneath;
    result |= target.commands != NULL || beneath.kind != PrDrawBeneathKind_Color;
    prDrawListRelease(list);
    prDrawRendererDestroy(NULL);
    prDrawFrameDestroy(NULL);
    PrMatrix4 identity = prMatrix4Identity();
    PrDrawProjection projection = {0};
    result |= !prDrawProjectionPlace(&identity, prAffine2Identity(), rect, rect, 64, &projection);
    result |= projection.scale != 1;
    return result;
}
