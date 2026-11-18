// The word guess game's view: the board, the keyboard and the game's messages, built with lib/ui's elements
// (pr/ui_element.h).

#pragma once

#include "pr/ui_element.h"

#include "examples/word_guess/word_guess_game.h"

PR_ASSUME_NONNULL_BEGIN

using namespace pr;

namespace word_guess {

// The game and what the view keeps between rounds.
struct WordGuessApp {
    Game game;
    bool started;
    uint32_t next;              // the word list's index of the next game's answer
};

// Declares a round of the view under `root`, which fills the window, and plays the round's input.
void buildWordGuess(WordGuessApp& app, UiBuilder& ui, PrLayoutNode root);

} // namespace word_guess

PR_ASSUME_NONNULL_END
