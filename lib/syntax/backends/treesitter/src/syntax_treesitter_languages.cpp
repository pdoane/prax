// Every tree-sitter language that the backend builds in (PR_SYNTAX_LANGUAGES), each as its grammar defines it
// (PR_SYNTAX_HAS_<LANGUAGE>).

#include "pr/syntax_treesitter_languages.h"

#include "pr/syntax_treesitter_bash.h"
#include "pr/syntax_treesitter_c.h"
#include "pr/syntax_treesitter_c_sharp.h"
#include "pr/syntax_treesitter_cmake.h"
#include "pr/syntax_treesitter_cpp.h"
#include "pr/syntax_treesitter_css.h"
#include "pr/syntax_treesitter_dart.h"
#include "pr/syntax_treesitter_diff.h"
#include "pr/syntax_treesitter_dockerfile.h"
#include "pr/syntax_treesitter_glsl.h"
#include "pr/syntax_treesitter_go.h"
#include "pr/syntax_treesitter_hcl.h"
#include "pr/syntax_treesitter_hlsl.h"
#include "pr/syntax_treesitter_html.h"
#include "pr/syntax_treesitter_java.h"
#include "pr/syntax_treesitter_javascript.h"
#include "pr/syntax_treesitter_json.h"
#include "pr/syntax_treesitter_kotlin.h"
#include "pr/syntax_treesitter_lua.h"
#include "pr/syntax_treesitter_make.h"
#include "pr/syntax_treesitter_markdown.h"
#include "pr/syntax_treesitter_markdown_inline.h"
#include "pr/syntax_treesitter_objc.h"
#include "pr/syntax_treesitter_php.h"
#include "pr/syntax_treesitter_powershell.h"
#include "pr/syntax_treesitter_python.h"
#include "pr/syntax_treesitter_ruby.h"
#include "pr/syntax_treesitter_rust.h"
#include "pr/syntax_treesitter_slang.h"
#include "pr/syntax_treesitter_sql.h"
#include "pr/syntax_treesitter_swift.h"
#include "pr/syntax_treesitter_terraform.h"
#include "pr/syntax_treesitter_toml.h"
#include "pr/syntax_treesitter_tsx.h"
#include "pr/syntax_treesitter_typescript.h"
#include "pr/syntax_treesitter_wgsl.h"
#include "pr/syntax_treesitter_xml.h"
#include "pr/syntax_treesitter_yaml.h"

using namespace pr_impl;

void prSyntaxTreeSitterAddLanguages(PrSyntaxRegistry* registry) {
#if defined(PR_SYNTAX_HAS_BASH)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterBash());
#endif
#if defined(PR_SYNTAX_HAS_C)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterC());
#endif
#if defined(PR_SYNTAX_HAS_C_SHARP)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterCSharp());
#endif
#if defined(PR_SYNTAX_HAS_CMAKE)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterCMake());
#endif
#if defined(PR_SYNTAX_HAS_CPP)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterCpp());
#endif
#if defined(PR_SYNTAX_HAS_CSS)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterCss());
#endif
#if defined(PR_SYNTAX_HAS_DART)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterDart());
#endif
#if defined(PR_SYNTAX_HAS_DIFF)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterDiff());
#endif
#if defined(PR_SYNTAX_HAS_DOCKERFILE)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterDockerfile());
#endif
#if defined(PR_SYNTAX_HAS_GLSL)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterGlsl());
#endif
#if defined(PR_SYNTAX_HAS_GO)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterGo());
#endif
#if defined(PR_SYNTAX_HAS_HCL)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterHcl());
#endif
#if defined(PR_SYNTAX_HAS_HLSL)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterHlsl());
#endif
#if defined(PR_SYNTAX_HAS_HTML)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterHtml());
#endif
#if defined(PR_SYNTAX_HAS_JAVA)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterJava());
#endif
#if defined(PR_SYNTAX_HAS_JAVASCRIPT)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterJavaScript());
#endif
#if defined(PR_SYNTAX_HAS_JSON)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterJson());
#endif
#if defined(PR_SYNTAX_HAS_KOTLIN)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterKotlin());
#endif
#if defined(PR_SYNTAX_HAS_LUA)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterLua());
#endif
#if defined(PR_SYNTAX_HAS_MAKE)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterMake());
#endif
#if defined(PR_SYNTAX_HAS_MARKDOWN)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterMarkdown());
#endif
#if defined(PR_SYNTAX_HAS_MARKDOWN_INLINE)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterMarkdownInline());
#endif
#if defined(PR_SYNTAX_HAS_OBJC)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterObjC());
#endif
#if defined(PR_SYNTAX_HAS_PHP)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterPhp());
#endif
#if defined(PR_SYNTAX_HAS_POWERSHELL)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterPowerShell());
#endif
#if defined(PR_SYNTAX_HAS_PYTHON)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterPython());
#endif
#if defined(PR_SYNTAX_HAS_RUBY)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterRuby());
#endif
#if defined(PR_SYNTAX_HAS_RUST)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterRust());
#endif
#if defined(PR_SYNTAX_HAS_SLANG)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterSlang());
#endif
#if defined(PR_SYNTAX_HAS_SQL)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterSql());
#endif
#if defined(PR_SYNTAX_HAS_SWIFT)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterSwift());
#endif
#if defined(PR_SYNTAX_HAS_TERRAFORM)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterTerraform());
#endif
#if defined(PR_SYNTAX_HAS_TOML)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterToml());
#endif
#if defined(PR_SYNTAX_HAS_TSX)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterTsx());
#endif
#if defined(PR_SYNTAX_HAS_TYPESCRIPT)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterTypeScript());
#endif
#if defined(PR_SYNTAX_HAS_WGSL)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterWgsl());
#endif
#if defined(PR_SYNTAX_HAS_XML)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterXml());
#endif
#if defined(PR_SYNTAX_HAS_YAML)
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterYaml());
#endif
}
