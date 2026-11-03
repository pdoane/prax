#include "pr/regex.h"

int prTestRegexCApi(void);

// The public header compiles as C, and a pattern compiles and matches.
int prTestRegexCApi(void) {
    PrRegex* regex = 0;
    if (prRegexCreate(PR_SV("^a+$"), &regex) != PrStatus_Ok)
        return 1;
    bool matched = prRegexMatch(regex, PR_SV("aaa"), 0, 0);
    prRegexDestroy(regex);
    return matched ? 0 : 1;
}
