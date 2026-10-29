#include "pr/text_simple.h"

int prTestTextSimpleCApi(void);

// The public headers compile as C. A shaper is made and freed.
int prTestTextSimpleCApi(void) {
    PrTextShaper* shaper = prTextSimpleCreate();
    int result = shaper == NULL;
    prTextShaperDestroy(shaper);
    return result;
}
