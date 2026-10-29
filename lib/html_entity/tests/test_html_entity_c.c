#include "pr/html_entity.h"

int prTestEntityCApi(void);

// The public headers compile as C. A reference expands, and an unknown name does not.
int prTestEntityCApi(void) {
    int result = prHtmlEntityMatch(PR_SV("amp;")).size != 1;
    result |= prHtmlEntityMatch(PR_SV("nosuchname;")).data != NULL;
    result |= (int)kPrHtmlEntityMaxLegacyLength > (int)kPrHtmlEntityMaxLength;
    return result;
}
