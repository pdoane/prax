#include "pr/text_editor.h"

int prTestTextEditorCApi(void);

// The public headers compiled as C. The default options wrap and number lines.
int prTestTextEditorCApi(void) {
    PrTextEditorOptions options = prTextEditorOptionsDefault();
    prTextEditorDestroy(NULL);
    prTextEditorDocumentDestroy(NULL);
    return options.wrap && options.lineNumbers ? 0 : 1;
}
