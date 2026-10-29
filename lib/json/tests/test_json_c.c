#include "pr/json.h"

int prTestJsonCApi(void);

// The public headers compile as C. An array's element reads back, and the array writes back.
int prTestJsonCApi(void) {
    PrJson* document = prJsonCreate();
    int result = !prJsonParse(document, PR_SV("[1, \"two\"]"), NULL);
    result |= prJsonKind(document, 0) != PrJsonKind_Array;
    result |= prJsonString(document, prJsonElement(document, 0, 1)).size != 3;
    result |= prJsonMember(document, 0, PR_SV("x")) != kPrJsonMissing;
    PrJsonWriter* writer = prJsonWriterCreate();
    prJsonWriterValue(writer, document, 0);
    result |= prJsonWriterText(writer).size != 9;
    prJsonWriterDestroy(writer);
    prJsonDestroy(document);
    return result;
}
