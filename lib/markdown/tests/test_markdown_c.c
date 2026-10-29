#include "pr/markdown.h"
#include "pr/markdown_html.h"

int prTestMarkdownCApi(void);

// The public headers compile as C. A heading parses into one block, whose HTML is the heading.
int prTestMarkdownCApi(void) {
    PrMarkdownOptions options = prMarkdownOptionsDefault();
    options.extensions = PrMarkdownExtensionFlag_Gfm;
    PrMarkdown* document = prMarkdownCreate(&options);
    prMarkdownParse(document, PR_SV("# Hi"));
    int result = prMarkdownBlockCount(document) != 1;
    PrString html = prStringEmpty();
    prMarkdownAppendHtml(document, 0, 1, &html);
    result |= prStringView(&html).size != 12;
    prStringReset(&html);
    prMarkdownDestroy(document);
    return result;
}
