#include "pr/number_list.h"

#include "doctest/doctest.h"

using namespace pr_impl;

TEST_CASE("a number reads past white space, with a sign, a fraction and an exponent") {
    size_t pos = 0;
    float value = 0;
    REQUIRE(numberListRead(" -1.5e2", &pos, &value));
    CHECK(value == -150.0f);
    CHECK(pos == 7);
    pos = 0;
    REQUIRE(numberListRead(".5", &pos, &value));
    CHECK(value == 0.5f);
}

TEST_CASE("a number ends where its grammar does, so two can touch") {
    size_t pos = 0;
    float first = 0;
    float second = 0;
    REQUIRE(numberListRead("1.5.5", &pos, &first));
    REQUIRE(numberListRead("1.5.5", &pos, &second));
    CHECK(first == 1.5f);
    CHECK(second == 0.5f);
}

TEST_CASE("no number leaves the position where it was") {
    size_t pos = 1;
    float value = 7;
    CHECK_FALSE(numberListRead("a-.e", &pos, &value));
    CHECK(pos == 1);
    CHECK(value == 7.0f);
}

TEST_CASE("an exponent without digits is not part of the number") {
    size_t pos = 0;
    float value = 0;
    REQUIRE(numberListRead("2e", &pos, &value));
    CHECK(value == 2.0f);
    CHECK(pos == 1);
}

TEST_CASE("a separator is white space with at most one comma") {
    size_t pos = 0;
    numberListSkipSeparator(" , 3", &pos);
    CHECK(pos == 3);
    pos = 0;
    numberListSkipSeparator(",,", &pos);
    CHECK(pos == 1);
}
