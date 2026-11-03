#include "pr/text_edit.h"

int prTestTextEditCApi(void);

// The public header compiles as C, and a view inserts at its caret.
int prTestTextEditCApi(void) {
    PrTextEditBuffer* buffer = prTextEditBufferCreate(PR_SV("ab"));
    PrTextEditView* view = prTextEditViewCreate(buffer);
    bool applied = prTextEditViewInsert(view, PR_SV("x"), PrTextEditOrigin_Local, PrTextEditMerge_None);
    uint64_t size = prTextEditBufferSize(buffer);
    prTextEditViewDestroy(view);
    prTextEditBufferDestroy(buffer);
    return applied && size == 3 ? 0 : 1;
}
