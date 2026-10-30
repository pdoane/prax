// The public API compiled as C, the language it is for. A header that C cannot parse, or a declaration that C cannot
// link against, fails the build here rather than in someone's project.

#include "pr/base.h"
#include "pr/color.h"
#include "pr/css.h"
#include "pr/dom_abort.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/utf8.h"

PR_STATIC_ASSERT(sizeof(PrCssStyleDeclaration*) == sizeof(void*));
PR_STATIC_ASSERT(PrCssRuleType_Style == 1);

// The first check that failed, or the empty string if every check holds.
PrStringView prTestCApi(PrCssStyleDeclaration* style);

// Returns `failed` if a check before this one failed, and otherwise `name` if `holds` is false.
static PrStringView check(PrStringView failed, bool holds, const char* name) {
    return failed.size == 0 && !holds ? PR_SV(name) : failed;
}

// A sheet that a caller creates and releases.
static PrStringView constructed(PrStringView failed) {
    PrCssStyleSheetInit options = prCssStyleSheetInitDefault();
    PrCssStyleSheet* sheet;

    options.baseUrl = PR_SV("x");
    sheet = prCssStyleSheetCreate(&options);
    prCssStyleSheetReplaceSync(sheet, PR_SV("a {} b {}"));

    failed = check(failed, prCssRuleListLength(prCssStyleSheetCssRules(sheet)) == 2,
        "a constructed sheet holds the two rules it was replaced with");
    prCssStyleSheetRelease(sheet);
    prCssStyleSheetRelease(NULL);
    return failed;
}

// The declaration API, called on a handle that lib/css/tests/test_css.cpp passes in.
static PrStringView declarations(PrStringView failed, PrCssStyleDeclaration* style) {
    PrCssDeclarationName color = prCssDeclarationNameFromProperty(PrCssProperty_Color);
    PrCssDeclarationName width = prCssDeclarationNameFromProperty(PrCssProperty_Width);
    PrString value = prCssStyleDeclarationGetPropertyValue(style, &color);
    PrString removed = prStringNull();

    failed = check(failed, prCssStyleDeclarationLength(style) == 1, "the declarations hold one property");
    failed = check(failed, prStringView(&value).size == 3, "color is red");

    prCssStyleDeclarationSetProperty(style, &width, PR_SV("1px"), false);
    failed = check(failed, prCssStyleDeclarationLength(style) == 2, "setting width adds a property");

    prCssStyleDeclarationRemoveProperty(style, &width, &removed);
    failed = check(failed, prStringView(&removed).size == 3, "removing width answers 1px");

    prStringReset(&value);
    prStringReset(&removed);
    return failed;
}

PrStringView prTestCApi(PrCssStyleDeclaration* style) {
    PrString text = prStringCopy(PR_SV("color"));
    PrString copy = prStringClone(&text);
    PrString none = prStringNull();
    PrColor color = prColorFromColor8((PrColor8){0x66, 0x33, 0x99, 0xff});
    PrStringView failed = PR_SV("");

    char encoded[kPrUtf8MaxBytes];
    uint32_t size = prUtf8Encode(0x2603, encoded);

    prStringReplace(&copy, PR_SV(""));

    failed = check(failed, prStringView(&text).size == 5, "a copy of \"color\" is 5 bytes");
    failed = check(failed, size == 3, "a snowman encodes to 3 bytes");
    failed = check(failed, prStringIsEmpty(&copy), "a string replaced with the empty one is empty");
    failed = check(failed, prStringIsNull(&none), "the null string is null");
    failed = check(failed, prStatusName(PrStatus_OutOfRange).size == 10, "a status is named for itself");
    failed = check(failed, (uint32_t)(color.b * 4.0f) == 2, "an 8-bit channel is a fraction of 255");
    failed = declarations(failed, style);
    failed = constructed(failed);

    prStringReset(&text);
    prStringReset(&copy);
    prStringReset(&none);
    return failed;
}
