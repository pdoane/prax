// Every tree-sitter language that the backend builds in.

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
    const PrSyntaxLanguage* const languages[] = {
        prSyntaxTreeSitterBash(),
        prSyntaxTreeSitterC(),
        prSyntaxTreeSitterCSharp(),
        prSyntaxTreeSitterCMake(),
        prSyntaxTreeSitterCpp(),
        prSyntaxTreeSitterCss(),
        prSyntaxTreeSitterDart(),
        prSyntaxTreeSitterDiff(),
        prSyntaxTreeSitterDockerfile(),
        prSyntaxTreeSitterGlsl(),
        prSyntaxTreeSitterGo(),
        prSyntaxTreeSitterHcl(),
        prSyntaxTreeSitterHlsl(),
        prSyntaxTreeSitterHtml(),
        prSyntaxTreeSitterJava(),
        prSyntaxTreeSitterJavaScript(),
        prSyntaxTreeSitterJson(),
        prSyntaxTreeSitterKotlin(),
        prSyntaxTreeSitterLua(),
        prSyntaxTreeSitterMake(),
        prSyntaxTreeSitterMarkdown(),
        prSyntaxTreeSitterMarkdownInline(),
        prSyntaxTreeSitterObjC(),
        prSyntaxTreeSitterPhp(),
        prSyntaxTreeSitterPowerShell(),
        prSyntaxTreeSitterPython(),
        prSyntaxTreeSitterRuby(),
        prSyntaxTreeSitterRust(),
        prSyntaxTreeSitterSlang(),
        prSyntaxTreeSitterSql(),
        prSyntaxTreeSitterSwift(),
        prSyntaxTreeSitterTerraform(),
        prSyntaxTreeSitterToml(),
        prSyntaxTreeSitterTsx(),
        prSyntaxTreeSitterTypeScript(),
        prSyntaxTreeSitterWgsl(),
        prSyntaxTreeSitterXml(),
        prSyntaxTreeSitterYaml(),
    };
    for (const PrSyntaxLanguage* language : languages)
        prSyntaxRegistryAdd(registry, language);
}
