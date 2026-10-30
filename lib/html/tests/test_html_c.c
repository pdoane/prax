// Checks that the native embedding API and IDL projections compile and link without a script engine.

#include "pr/html_forms.h"
#include "pr/html_page.h"
#include "pr/html_view.h"

PrStatus prTestHtmlCApi(PrStringView source);

static PrStatus runScript(void* context, PrHtmlPage* page, const PrHtmlScript* script) {
    (void)context;
    (void)script;
    return prHtmlDocumentWrite(prHtmlWindowDocument(prHtmlPageWindow(page)), PR_SV("<p>native script</p>"));
}

PrStatus prTestHtmlCApi(PrStringView source) {
    PrHtmlHost scripting = prHtmlHostDefault();
    scripting.execute = runScript;
    PrHtmlPage* page = prHtmlPageCreate(&scripting);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    PrDomDocument* document = prHtmlWindowDocument(window);
    PrDomText* text = prDomTextCreate(document, PR_SV("text"));
    PrDomCharacterData* data = prDomTextAsCharacterData(text);
    PrDomNode* node = prDomTextAsNode(text);
    PrDomEventTarget* target = prDomTextAsEventTarget(text);

    (void)prDomCharacterDataData(data);
    (void)prDomNodeType(node);
    (void)prDomTextQuery(target);
    (void)prDomCharacterDataQuery(target);
    (void)prDomNodeQuery(target);
    (void)prDomDocumentQuery(target);
    (void)prDomDocumentTypeQuery(target);
    (void)prDomDocumentFragmentQuery(target);
    (void)prDomElementQuery(target);
    (void)prDomCommentQuery(target);
    (void)prDomProcessingInstructionQuery(target);
    (void)prHtmlElementQuery(target);
    (void)prHtmlStyleElementQuery(target);
    (void)prHtmlWindowQuery(prHtmlWindowAsEventTarget(window));

    (void)prDomCommentCreate(document, PR_SV("comment"));
    (void)prDomDocumentFragmentCreate(document);
    PrDomProcessingInstruction* instruction = NULL;
    (void)prDomProcessingInstructionCreate(document, PR_SV("target"), PR_SV("data"), &instruction);

    PrHtmlResponseHeaders headers = {PR_SV("text/html"), PR_SV("en")};
    PrStatus status = prHtmlPageLoad(page, source, PR_SV("about:blank"), &headers);
    prHtmlPageDestroy(page);
    return status;
}
