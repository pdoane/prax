// The word guess game's rules: six guesses at a hidden five-letter word, each marked letter by letter.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace word_guess {

// The letters of a word, and the guesses of a game.
constexpr uint32_t kWordLength = 5;
constexpr uint32_t kMaxGuesses = 6;

// What a guess says of one of its letters: in the word at that place, in the word elsewhere, or not in it. A letter of
// the keyboard that no guess has used is Unknown.
enum {
    Mark_Unknown,
    Mark_Absent,
    Mark_Present,
    Mark_Correct,
};
typedef uint8_t Mark;

// Where a game stands.
enum {
    Outcome_Playing,
    Outcome_Won,
    Outcome_Lost,
};
typedef uint8_t Outcome;

// What became of a submitted guess.
enum {
    Submit_Accepted,
    Submit_TooShort,
    Submit_NotWord,
    Submit_Over,
};
typedef uint8_t Submit;

// A game: the hidden word, the guesses submitted, the letters of the guess being typed, and when each animation that
// the view shows began, in seconds on the clock that the caller passes.
struct Game {
    char answer[kWordLength];
    char guesses[kMaxGuesses][kWordLength];
    Mark marks[kMaxGuesses][kWordLength];
    uint32_t guessCount;
    char typing[kWordLength];
    uint32_t typed;
    Outcome outcome;
    double typedAt[kWordLength];    // when each letter of the guess being typed was typed
    double submittedAt;             // when the last guess was accepted
    double rejectedAt;              // when the last guess was rejected, or a negative time for never
    Submit rejection;               // why it was rejected
};

// Marks `guess` against `answer`. A letter that the answer holds n times marks at most n of the guess's letters Correct
// or Present, the Correct ones first, then from the left.
void markGuess(const char* guess, const char* answer, Mark* out);

// Starts a game whose answer is the word list's word `index`, wrapping.
void startGame(Game& game, uint32_t index);

// Types a letter, A to Z, or erases the last, while the game is played.
void typeLetter(Game& game, char letter, double time);
void eraseLetter(Game& game);

// Submits the guess being typed.
Submit submitGuess(Game& game, double time);

// The best mark that a letter has had in any guess, for the keyboard.
Mark letterMark(const Game& game, char letter);

// The words that the game hides, and whether a word is one that it accepts as a guess.
uint32_t answerCount();
PrStringView answerAt(uint32_t index);
bool isWord(const char* letters);

} // namespace word_guess

PR_ASSUME_NONNULL_END
