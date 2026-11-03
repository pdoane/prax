#include "pr/syntax_treesitter.h"
#include "pr/syntax_treesitter_c.h"
#include "pr/syntax_treesitter_cpp.h"
#include "pr/syntax_treesitter_css.h"
#include "pr/syntax_treesitter_html.h"
#include "pr/syntax_treesitter_javascript.h"
#include "pr/syntax_treesitter_json.h"
#include "pr/syntax_treesitter_markdown.h"
#include "pr/syntax_treesitter_markdown_inline.h"
#include "pr/syntax_treesitter_python.h"
#include "pr/syntax_treesitter_xml.h"

int prTestSyntaxTreeSitterCApi(void);

// The public header compiles as C, and a built-in language has its name.
int prTestSyntaxTreeSitterCApi(void) {
    const PrSyntaxLanguage* language = prSyntaxTreeSitterJson();
    return language->name.size == 4 ? 0 : 1;
}
