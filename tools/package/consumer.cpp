// A program over the libraries' public headers, as an application includes them: it makes and frees what it calls in
// the C API, on the backends that the package's build has, and keeps a function of the HTML engine's QuickJS layer, so
// that the link resolves that layer and everything that it links.

#include "pr/draw_list.h"
#include "pr/font_hinter.h"
#include "pr/html_quickjs.h"
#include "pr/text.h"

#include <stdio.h>

using namespace pr;

int main() {
    PrDrawList* list = prDrawListCreate();
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    PrFontHinter* hinter = prFontHinterCreate(PrFontHinterBackend_Default);
    PrJsHtmlEngine* (*volatile create)(const PrJsHtmlEngineDesc*) = &prJsHtmlEngineCreate;
    bool ok = shaper != nullptr && create != nullptr;
    prFontHinterDestroy(hinter);
    prTextShaperDestroy(shaper);
    prDrawListRelease(list);
    printf("consumer: %s\n", ok ? "ok" : shaper == nullptr ? "no shaper" : "no HTML engine");
    return ok ? 0 : 1;
}
