// The word guess game's rules: how a guess is marked, how a game takes guesses, and its word lists.

#include "pr/string.h"

#include "examples/word_guess/word_guess_game.h"
#include "examples/word_guess/word_list.h"

#include "tools/harness/test.h"

using namespace pr;
using namespace word_guess;

namespace {

// Returns the marks of `guess` against `answer`, one letter of "-?+" for each of Absent, Present and Correct.
String marks(const char* guess, const char* answer) {
    Mark out[kWordLength];
    markGuess(guess, answer, out);
    constexpr char kSymbols[] = "?-?+";
    char text[kWordLength];
    for (uint32_t i = 0; i < kWordLength; ++i)
        text[i] = kSymbols[out[i]];
    return String(PrStringView{text, kWordLength});
}

} // namespace

TEST_CASE("a guess marks each letter correct, present elsewhere, or absent") {
    CHECK(marks("CRANE", "CRANE") == "+++++");
    CHECK(marks("PLATE", "SCOPE") == "?---+");
    CHECK(marks("SPINE", "SCOPE") == "+?--+");
}

TEST_CASE("a repeated letter of a guess is marked only as often as the answer holds it, correct places first") {
    // THEME has two Es: the correct E takes one, and the first other E of the guess takes the other.
    CHECK(marks("EERIE", "THEME") == "?---+");
    // ALOFT has one L, which the guess's second L takes in place, so the first is absent.
    CHECK(marks("LLAMA", "ALOFT") == "-+?--");
    CHECK(marks("SPEED", "ABIDE") == "--?-?");
}

TEST_CASE("a game accepts only whole words from its lists, and ends when won or out of guesses") {
    Game game;
    startGame(game, 0);
    PrStringView answer = answerAt(0);
    for (uint32_t i = 0; i < 3; ++i)
        typeLetter(game, 'Z', 0);
    CHECK(submitGuess(game, 1) == Submit_TooShort);
    typeLetter(game, 'Z', 0);
    typeLetter(game, 'Z', 0);
    CHECK(submitGuess(game, 1) == Submit_NotWord);
    CHECK(game.guessCount == 0);
    for (uint32_t i = 0; i < kWordLength; ++i)
        eraseLetter(game);
    for (uint32_t i = 0; i < kWordLength; ++i)
        typeLetter(game, char(answer.data[i] - 'a' + 'A'), 0);
    CHECK(submitGuess(game, 2) == Submit_Accepted);
    CHECK(game.outcome == Outcome_Won);
    CHECK(letterMark(game, game.answer[0]) == Mark_Correct);
    typeLetter(game, 'A', 3);
    CHECK(game.typed == 0);
}

TEST_CASE("every word of the lists is five lowercase letters") {
    const PrStringView lists[] = {kAnswers, kGuesses};
    for (PrStringView list : lists) {
        CHECK(list.size % (kWordLength + 1) == kWordLength);
        for (uint32_t at = 0; at < list.size; ++at) {
            bool space = at % (kWordLength + 1) == kWordLength;
            CHECK((space ? list.data[at] == ' ' : list.data[at] >= 'a' && list.data[at] <= 'z'));
        }
    }
}
