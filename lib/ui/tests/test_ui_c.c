#include "pr/ui.h"

int prTestUiCApi(void);

// The public header compiles as C, and a context runs a frame of an empty tree.
static void buildEmpty(void* context, PrUi* ui, const PrUiRound* round) {
    (void)context;
    (void)round;
    PrLayoutTree* tree = prUiTree(ui);
    PrLayoutStyle style = prLayoutStyleDefault();
    PrLayoutNode root = prLayoutTreeFrameElement(tree, kPrLayoutNodeNone, 0, prLayoutStyleIntern(tree, &style));
    prUiSetFlags(ui, root, PrUiFlag_Pressable);
}

int prTestUiCApi(void) {
    PrLayoutTreeDesc desc = {.targetPixelRatio = 1};
    PrLayoutTree* tree = prLayoutTreeCreate(&desc);
    PrUi* ui = prUiCreate(tree);
    PrUiFrameDesc frame = {.time = 1, .extent = {100, 100}, .maxRounds = 1, .display = prAppDisplayDefault()};
    uint32_t rounds = prUiFrame(ui, &frame, buildEmpty, NULL);
    int result = rounds != 1 || prUiWantsFrame(ui);
    prUiDestroy(ui);
    prLayoutTreeDestroy(tree);
    return result;
}
