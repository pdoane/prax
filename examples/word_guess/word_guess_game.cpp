// The word guess game's rules.

#include "examples/word_guess/word_guess_game.h"

#include "pr/mem.h"

#include "examples/word_guess/word_list.h"

namespace word_guess {

void markGuess(const char* guess, const char* answer, Mark* out) {
    // The answer's letters that no Correct letter of the guess took, by letter.
    uint8_t left[26] = {};
    for (uint32_t i = 0; i < kWordLength; ++i) {
        if (guess[i] == answer[i]) {
            out[i] = Mark_Correct;
        } else {
            out[i] = Mark_Absent;
            ++left[answer[i] - 'A'];
        }
    }
    for (uint32_t i = 0; i < kWordLength; ++i) {
        if (out[i] == Mark_Correct)
            continue;
        uint8_t& count = left[guess[i] - 'A'];
        if (count > 0) {
            out[i] = Mark_Present;
            --count;
        }
    }
}

void startGame(Game& game, uint32_t index) {
    game = Game{.rejectedAt = -1};
    PrStringView answer = answerAt(index % answerCount());
    for (uint32_t i = 0; i < kWordLength; ++i)
        game.answer[i] = char(answer.data[i] - 'a' + 'A');
    game.submittedAt = -1;
}

void typeLetter(Game& game, char letter, double time) {
    if (game.outcome != Outcome_Playing || game.typed == kWordLength)
        return;
    game.typing[game.typed] = letter;
    game.typedAt[game.typed] = time;
    ++game.typed;
}

void eraseLetter(Game& game) {
    if (game.outcome == Outcome_Playing && game.typed > 0)
        --game.typed;
}

Submit submitGuess(Game& game, double time) {
    Submit result = Submit_Accepted;
    if (game.outcome != Outcome_Playing)
        result = Submit_Over;
    else if (game.typed < kWordLength)
        result = Submit_TooShort;
    else if (!isWord(game.typing))
        result = Submit_NotWord;
    if (result != Submit_Accepted) {
        if (result != Submit_Over) {
            game.rejectedAt = time;
            game.rejection = result;
        }
        return result;
    }
    uint32_t row = game.guessCount++;
    prMemCopy(game.guesses[row], game.typing, kWordLength);
    markGuess(game.typing, game.answer, game.marks[row]);
    game.typed = 0;
    game.submittedAt = time;
    if (prMemEquals(game.typing, game.answer, kWordLength))
        game.outcome = Outcome_Won;
    else if (game.guessCount == kMaxGuesses)
        game.outcome = Outcome_Lost;
    return result;
}

Mark letterMark(const Game& game, char letter) {
    Mark best = Mark_Unknown;
    for (uint32_t row = 0; row < game.guessCount; ++row) {
        for (uint32_t i = 0; i < kWordLength; ++i) {
            if (game.guesses[row][i] == letter && game.marks[row][i] > best)
                best = game.marks[row][i];
        }
    }
    return best;
}

uint32_t answerCount() {
    return kAnswers.size / (kWordLength + 1);
}

PrStringView answerAt(uint32_t index) {
    return PrStringView{kAnswers.data + index * (kWordLength + 1), kWordLength};
}

bool isWord(const char* letters) {
    char lower[kWordLength];
    for (uint32_t i = 0; i < kWordLength; ++i)
        lower[i] = char(letters[i] - 'A' + 'a');
    const PrStringView lists[] = {kAnswers, kGuesses};
    for (PrStringView list : lists) {
        for (uint32_t at = 0; at + kWordLength <= list.size; at += kWordLength + 1) {
            if (prMemEquals(list.data + at, lower, kWordLength))
                return true;
        }
    }
    return false;
}

} // namespace word_guess
