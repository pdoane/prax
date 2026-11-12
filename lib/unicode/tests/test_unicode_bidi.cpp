#include "pr/unicode_bidi.h"

#include "tools/harness/test.h"

TEST_CASE("the strong type of a code point is left, right or none") {
    CHECK(prUnicodeBidiStrong('A') == PrUnicodeBidiStrong_Left);
    CHECK(prUnicodeBidiStrong('1') == PrUnicodeBidiStrong_None);
    CHECK(prUnicodeBidiStrong(0x05EA) == PrUnicodeBidiStrong_Right);  // HEBREW LETTER TAV
    CHECK(prUnicodeBidiStrong(0x0627) == PrUnicodeBidiStrong_Right);  // ARABIC LETTER ALEF, which is AL
    CHECK(prUnicodeBidiStrong(0x0660) == PrUnicodeBidiStrong_None);   // ARABIC-INDIC DIGIT ZERO, which is AN
    CHECK(prUnicodeBidiStrong(0x4E00) == PrUnicodeBidiStrong_Left);
    CHECK(prUnicodeBidiStrong(0x05FF) == PrUnicodeBidiStrong_Right);  // unassigned in a right-to-left block
    CHECK(prUnicodeBidiStrong(0x10FFFF) == PrUnicodeBidiStrong_None);
}

TEST_CASE("a mirrored character's glyph is its pair's, and others mirror to themselves") {
    CHECK(prUnicodeBidiMirror('(') == ')');
    CHECK(prUnicodeBidiMirror(')') == '(');
    CHECK(prUnicodeBidiMirror(0x2208) == 0x220B);
    CHECK(prUnicodeBidiMirror(0x27E8) == 0x27E9);
    CHECK(prUnicodeBidiMirror(0x221A) == 0x221A);
    CHECK(prUnicodeBidiMirror('x') == 'x');
    CHECK(prUnicodeBidiIsMirrored('('));
    CHECK(prUnicodeBidiIsMirrored(0x2211));
    CHECK(prUnicodeBidiIsMirrored(0x221A));
    CHECK_FALSE(prUnicodeBidiIsMirrored(0x2192));
    CHECK_FALSE(prUnicodeBidiIsMirrored('x'));
}

TEST_CASE("the first strong code point of a text decides its direction") {
    CHECK(prUnicodeBidiFirstStrong("") == PrUnicodeBidiStrong_None);
    CHECK(prUnicodeBidiFirstStrong("12 -") == PrUnicodeBidiStrong_None);
    CHECK(prUnicodeBidiFirstStrong("12 \xD7\xAA abc") == PrUnicodeBidiStrong_Right);
    CHECK(prUnicodeBidiFirstStrong("\xC3\xA9 \xD7\xAA") == PrUnicodeBidiStrong_Left);
    CHECK(prUnicodeBidiFirstStrong("\xFF\xD7\xAA") == PrUnicodeBidiStrong_Right);
}

