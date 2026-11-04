#include "pr/tex.h"

int prTestTexCApi(void);

// The public headers compile as C. A letter is an <mi> in a <math>, and a document holds a definition.
int prTestTexCApi(void) {
    PrTexOptions options = prTexOptionsDefault();
    options.annotate = false;
    PrRawString markup = prRawStringEmpty();
    prTexAppendMathMl(PR_SV("x"), &options, &markup);
    int result = prRawStringView(&markup).size != 40;
    prRawStringReset(&markup);
    PrTexDocument* document = prTexDocumentCreate();
    prTexDocumentDefineMacro(document, PR_SV("R"), 0, PR_SV("\\mathbb{R}"));
    result |= prTexDocumentMacroCount(document) != 1;
    result |= prTexDocumentLabel(document, PR_SV("x")).data != NULL;
    prTexDocumentDestroy(document);
    return result;
}
