#include "pr/webidl.h"

#include "tools/harness/test.h"

TEST_CASE("webidl/#dfn-error-names-table gives a code to the names it has one for") {
    struct Case {
        PrStringView name;
        PrWebIdlErrorCode code;
    };
    const Case cases[] = {
        {"IndexSizeError", PrWebIdlErrorCode_IndexSizeErr},
        {"SyntaxError", PrWebIdlErrorCode_SyntaxErr},
        {"InUseAttributeError", PrWebIdlErrorCode_InuseAttributeErr},
        {"URLMismatchError", PrWebIdlErrorCode_UrlMismatchErr},
        {"DataCloneError", PrWebIdlErrorCode_DataCloneErr},
        {"NotAllowedError", 0},   // a name with no legacy code
        {"syntaxerror", 0},       // names match exactly
        {"Error", 0},             // the constructor's default name
        {"", 0},
    };
    for (const Case& c : cases)
        CHECK_MESSAGE(prWebIdlErrorCode(c.name) == c.code, c.name);
}
