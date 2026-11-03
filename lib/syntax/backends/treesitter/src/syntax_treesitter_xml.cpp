// The tree-sitter language of XML: its grammar, and the queries that the build reads from its package.

#include "pr/syntax_treesitter_xml.h"

#include "pr/syntax_treesitter.h"

#include "syntax/backends/treesitter/src/syntax_queries_xml.h"

#include "tree-sitter-xml.h"

using namespace pr_impl;

const PrSyntaxLanguage* prSyntaxTreeSitterXml(void) {
    static const PrStringView kExtensions[] = {"xml", "svg", "xsd", "xslt", "xsl", "rng"};
    static const PrSyntaxLanguage* language = nullptr;
    if (language == nullptr) {
        PrSyntaxTreeSitterDesc desc{
            .name = "xml",
            .namePattern = "^(xml|svg|xsd|xslt|xsl|rng)$",
            .extensions = kExtensions,
            .extensionCount = 6,
            .grammar = tree_sitter_xml(),
            .highlights = kTreeSitterHighlights,
            .injections = kTreeSitterInjections,
        };
        language = prSyntaxTreeSitterLanguageCreate(&desc);
    }
    return language;
}
