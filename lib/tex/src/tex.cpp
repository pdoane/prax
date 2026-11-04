// TeX math: formulas in the TeX language of LaTeX, amsmath, MathJax, KaTeX and Temml, written as MathML Core.

#include "pr/tex.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/utf8.h"

#include "tex/src/tex_parse.h"
#include "tex/src/tex_spacing.h"

namespace pr_impl {

namespace {

// The local names of the elements, by TexElement.
const char* const kElementNames[] = {
    "math", "semantics", "annotation", "mrow", "mi", "mn", "mo", "mtext", "mspace", "msub", "msup", "msubsup",
    "munder", "mover", "munderover", "mmultiscripts", "mprescripts", "mfrac", "msqrt", "mroot", "mstyle", "merror",
    "mpadded", "mphantom", "menclose", "mtable", "mtr", "mtd", "mlabeledtr",
};
static_assert(countOf(kElementNames) == TexElement_Mlabeledtr + 1, "every element has a name");

// The maximum number of attributes of a node, counting the operator properties that the writer computes and the
// attributes that the parser sets.
constexpr uint32_t kMaxAttributes = 16;

bool isToken(TexElement element) {
    switch (element) {
        case TexElement_Mi:
        case TexElement_Mn:
        case TexElement_Mo:
        case TexElement_Mtext:
        case TexElement_Annotation:
            return true;
        default:
            return false;
    }
}

// Appends a space in mu as an attribute value in em, with at most four decimals, as the writer writes lengths.
void appendMu(PrTextBuffer* out, uint8_t mu) {
    static const char* const kSpaces[] = {"0em", "0.0556em", "0.1111em", "0.1667em", "0.2222em", "0.2778em"};
    prTextBufferAppend(out, mu < countOf(kSpaces) ? kSpaces[mu] : "0.3333em");
}

struct Writer {
    const TexTree* tree;
    PrTexEventFn fn;
    void* PR_NULLABLE context;
    PrTextBuffer values;            // the values that the writer computes for the node being written
};

// A computed attribute, whose value is a range of the writer's values.
struct Computed {
    PrStringView name;
    uint32_t start;
    uint32_t size;
};

void addComputed(Writer* w, Computed* computed, uint32_t* count, PrStringView name, PrStringView value) {
    PR_ASSERT(*count < kMaxAttributes);
    uint32_t start = w->values.size;
    prTextBufferAppend(&w->values, value);
    computed[(*count)++] = Computed{name, start, w->values.size - start};
}

bool hasFlag(TexCategoryValues values, TexOperatorFlags flag) {
    return (values.flags & flag) != 0;
}

// Adds the attributes of an operator where TeX's requirements differ from the operator dictionary's properties.
void addOperatorAttributes(Writer* w, const TexNode& node, PrStringView text, Computed* computed, uint32_t* count) {
    TexCategoryValues dictionary = texOperatorDictionary(text, node.form, false);
    if (node.flags & TexNodeFlag_Spaced) {
        if (node.lspace != dictionary.lspace) {
            PrTextBuffer value{};
            appendMu(&value, node.lspace);
            addComputed(w, computed, count, "lspace", prTextBufferView(&value));
            prTextBufferReset(&value);
        }
        if (node.rspace != dictionary.rspace) {
            PrTextBuffer value{};
            appendMu(&value, node.rspace);
            addComputed(w, computed, count, "rspace", prTextBufferView(&value));
            prTextBufferReset(&value);
        }
    }
    if (node.stretchy != TexTri_Any && (node.stretchy == TexTri_True) != hasFlag(dictionary, TexOperatorFlag_Stretchy)) {
        // An operator that TeX does not stretch is marked only if it would otherwise stretch, which is a block-axis
        // operator in a row or an inline-axis operator in <munder>, <mover> or <munderover>.
        uint32_t codePoint = 0;
        if (text.size != 0)
            prUtf8Decode(text, 0, &codePoint);
        bool inline_ = texIsInlineAxis(codePoint);
        bool stretches = !inline_ || (node.flags & TexNodeFlag_UnderOver);
        if (node.stretchy == TexTri_True || stretches)
            addComputed(w, computed, count, "stretchy", node.stretchy == TexTri_True ? "true" : "false");
    }
    if (node.symmetric != TexTri_Any &&
            (node.symmetric == TexTri_True) != hasFlag(dictionary, TexOperatorFlag_Symmetric))
        addComputed(w, computed, count, "symmetric", node.symmetric == TexTri_True ? "true" : "false");
    if (node.largeop != TexTri_Any && (node.largeop == TexTri_True) != hasFlag(dictionary, TexOperatorFlag_LargeOp))
        addComputed(w, computed, count, "largeop", node.largeop == TexTri_True ? "true" : "false");
    if (node.movablelimits != TexTri_Any && (node.movablelimits == TexTri_True) !=
            hasFlag(dictionary, TexOperatorFlag_MovableLimits))
        addComputed(w, computed, count, "movablelimits", node.movablelimits == TexTri_True ? "true" : "false");
}

void writeNode(Writer* w, TexNodeIndex index) {
    const TexTree* tree = w->tree;
    const TexNode& node = tree->nodes.data[index];
    PrStringView name = kElementNames[node.element];
    PrStringView text = texNodeText(tree, index);

    Computed computed[kMaxAttributes];
    uint32_t computedCount = 0;
    prTextBufferClear(&w->values);
    if (node.element == TexElement_Mo)
        addOperatorAttributes(w, node, text, computed, &computedCount);

    PrTexAttribute attributes[kMaxAttributes];
    uint32_t count = 0;
    for (uint32_t i = 0; i < computedCount; ++i) {
        PrStringView value{w->values.data + computed[i].start, computed[i].size};
        attributes[count++] = PrTexAttribute{computed[i].name, computed[i].size != 0 ? value : PrStringView{""}};
    }
    for (uint32_t a = node.firstAttribute; a != kTexNodeNone && count < kMaxAttributes;
            a = tree->attributes.data[a].next) {
        const TexAttributeRecord& record = tree->attributes.data[a];
        attributes[count++] = PrTexAttribute{record.name, texAttributeValue(tree, record)};
    }

    PrTexEvent event{};
    event.kind = PrTexEventKind_StartTag;
    event.name = name;
    event.attributes = count != 0 ? attributes : nullptr;
    event.attributeCount = count;
    w->fn(w->context, &event);
    if (isToken(node.element) && text.size != 0) {
        PrTexEvent characters{};
        characters.kind = PrTexEventKind_Text;
        characters.text = text;
        w->fn(w->context, &characters);
    }
    for (TexNodeIndex child = node.firstChild; child != kTexNodeNone; child = tree->nodes.data[child].next)
        writeNode(w, child);
    PrTexEvent end{};
    end.kind = PrTexEventKind_EndTag;
    end.name = name;
    w->fn(w->context, &end);
}

void escape(PrTextBuffer* out, PrStringView text, bool attribute) {
    size_t run = 0;
    for (size_t i = 0; i < text.size; ++i) {
        const char* reference = nullptr;
        switch (text.data[i]) {
            case '&':
                reference = "&amp;";
                break;
            case '<':
                reference = "&lt;";
                break;
            case '>':
                reference = "&gt;";
                break;
            case '"':
                if (!attribute)
                    continue;
                reference = "&quot;";
                break;
            default:
                continue;
        }
        prTextBufferAppend(out, PrStringView{text.data + run, i - run});
        prTextBufferAppend(out, reference);
        run = i + 1;
    }
    prTextBufferAppend(out, PrStringView{text.data + run, text.size - run});
}

void appendEvent(PrTextBuffer* out, const PrTexEvent* event) {
    switch (event->kind) {
        case PrTexEventKind_StartTag:
            prTextBufferAppendChar(out, '<');
            prTextBufferAppend(out, event->name);
            for (uint32_t i = 0; i < event->attributeCount; ++i) {
                prTextBufferAppendChar(out, ' ');
                prTextBufferAppend(out, event->attributes[i].name);
                prTextBufferAppend(out, "=\"");
                escape(out, event->attributes[i].value, true);
                prTextBufferAppendChar(out, '"');
            }
            prTextBufferAppendChar(out, '>');
            break;
        case PrTexEventKind_EndTag:
            prTextBufferAppend(out, "</");
            prTextBufferAppend(out, event->name);
            prTextBufferAppendChar(out, '>');
            break;
        case PrTexEventKind_Text:
            escape(out, event->text, false);
            break;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTexDocument : TexDocument {};

PrTexDocument* prTexDocumentCreate(void) {
    return allocZeroed<PrTexDocument>();
}

void prTexDocumentDestroy(PrTexDocument* PR_NULLABLE document) {
    if (document == nullptr)
        return;
    texDocumentReset(document);
    prMemoryFree(document);
}

void prTexDocumentClear(PrTexDocument* document) {
    texDocumentClear(document);
}

void prTexDocumentDefineMacro(PrTexDocument* document, PrStringView name, uint32_t parameterCount,
        PrStringView replacement) {
    PR_ASSERT(parameterCount <= 9);
    texMacroTableDefine(&document->macros, name, parameterCount, replacement, nullptr, PrStringView{});
}

uint32_t prTexDocumentMacroCount(const PrTexDocument* document) {
    return document->macros.macros.count;
}

uint32_t prTexDocumentEquationCount(const PrTexDocument* document) {
    return document->equationCount;
}

PrStringView prTexDocumentLabel(const PrTexDocument* document, PrStringView label) {
    return texDocumentLabel(document, label);
}

PrTexOptions prTexOptionsDefault(void) {
    PrTexOptions options{};
    options.display = PrTexDisplay_Inline;
    options.annotate = true;
    return options;
}

void prTexWriteMathMl(PrStringView source, const PrTexOptions* PR_NULLABLE options, PrTexEventFn fn, void* PR_NULLABLE context) {
    PrTexOptions used = options != nullptr ? *options : prTexOptionsDefault();
    TexTree tree{};
    tree.root = kTexNodeNone;
    texParse(&tree, source, used.display, used.annotate, used.document);
    texTreeSpace(&tree);
    Writer w{};
    w.tree = &tree;
    w.fn = fn;
    w.context = context;
    writeNode(&w, tree.root);
    prTextBufferReset(&w.values);
    texTreeReset(&tree);
}

void prTexAppendMathMl(PrStringView source, const PrTexOptions* PR_NULLABLE options, PrRawString* out) {
    PrTextBuffer text{};
    prTexWriteMathMl(source, options, [](void* PR_NULLABLE context, const PrTexEvent* event) {
        appendEvent(static_cast<PrTextBuffer*>(context), event);
    }, &text);
    prRawStringAppend(out, prTextBufferView(&text));
    prTextBufferReset(&text);
}

void prTexEventAppendText(const PrTexEvent* event, PrRawString* out) {
    PrTextBuffer text{};
    appendEvent(&text, event);
    prRawStringAppend(out, prTextBufferView(&text));
    prTextBufferReset(&text);
}
