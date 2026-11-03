// CSS unit tests
//
// The helpers that the CSS unit tests share. Text is a std::string, which doctest can print and compare.

#pragma once

#include "pr/css.h"
#include "pr/string_ops.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

#include "css/src/css_object.h"
#include "css/src/css_syntax.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the text of a view or a string. The null view reads as the empty string.
inline std::string textOf(PrStringView text) {
    return std::string(text.size != 0 ? text.data : "", text.size);
}
inline std::string textOf(const PrRawString& string) {
    return textOf(prRawStringView(&string));
}

// Returns the text of a returned string, which is then released.
inline std::string take(PrRawString string) {
    std::string text = textOf(string);
    prRawStringReset(&string);
    return text;
}

// Returns the text written to a buffer, which is then released.
inline std::string take(PrTextBuffer* buffer) {
    std::string text = textOf(prTextBufferView(buffer));
    prTextBufferReset(buffer);
    return text;
}

// A source cut into tokens, and a stream over them. The tokens are released with the test, and the source must outlive
// them.
struct CssTestSource {
    CssTokenScratch scratch = {};
    CssTokenArray tokens = {};
    CssTokenStream ts;

    explicit CssTestSource(PrStringView source, CssTokenizeFlags flags = 0) {
        cssTokenize(&tokens, &scratch, source, flags);
        ts = cssStream(&tokens);
    }

    ~CssTestSource() {
        cssTokenArrayReset(&tokens);
        cssTokenScratchReset(&scratch);
    }

    CssTestSource(const CssTestSource&) = delete;
    CssTestSource& operator=(const CssTestSource&) = delete;

    // Return the number of tokens, and the kind and flags of the token at `index`.
    uint32_t count() const { return cssTokenCount(&tokens); }
    CssTokenKind kind(uint32_t index) const { return cssTokenKind(&tokens, index); }
    CssTokenFlags flags(uint32_t index) const { return cssTokenFlags(&tokens, index); }

    // Returns the bytes that the token at `index` covers.
    std::string text(uint32_t index) const { return textOf(cssTokenText(&tokens, index)); }

    // Returns the bytes that `span` covers.
    std::string text(CssTokenSpan span) const {
        return span.count != 0 ? textOf(cssSpanText(&tokens, span)) : std::string();
    }

    // Returns the bytes that each token covers, in order.
    std::vector<std::string> all() const {
        std::vector<std::string> out;
        for (uint32_t i = 0; i < count(); ++i)
            out.push_back(text(i));
        return out;
    }

    // Returns the kind of each token, in order.
    std::vector<CssTokenKind> kinds() const {
        std::vector<CssTokenKind> out;
        for (uint32_t i = 0; i < count(); ++i)
            out.push_back(kind(i));
        return out;
    }
};

// A constructed sheet parsed from `text`, released with the test.
struct CssTestSheet {
    PrCssStyleSheet* sheet;

    explicit CssTestSheet(PrStringView text) : sheet(cssStyleSheetCreate(cssSheetDataCreate())) {
        cssStyleSheetRetain(sheet);
        sheet->constructed = true;
        cssStyleSheetReplace(sheet, text);
    }

    ~CssTestSheet() { cssStyleSheetRelease(sheet); }

    CssTestSheet(const CssTestSheet&) = delete;
    CssTestSheet& operator=(const CssTestSheet&) = delete;

    // Returns the number of top-level rules.
    uint32_t count() const { return sheet->data->rules.count; }

    // Return the data of the rule at `index`, and the object that presents it.
    CssRuleData* rule(uint32_t index) const { return sheet->data->rules.data[index]; }
    PrCssRule* object(uint32_t index) const { return cssStyleSheetRule(sheet, index); }

    // Returns the sheet's public handle.
    PrCssStyleSheet* handle() const { return sheet; }
};

// Return the declaration name that `text` names in `block`, or in a block of properties. The test requires that the
// block accepts the name.
inline CssDeclaredName named(const CssDeclarationBlock* block, PrStringView text) {
    CssDeclaredName name = {};
    REQUIRE(cssNameFromText(block->rule, text, &name));
    return name;
}
inline CssDeclaredName named(PrStringView text) {
    CssDeclaredName name = {};
    REQUIRE(cssNameFromText(CssAtRule_None, text, &name));
    return name;
}

// The public form of a declaration's name, as `block` or a block of properties names it. It converts to the pointer
// that a call takes.
struct Named {
    PrCssDeclarationName name = prCssDeclarationNameFromProperty(PrCssProperty_Count);

    Named(const PrCssStyleDeclaration* block, PrStringView text) {
        REQUIRE(prCssStyleDeclarationNameFromText(block, text, &name));
    }
    explicit Named(PrStringView text) { REQUIRE(prCssPropertyNameFromText(text, &name)); }
    ~Named() { prCssDeclarationNameReset(&name); }

    Named(const Named&) = delete;
    Named& operator=(const Named&) = delete;

    using Pointer = const PrCssDeclarationName* PR_NONNULL;
    operator Pointer() const { return &name; }
};

} // namespace pr_impl

PR_ASSUME_NONNULL_END
