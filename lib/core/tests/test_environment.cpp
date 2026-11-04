#include "pr/environment.h"

#include "tools/harness/test.h"

TEST_CASE("an environment variable reads as its value, and one that is not set as the empty string") {
    PrRawString path = prEnvironmentVariable("PATH");
    CHECK(!prRawStringIsEmpty(&path));
    prRawStringReset(&path);

    PrRawString missing = prEnvironmentVariable("PRAX_TEST_VARIABLE_THAT_IS_NOT_SET");
    CHECK(prRawStringIsEmpty(&missing));
    prRawStringReset(&missing);

    PrRawString unnamed = prEnvironmentVariable("");
    CHECK(prRawStringIsEmpty(&unnamed));
    prRawStringReset(&unnamed);
}
