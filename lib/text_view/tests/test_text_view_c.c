#include "pr/text_view.h"

int prTestTextViewCApi(void);

// The public header compiles as C, and a view reports its buffer's lines.
int prTestTextViewCApi(void) {
    PrTextEditBuffer* buffer = prTextEditBufferCreate(PR_SV("a\nb"));
    PrTextView* view = prTextViewCreate(buffer);
    uint32_t lines = prTextViewInfo(view).lineCount;
    prTextViewDestroy(view);
    prTextEditBufferDestroy(buffer);
    return lines == 2 ? 0 : 1;
}
