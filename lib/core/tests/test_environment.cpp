#include "pr/environment.h"
#include "pr/string_ops.h"

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

TEST_CASE("a variable that is set reads as its value, and one that is removed reads as not set") {
    CHECK(prEnvironmentSetVariable("PRAX_TEST_VARIABLE_SET", "one two"));
    PrRawString value = prEnvironmentVariable("PRAX_TEST_VARIABLE_SET");
    CHECK(prRawStringView(&value) == "one two");
    prRawStringReset(&value);
    CHECK(prEnvironmentRemoveVariable("PRAX_TEST_VARIABLE_SET"));
    value = prEnvironmentVariable("PRAX_TEST_VARIABLE_SET");
    CHECK(prRawStringIsEmpty(&value));
    prRawStringReset(&value);
    CHECK(prEnvironmentRemoveVariable("PRAX_TEST_VARIABLE_SET"));

    CHECK(!prEnvironmentSetVariable("", "value"));
    CHECK(!prEnvironmentSetVariable("PRAX=TEST", "value"));
    CHECK(!prEnvironmentSetVariable("PRAX_TEST_VARIABLE_SET", PrStringView{"a\0b", 3}));
    CHECK(!prEnvironmentRemoveVariable("PRAX=TEST"));
}

TEST_CASE("the host name is the machine's name, which has text and no NUL") {
    PrRawString name = prEnvironmentHostName();
    PrStringView text = prRawStringView(&name);
    CHECK(text.size != 0);
    CHECK(prStringFindByte(text, '\0') == text.size);
    prRawStringReset(&name);
}
