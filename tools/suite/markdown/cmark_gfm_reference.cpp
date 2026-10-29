// The cmark-gfm harness, which tools/suite/markdown/markdown_compare.py drives.
//
// The single field of a case names the extensions that it parses, as cmark-gfm names them, or `commonmark` for none.
// The response is the HTML that cmark-gfm writes for the case, with raw HTML kept as lib/markdown keeps it.

#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/string_ops.h"

#include "tools/harness/harness.h"

#include "cmark-gfm-core-extensions.h"
#include "cmark-gfm.h"

using namespace pr;
using namespace harness;

namespace {

// Attaches the extensions that `field` names to `parser`, if it is not null, and adds the options that they need to
// `*options`. Returns false if the field names one that cmark-gfm lacks.
bool attachExtensions(cmark_parser* PR_NULLABLE parser, PrStringView field, int* options) {
    if (field == "commonmark")
        return true;
    size_t start = 0;
    while (start <= field.size) {
        size_t end = start;
        while (end < field.size && field.data[end] != ',')
            ++end;
        PrStringView name = stringSlice(field, start, end - start);
        start = end + 1;
        if (name == "footnotes") {
            *options |= CMARK_OPT_FOOTNOTES;
            continue;
        }
        if (name == "smart") {
            *options |= CMARK_OPT_SMART;
            continue;
        }
        char text[32] = {};
        if (name.size >= sizeof(text))
            return false;
        memCopy(text, name.data, name.size);
        cmark_syntax_extension* extension = cmark_find_syntax_extension(text);
        if (extension == nullptr)
            return false;
        if (parser != nullptr)
            cmark_parser_attach_syntax_extension(parser, extension);
    }
    return true;
}

} // namespace

int main() {
    prCrashHandler();
    cmark_gfm_core_extensions_ensure_registered();
    HarnessCase test = {};
    while (harnessReadCase(&test)) {
        if (test.fieldCount != 1)
            return 1;
        // The extensions' options are read first, since a parser takes its options when it is made.
        int options = CMARK_OPT_UNSAFE;
        if (!attachExtensions(nullptr, test.fields[0], &options))
            return 1;
        cmark_parser* parser = cmark_parser_new(options);
        attachExtensions(parser, test.fields[0], &options);
        PrStringView source = textView(&test.input);
        cmark_parser_feed(parser, source.data != nullptr ? source.data : "", source.size);
        cmark_node* document = cmark_parser_finish(parser);
        char* html = cmark_render_html(document, options, cmark_parser_get_syntax_extensions(parser));

        TextBuffer out;
        textAppend(&out, "{\"html\":");
        harnessWriteString(&out, html);
        textAppendChar(&out, '}');
        harnessWriteLine(textView(&out));
        textReset(&out);
        cmark_get_default_mem_allocator()->free(html);
        cmark_node_free(document);
        cmark_parser_free(parser);
    }
    harnessCaseReset(&test);
    return 0;
}
